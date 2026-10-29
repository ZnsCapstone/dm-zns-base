// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: LSM-tree 기반 매핑 테이블로 랜덤 쓰기를 순차 쓰기로 바꿔주는
 * ZNS(zoned) DM 타깃.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/sort.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/crc32.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/ktime.h>

#include "skiplist.h"

#define DM_MSG_PREFIX "zns-base"

/* 선택적으로 수행하는 제한된 진단이다. 할당·매핑 정책은 변경하지 않는다. */
static unsigned int gc_diag_budget;
module_param(gc_diag_budget, uint, 0444);
MODULE_PARM_DESC(gc_diag_budget, "maximum live>used mapping audits per module load (0 disables)");
static atomic_t gc_diag_events = ATOMIC_INIT(0);

static bool metrics_enabled;
module_param(metrics_enabled, bool, 0444);
MODULE_PARM_DESC(metrics_enabled, "enable optional evaluation counters exposed in dmsetup status");
/* 진단 전용. 기본 경로에는 timestamp/atomic 갱신을 추가하지 않는다. */
static bool wal_timing_enabled;
module_param(wal_timing_enabled, bool, 0444);
MODULE_PARM_DESC(wal_timing_enabled, "enable foreground WAL timing diagnostics (requires metrics_enabled)");

enum wal_diag_counter {
	WD_DATA_COUNT, WD_DATA_NS, WD_BATCHES, WD_RECORDS, WD_QUEUE_NS,
	WD_IO_COUNT, WD_IO_NS, WD_IO_ERRORS, WD_COMMIT_COUNT, WD_COMMIT_NS,
	WD_ACK_NS, WD_BATCH_1, WD_BATCH_2_8, WD_BATCH_9_32, WD_BATCH_33_PLUS,
	WD_READY_COUNT, WD_READY_NS, WD_READY_SLOW,
	WD_WORKER_COUNT, WD_WORKER_NS, WD_WORKER_SLOW,
	WD_DISPATCH_COUNT, WD_DISPATCH_NS, WD_SUBMIT_COUNT, WD_SUBMIT_NS,
	WD_COMPLETE_COUNT, WD_COMPLETE_NS, WD_COMPLETE_SLOW,
	WD_COUNT
};

static const char * const wal_diag_names[WD_COUNT] = {
	"data_count", "data_ns", "batches", "records", "queue_ns",
	"io_count", "io_ns", "io_errors", "commit_count", "commit_ns",
	"ack_ns", "batch_1", "batch_2_8", "batch_9_32", "batch_33_plus",
	"ready_count", "ready_ns", "ready_slow",
	"worker_count", "worker_ns", "worker_slow",
	"dispatch_count", "dispatch_ns", "submit_count", "submit_ns",
	"complete_count", "complete_ns", "complete_slow"
};

/* foreground pending 요청을 process context에서 재시도한다. */
static struct workqueue_struct *zns_wq;

/* WAL durable commit는 pending flush/write 재시도와 실행 슬롯을 공유하지 않는다.
 * 배치 자체는 직렬 실행해 WAL 예약/기록/공개 순서를 유지한다. */
static struct workqueue_struct *zns_wal_wq;

/* bio 제출은 sleep할 수 있으므로 process context로 미룬다.
 * WAL worker가 제출 차례를 기다려도 선행 bio가 진행할 수 있도록 실행 큐를 분리한다. */
static struct workqueue_struct *zns_bio_wq;

/* compaction 대기가 bio 제출을 막지 않도록 실행 큐를 분리한다. */
static struct workqueue_struct *zns_compaction_wq;

/* GC와 compaction이 서로의 실행 슬롯을 점유한 채 대기하지 않도록 분리한다. */
static struct workqueue_struct *zns_gc_wq;

/* durable checkpoint가 보호하는 이전 WAL zone을 별도 워크큐에서 회수한다. */
static struct workqueue_struct *zns_wal_reclaim_wq;

/* 큰 frozen memtable 직렬화는 atomic 완료 콜백 대신 process context의 전용 워크큐에서 수행한다. */
static struct workqueue_struct *zns_flush_wq;

struct deferred_bio_work {
	struct work_struct work;
	struct bio *bio;
};

static void deferred_bio_submit_fn(struct work_struct *w)
{
	struct deferred_bio_work *dw = container_of(w, struct deferred_bio_work, work);

	submit_bio(dw->bio);
	kfree(dw);
}

/* atomic context에서도 호출되므로 후속 bio 제출을 워크큐에 맡긴다. */
static void submit_bio_deferred(struct bio *bio)
{
	struct deferred_bio_work *dw = kmalloc(sizeof(*dw), GFP_ATOMIC);

	if (!dw) {
		/* 할당 실패 시 직접 제출하는 기존 fallback을 유지한다. */
		submit_bio(bio);
		return;
	}
	INIT_WORK(&dw->work, deferred_bio_submit_fn);
	dw->bio = bio;
	queue_work(zns_bio_wq, &dw->work);
}

enum zone_tag {
	ZONE_TAG_FREE = 0,   /* 아직 아무도 안 쓰는 zone */
	ZONE_TAG_USER_DATA,
	ZONE_TAG_GC_DATA,
	ZONE_TAG_WAL,
	ZONE_TAG_SSTABLE,
	ZONE_TAG_COUNT,
};

#define ZONE_NONE UINT_MAX
#define BLOCK_SECTORS 8   /* 매핑 단위 = 4KB = 512B 섹터 8개 */

/* memtable 엔트리 수가 임계값에 도달하면 SSTable flush를 요청한다. */
static unsigned int flush_threshold = 1000000;
module_param(flush_threshold, uint, 0444);
MODULE_PARM_DESC(flush_threshold, "memtable entry count that triggers an SSTable flush");

/* live SSTable 수를 기준으로 k-way compaction을 시작한다. */
static unsigned int compaction_k = 4;
module_param(compaction_k, uint, 0444);
MODULE_PARM_DESC(compaction_k, "number of oldest SSTables merged per compaction run");

/* free zone 수가 임계값 이하이면 GC를 요청한다. */
static unsigned int gc_low_watermark = 4;
module_param(gc_low_watermark, uint, 0444);
MODULE_PARM_DESC(gc_low_watermark, "free zone count that starts background GC");

static unsigned int gc_high_watermark = 5;
module_param(gc_high_watermark, uint, 0444);
MODULE_PARM_DESC(gc_high_watermark, "free zone count at which background GC stops");

static inline unsigned int gc_stop_watermark(void)
{
	return max(gc_high_watermark, gc_low_watermark + 1);
}

/* 일반 할당이 GC 이주 공간을 소진하지 않도록 free zone을 예약한다.
 * reserve 경계의 차용과 metadata 회전은 별도 workspace 검사로 허용한다. */
static unsigned int gc_reserved_zones = 2;
module_param(gc_reserved_zones, uint, 0444);
MODULE_PARM_DESC(gc_reserved_zones, "free zones reserved exclusively for GC relocation");

static unsigned int wal_batch_max_records = 127;
module_param(wal_batch_max_records, uint, 0444);
MODULE_PARM_DESC(wal_batch_max_records, "maximum PUT records per 4KB WAL page");

static unsigned int wal_batch_delay_ms = 0;
module_param(wal_batch_delay_ms, uint, 0444);
MODULE_PARM_DESC(wal_batch_delay_ms, "maximum foreground WAL group-commit delay in ms");

/* 요청 기반 WAL 영속화와 압력별 GC는 기본 동작이다. 장치별 크기만 조절한다. */
static unsigned int data_append_max_blocks = 64;
module_param(data_append_max_blocks, uint, 0444);
MODULE_PARM_DESC(data_append_max_blocks, "maximum 4KiB blocks per data append (default 64)");

static unsigned int wal_group_pages = 8;
module_param(wal_group_pages, uint, 0444);
MODULE_PARM_DESC(wal_group_pages, "WAL pages per durability group (1..16, default 8)");

static unsigned int failpoint;
module_param(failpoint, uint, 0644);
MODULE_PARM_DESC(failpoint, "one-shot tests: 1=data completion, 2=WAL, 3=SSTable, 4=checkpoint, 5=reset");

static bool failpoint_hit(unsigned int point)
{
	return READ_ONCE(failpoint) == point && cmpxchg(&failpoint, point, 0) == point;
}

static unsigned int wal_group_capacity(void)
{
	return wal_batch_max_records * wal_group_pages;
}

/* 여유가 있으면 무효 비율 20%를 요구한다. reserve+1에서 active tail이 25% 이상 사용됐으면 10%로 낮춘다.
 * reserve 경계는 bootstrap 순이익 검사로 판단한다. */
static unsigned int gc_reclaim_threshold(unsigned int free, unsigned int reserve,
					 sector_t used, sector_t capacity)
{
	if (free <= reserve)
		return 0;
	if (free == reserve + 1 && capacity && used * 100 >= capacity * 25)
		return 10;
	return 20;
}

static sector_t append_extent_sectors(sector_t nr, unsigned int max_blocks,
				      unsigned int append_max, unsigned int io_max)
{
	sector_t limit = min_t(sector_t, min(append_max, io_max),
				(sector_t)max_blocks * BLOCK_SECTORS);

	limit -= limit % BLOCK_SECTORS;
	return min(nr, limit ? limit : (sector_t)BLOCK_SECTORS);
}

/* 회귀 테스트에서 WAL 공간 부족 경로를 결정적으로 재현하기 위한 fault
 * injection. 0(기본값)이면 실제 동작에는 아무 영향이 없다. */
static unsigned int wal_batch_alloc_failures;
module_param(wal_batch_alloc_failures, uint, 0444);
MODULE_PARM_DESC(wal_batch_alloc_failures, "number of foreground WAL allocations to fail for testing");

/* zone 할당과 제출 순서 관리 */
struct zone_pool {
	sector_t zone_sectors;
	unsigned int nr_zones;
	enum zone_tag *zone_tag; /* zone_tag[zone_id] — 이 zone이 지금 뭘로 쓰이는지 */
	sector_t *wp; /* zone 내부 예약 길이. 아직 완료되지 않은 쓰기를 포함 */
	sector_t *dispatch_wp; /* 제출·취소 게이트가 전진시킨 절대 섹터 주소 */
	struct list_head *dispatch_waiters; /* zone_id별: 아직 자기 차례가 안 된 쓰기 대기열 */
	unsigned int *invalid_count; /* zone별 무효 블록 추정 개수: GC 후보 선정에 사용 */
	unsigned int *sstable_live_count; /* zone에 남아 있는 live SSTable 참조 수 */
	unsigned int active_zone[ZONE_TAG_COUNT]; /* 태그별 현재 활성 zone */
	u64 *wal_gen; /* WAL zone에 한해서만 의미 있는 배정 순번(generation) — replay 순서 판정용 */
	u64 wal_next_gen; /* 다음 WAL zone에 부여할 generation (단조 증가, c->lock 하에 증가) */
	atomic_t *inflight_reads; /* zone_id별 진행 중인 읽기 수 — 회수(reset) 전 drain 대기용 */
	wait_queue_head_t reclaim_waitq; /* inflight_reads가 0이 되길 기다리는 회수 대기 큐 */
	bool *append_ready; /* zone header 쓰기가 완료되어 append를 받을 수 있음 */
	struct list_head *append_waiters; /* header 완료 전 대기하는 zone append bio */
};

/* 예약 범위의 제출 차례를 기다리는 항목이다. dispatch_wp가 phys에 도달하면 fire(arg)를 호출한다. */
struct dispatch_waiter {
	struct list_head link;
	sector_t phys;
	sector_t nr;
	void (*fire)(void *arg);
	void *arg;
};

struct append_waiter {
	struct list_head link;
	struct bio *bio;
};

struct zns_base_c {
	struct dm_dev *dev;
	atomic64_t metric_logical_write_bytes;
	atomic64_t metric_logical_read_bytes;
	atomic64_t metric_write_bytes[ZONE_TAG_COUNT];
	atomic64_t metric_gc_runs, metric_gc_ns, metric_gc_reclaims;
	atomic64_t metric_gc_moved_bytes;
	atomic64_t metric_compaction_runs, metric_compaction_ns;
	atomic64_t metric_compaction_input_bytes, metric_compaction_output_bytes;
	atomic64_t metric_gc_scratch_bytes, metric_compaction_scratch_bytes;
	atomic64_t metric_foreground_wait_ns, metric_foreground_wait_count;
	atomic64_t wal_diag[WD_COUNT];
	u64 diag_wal_ready_ns; /* c->lock 보호: idle·backlog 준비부터 다음 worker 진입까지의 시작 시각 */
	atomic64_t metric_flush_started_ns, metric_flush_runs, metric_flush_ns;
	atomic64_t metric_flush_write_bytes;
	u64 metric_mapping_peak_bytes; /* c->lock으로 보호하는 표본 최대값 */

	sector_t nr_sectors;
	struct zone_pool *zp;
	struct skiplist *memtable; /* LBA -> 물리 주소 매핑 */
	/* flush는 하나씩 수행하며 SSTable 공개 전까지 frozen map을 읽기에 유지한다. */
	struct skiplist *frozen_memtable;
	bool frozen_published;
	bool checkpoint_failed;
	bool metadata_failed;
	u64 read_view_epoch;
	atomic64_t sstable_probe_ios;
	atomic64_t sstable_probe_reuses;
	u64 next_seq_no; /* 다음 flush에 붙일 SSTable 세대 번호 */

	struct sstable_info *sstables; /* live SSTable 색인. 등록 시 확장하고 제거 시 마지막 항목과 교체 */
	unsigned int nr_sstables;
	unsigned int sstables_cap; /* sstables 배열의 현재 용량 */

	struct work_struct compaction_work; /* compaction_wq에 큐잉되는 백그라운드 작업 */
	struct work_struct gc_work; /* gc_wq에 큐잉되는 백그라운드 작업 */
	struct work_struct wal_reclaim_work; /* wal_reclaim_wq에 큐잉되는 백그라운드 작업 */
	struct delayed_work wal_rotate_work; /* reserve 경계의 WAL 회전과 checkpoint */
	struct delayed_work wal_batch_work;
	struct list_head wal_pending;
	unsigned int wal_pending_count;
	unsigned int wal_pending_buffered_count;
	unsigned int wal_buffered_count; /* pending + worker가 보유한 조기 완료 PUT */
	u64 wal_buffered_acks;
	u64 wal_buffered_commits;
	u64 wal_buffered_errors;
	bool wal_batch_busy;
	bool wal_batch_has_waiters;
	struct delayed_work pending_flush_work;
	struct list_head pending_flush_bios;
	struct delayed_work pending_write_work;
	struct list_head pending_write_bios;
	bool stopping;
	atomic_t foreground_writes;

	/* 발행된 checkpoint가 모두 완료될 때 durable frontier를 전진시킨다.
	 * 완료 순서가 바뀌어도 아직 보호되지 않은 WAL 세대를 먼저 회수하면 안 된다. */
	int wal_ckpt_inflight; /* 아직 durable 안 된 체크포인트 개수 */
	u64 wal_highest_issued_split_gen; /* 지금까지 발행된 체크포인트 중 최대 split_gen */
	u64 wal_durable_split_gen; /* 이 값보다 작은 gen의 WAL zone은 회수 가능 */
	wait_queue_head_t flush_waitq; /* dtr에서 비동기 flush chain drain 대기 */
	bool gc_active; /* GC latest-map 구축/이주 중 memtable swap 금지 */
	bool gc_foreground_pause; /* victim 이주 전에 새 foreground 할당을 보류 */
	bool sstable_rotation_pending; /* compaction이 reserve 경계 metadata 회전을 소유 */
	sector_t gc_wal_budget; /* 현재 victim에 남은 WAL 섹터 예산 */
	sector_t gc_wal_reserve; /* foreground가 reserve를 빌린 동안 보호할 WAL floor */
	bool wal_rotation_pending; /* 이전 WAL을 drain하는 동안 새 매핑 보류 */
	bool wal_rotation_credit; /* 회전 완료 후 한 번의 seed·handoff에 사용할 credit */
	unsigned int wal_rotation_old_zone; /* foreground 재개 전에 회수할 이전 WAL zone */
	unsigned int foreground_handoff_gc_zone; /* 마지막으로 foreground에 넘긴 GC tail */
	unsigned int gc_no_progress; /* 연속으로 공간을 못 만든 GC cycle 수 */

	spinlock_t lock;
};

struct pending_flush_bio {
	struct list_head link;
	struct bio *bio;
};

struct pending_write_bio {
	struct list_head link;
	struct bio *bio;
	sector_t lba;
	sector_t nr;
	u64 metric_queued_ns;
};

struct memtable_flush_work {
	struct work_struct work;
	struct zns_base_c *c;
	struct skiplist *old_memtable;
	u64 seq_no;
	u64 split_gen;
	sector_t split_off;
};

/* SSTable 온디스크 형식과 메모리 색인 */
#define SSTABLE_MAGIC 0x53535442U  /* SSTable 매직 값: "SSTB" */

/* LBA 순서로 기록하는 고정 16B 레코드로, 인덱스에서 위치를 계산해 이진 탐색한다.
 * 온디스크 필드는 little-endian이며 cpu_to_le64/le64_to_cpu로 변환한다. */
struct sstable_record {
	__le64 lba;
	__le64 phys;
};

/* 세대별 첫 섹터의 little-endian 헤더다. min/max_lba로 조회 후보를 거른다. */
struct sstable_header {
	__le32 magic;
	__le32 reserved;
	__le64 seq_no;
	__le64 record_count;
	__le64 min_lba;
	__le64 max_lba;
};

/* SSTable header의 메모리 색인이다. phys는 header가 기록된 절대 섹터 주소다. */
struct sstable_info {
	sector_t phys;
	u64 seq_no;
	u64 record_count;
	u64 min_lba;
	u64 max_lba;
};

/* WAL 온디스크 형식과 복구 레코드 */
#define WAL_REC_PUT        1
#define WAL_REC_CHECKPOINT 2
#define WAL_REC_GC_PUT     3
#define WAL_REC_DELETE     4
#define WAL_REC_DELETE_RANGE 5
#define MAPPING_TOMBSTONE  U64_MAX
#define WAL_PAGE_MAGIC     0x57414C50U /* WAL 페이지 매직 값: "WALP" */
#define WAL_PAGE_VERSION   1
#define WAL_PAGE_SECTORS   (PAGE_SIZE / 512)

/* checkpoint는 memtable swap 시점의 WAL 위치를 (split_gen, split_off)로 기록한다.
 * replay는 그 이전 매핑을 SSTable에 반영된 것으로 간주한다. zone 재사용 후에도 기록 순서를 알 수 있도록 generation을 사용한다. */
/* 온디스크 필드는 little-endian이며 접근 시 cpu_to_le/le_to_cpu로 변환한다. */
struct wal_record {
	__le32 type;
	__le32 reserved;
	union {
	struct { __le64 lba; __le64 phys; } put;
	struct { __le64 lba; __le64 phys; __le64 expected_old; } gc_put;
	struct { __le64 seq_no; __le64 split_gen; __le64 split_off; } checkpoint;
    };
};

#define WAL_PAGE_MAX_RECORDS ((PAGE_SIZE - 16) / sizeof(struct wal_record))
#define GC_RELOC_WAL_PAGES 16
struct wal_page {
	__le32 magic;
	__le16 version;
	__le16 count;
	__le32 crc32;
	__le32 reserved;
	struct wal_record records[WAL_PAGE_MAX_RECORDS];
};

/* zone_pool_alloc이 새로 배정한 zone 하나 — 헤더를 비동기로 써줘야 할 대상 */
struct pending_header {
	unsigned int zone_id;
	unsigned int tag; /* zone_tag 열거값 */
	u64 gen; /* WAL zone이면 그 generation, 아니면 0 */
};

/* 쓰기·flush 체인이 공유하는 상태다. 새 zone의 header를 기록한 뒤 on_headers_done으로 본 작업을 진행한다. */
struct zns_io_ctx {
	struct zns_base_c *c;

	/* 헤더 체인 공용 */
	/* 현재 체인은 한 번에 header 하나를 등록한다. 배열 크기는 기존 경로와 호환되도록 유지한다. */
	struct pending_header headers[2];
	int nr_headers;
	int header_idx;
	void *hdr_buf; /* submit_header_async가 할당, header_write_done이 해제 */
	void (*on_headers_done)(struct zns_io_ctx *ctx);
	struct list_head wal_link;
	struct zns_io_ctx **extent_children;
	unsigned int extent_count;
	u64 diag_data_start_ns, diag_wal_queued_ns;

	/* PUT과 CHECKPOINT 기록에서 공유하는 WAL 버퍼와 예약 위치다. */
	sector_t wal_phys;
	struct wal_record *wal_buf;

	/* WRITE 경로 전용 */
	struct bio *orig_bio;
	u64 lba;
	sector_t phys;
	bool is_discard;
	u64 discard_blocks;
	sector_t reserved_phys; /* capacity 예약용; append 완료 전에는 실제 주소가 아님 */
	sector_t reserved_nr; /* append 예약 완료 시 dispatch_wp 회계 전진용 */
	sector_t orig_sector;
	unsigned int orig_opf;
	bio_end_io_t *orig_end_io;
	void *orig_private;

	/* SSTable flush 경로 전용 */
	struct skiplist *old_memtable;
	void *sstable_buf;
	sector_t sstable_phys;
	sector_t sstable_nr_sectors;
	sector_t sstable_done_sectors; /* 청크 기록 진행 오프셋(≤BIO_MAX_VECS씩) */
	u64 checkpoint_seq;
	u64 checkpoint_split_gen; /* 스왑 시점 활성 WAL zone의 generation */
	sector_t checkpoint_split_off; /* 그 zone 내 다음 WAL 쓰기 오프셋 */
};

/* 재적재 시 zone 용도를 복원할 수 있도록 첫 섹터에 태그와 generation을 기록한다. */
#define ZONE_HEADER_MAGIC 0x5A4E5348U  /* zone header 매직 값: "ZNSH" */

/* 온디스크 필드는 little-endian이며 cpu_to_le/le_to_cpu로 변환한다.
 * zone 섹터 0에 기록되어 재적재 후 zone 용도를 복원하는 단서. */
struct zone_header {
	__le32 magic;
	__le32 tag; /* zone_tag 열거값 */
	__le64 gen; /* WAL zone 배정 순번 — 회수 후 zone_id 순서≠기록순서라 replay 순서 판정용. WAL 외 0. */
};

/* 호출자가 c->lock을 보유해야 한다. free zone 하나를 배정하고 첫 섹터를 header용으로 예약한다.
 * gc_ctx가 false이면 reserve 이하의 free zone은 배정하지 않는다. 실패 시 ZONE_NONE을 반환한다. */
static unsigned int zone_pool_acquire_free(struct zone_pool *zp, enum zone_tag tag, bool gc_ctx)
{
	unsigned int z;

	if (!gc_ctx) {
		unsigned int free_count = 0;

		for (z = 0; z < zp->nr_zones; z++)
			if (zp->zone_tag[z] == ZONE_TAG_FREE)
				free_count++;
		if (free_count <= gc_reserved_zones)
			return ZONE_NONE;
	}

	for (z = 0; z < zp->nr_zones; z++) {
		if (zp->zone_tag[z] == ZONE_TAG_FREE) {
			zp->zone_tag[z] = tag;
			zp->wp[z] = 1;
			return z;
		}
	}
	return ZONE_NONE;
}

/* 호출자가 c->lock을 보유해야 한다. 태그별 active zone에서 nr 섹터를 예약한다.
 * 새 zone을 확보하면 new_zone_out에 ID를 반환하며, 호출자가 락 밖에서 header를 기록해야 한다. */
static int zone_pool_alloc(struct zone_pool *zp, enum zone_tag tag, sector_t nr,
			   sector_t *phys_out, int *new_zone_out, bool gc_ctx)
{
	unsigned int z = zp->active_zone[tag];

	if (new_zone_out)
		*new_zone_out = -1;

	/* header를 제외한 zone 용량을 넘는 요청은 새 zone을 소비하기 전에 거절한다. */
	if (nr > zp->zone_sectors - 1)
		return -ENOSPC;

	if (z == ZONE_NONE) {
		z = zone_pool_acquire_free(zp, tag, gc_ctx);
		if (z == ZONE_NONE)
			return -ENOSPC;
		zp->active_zone[tag] = z;
		if (tag == ZONE_TAG_WAL)
			zp->wal_gen[z] = zp->wal_next_gen++;
		if (new_zone_out)
			*new_zone_out = z;
	}

	while (zp->wp[z] + nr > zp->zone_sectors) {
		z = zone_pool_acquire_free(zp, tag, gc_ctx);
		if (z == ZONE_NONE)
			return -ENOSPC;
		zp->active_zone[tag] = z;
		if (tag == ZONE_TAG_WAL)
			zp->wal_gen[z] = zp->wal_next_gen++;
		if (new_zone_out)
			*new_zone_out = z;
	}

	*phys_out = z * zp->zone_sectors + zp->wp[z];
	zp->wp[z] += nr;
	return 0;
}

/* active tail이 남아 있어도 새 zone에서 시작한다. 호출자는 c->lock을 보유해야 한다.
 * reserve 차용 시에는 같은 작업의 commit으로 이전 zone을 회수할 수 있는지 먼저 검증한다. */
static int zone_pool_alloc_fresh(struct zone_pool *zp, enum zone_tag tag,
				 sector_t nr, sector_t *phys_out,
				 int *new_zone_out, bool gc_ctx)
{
	unsigned int z;

	if (new_zone_out)
		*new_zone_out = -1;
	if (nr > zp->zone_sectors - 1)
		return -ENOSPC;
	z = zone_pool_acquire_free(zp, tag, gc_ctx);
	if (z == ZONE_NONE)
		return -ENOSPC;
	zp->active_zone[tag] = z;
	if (tag == ZONE_TAG_WAL)
		zp->wal_gen[z] = zp->wal_next_gen++;
	if (new_zone_out)
		*new_zone_out = z;
	*phys_out = (sector_t)z * zp->zone_sectors + zp->wp[z];
	zp->wp[z] += nr;
	return 0;
}

/* 호출자가 c->lock을 보유하고 GC가 종료된 상태여야 한다. GC tail의 단독 소유권을 USER_DATA로 이전한다.
 * foreground append와 GC positional write가 같은 zone에서 경쟁하지 않도록 GC_DATA active slot을 비운다. */
static int zone_pool_handoff_gc_tail(struct zone_pool *zp, sector_t nr,
				     sector_t *phys_out, unsigned int *zone_out)
{
	unsigned int z = zp->active_zone[ZONE_TAG_GC_DATA];

	if (z == ZONE_NONE || zp->wp[z] + nr > zp->zone_sectors)
		return -ENOSPC;
	zp->active_zone[ZONE_TAG_GC_DATA] = ZONE_NONE;
	zp->active_zone[ZONE_TAG_USER_DATA] = z;
	*phys_out = (sector_t)z * zp->zone_sectors + zp->wp[z];
	zp->wp[z] += nr;
	if (zone_out)
		*zone_out = z;
	return 0;
}

/* 어느 태그에서든 active로 참조하는 zone은 회수 후보에서 제외한다. */
static bool zone_pool_zone_is_active(struct zone_pool *zp, unsigned int zone)
{
	unsigned int tag;

	for (tag = 0; tag < ZONE_TAG_COUNT; tag++)
		if (zp->active_zone[tag] == zone)
			return true;
	return false;
}

/* process context에서 ZNS reset을 동기 실행한다. 성공 뒤 소프트웨어 상태는 zone_pool_mark_free로 갱신한다. */
static int zone_reset_hw(struct zns_base_c *c, unsigned int zone_id)
{
	if (failpoint_hit(5))
		return -EIO;
	return blkdev_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET,
				  (sector_t)zone_id * c->zp->zone_sectors,
				  c->zp->zone_sectors, GFP_KERNEL);
}

/* 호출자가 c->lock을 보유해야 한다. 하드웨어 reset 성공 뒤 소프트웨어 상태를 free로 전환한다. */
static void zone_pool_mark_free(struct zone_pool *zp, unsigned int zone_id)
{
	/* 미제출 예약이 남은 회수는 오류다. 여기서는 진단만 남기며 대기열을 완료하지 않는다. */
	if (!list_empty(&zp->dispatch_waiters[zone_id]))
		DMERR("zone %u reclaimed while dispatch waiters remain — those writes will never complete (reclaim guard missed)",
		      zone_id);

	zp->wp[zone_id] = 0;
	/* dispatch_wp는 절대 섹터 좌표계 — 그 zone의 절대 시작 섹터로 되돌린다. */
	zp->dispatch_wp[zone_id] = (sector_t)zone_id * zp->zone_sectors;
	zp->zone_tag[zone_id] = ZONE_TAG_FREE;
	zp->invalid_count[zone_id] = 0;
	zp->sstable_live_count[zone_id] = 0;
	zp->append_ready[zone_id] = false;
}

static inline unsigned int zone_of(struct zone_pool *zp, sector_t phys)
{
	return phys / zp->zone_sectors;
}

/* per-bio 데이터 — 이 읽기가 pin한 zone(없으면 -1). zns_base_end_io가 unpin에 씀. */
struct zns_read_pin {
	int zone;
};

/* 읽기가 물리 주소를 확정할 때 c->lock 아래에서 zone을 pin한다.
 * 회수 측은 새 참조를 차단하고 기존 읽기가 drain된 뒤 reset해야 한다. */
static inline void zone_read_get(struct zone_pool *zp, unsigned int zid)
{
	atomic_inc(&zp->inflight_reads[zid]);
}
static inline void zone_read_put(struct zone_pool *zp, unsigned int zid)
{
	if (atomic_dec_and_test(&zp->inflight_reads[zid]))
		wake_up_all(&zp->reclaim_waitq);
}
/* reset 직전, 그 zone의 진행 중 읽기가 전부 끝나길 대기 — process context(워커) 전용. */
static void zone_wait_reads_drained(struct zone_pool *zp, unsigned int zid)
{
	wait_event(zp->reclaim_waitq, atomic_read(&zp->inflight_reads[zid]) == 0);
}

/* 배정 순서와 실제 제출 순서를 맞춘다. 차례가 아니면 대기열에 보관한다.
 * 이 함수가 c->lock을 잡으며 fire도 락 안에서 호출하므로 sleep하면 안 된다.
 * 0은 발행 또는 큐잉 성공, -ENOMEM은 호출자가 arg를 오류 처리해야 함을 뜻한다. */
static int zone_dispatch_gate(struct zns_base_c *c, sector_t phys, sector_t nr,
			      void (*fire)(void *arg), void *arg)
{
	unsigned int zone_id = zone_of(c->zp, phys);
	struct dispatch_waiter *w, *tmp;
	bool my_turn;

	spin_lock_irq(&c->lock);
	my_turn = (phys == c->zp->dispatch_wp[zone_id]);
	if (!my_turn) {
		/* bio 완료 콜백에서도 호출되므로 sleep 가능한 할당을 사용하지 않는다. */
		w = kzalloc(sizeof(*w), GFP_ATOMIC);
		if (!w) {
			spin_unlock_irq(&c->lock);
			return -ENOMEM;
		}
		w->phys = phys;
		w->nr = nr;
		w->fire = fire;
		w->arg = arg;
		list_add_tail(&w->link, &c->zp->dispatch_waiters[zone_id]);
		spin_unlock_irq(&c->lock);
		return 0;
	}

	/* dispatch_wp 전진과 fire 호출을 같은 락 아래에서 처리해 제출 순서를 유지한다. */
	c->zp->dispatch_wp[zone_id] += nr;
	fire(arg);
	for (;;) {
		struct dispatch_waiter *found = NULL;

		list_for_each_entry(tmp, &c->zp->dispatch_waiters[zone_id], link) {
			if (tmp->phys == c->zp->dispatch_wp[zone_id]) {
				found = tmp;
				break;
			}
		}
		if (!found)
			break;
		list_del(&found->link);
		c->zp->dispatch_wp[zone_id] += found->nr;
		found->fire(found->arg);
		kfree(found);
	}
	spin_unlock_irq(&c->lock);
	return 0;
}

static void dispatch_fire_submit_bio(void *arg)
{
	submit_bio_deferred((struct bio *)arg);
}

/* 제출 차례가 되면 bio를 하위 장치로 보낸다. */
static void zone_dispatch_write(struct zns_base_c *c, sector_t phys, sector_t nr, struct bio *bio)
{
	if (metrics_enabled)
		atomic64_add((u64)nr * 512, &c->metric_write_bytes[
			READ_ONCE(c->zp->zone_tag[zone_of(c->zp, phys)])]);
	if (zone_dispatch_gate(c, phys, nr, dispatch_fire_submit_bio, bio)) {
		DMERR("zone dispatch: out of memory queuing write (zone %u, phys %llu) — failing this write",
		      zone_of(c->zp, phys), (unsigned long long)phys);
		bio->bi_status = BLK_STS_RESOURCE;
		bio_endio(bio);
	}
}

static void dispatch_fire_complete(void *arg)
{
	complete((struct completion *)arg);
}

static void dispatch_fire_noop(void *arg)
{
	/* 취소된 예약 — 실제로 나갈 bio가 없으므로 자리만 넘긴다 */
}

/* 실패한 작업의 미제출 예약을 건너뛴다. 취소하지 않으면 뒤의 모든 예약이 제출 차례를 기다리게 된다. */
static void zone_dispatch_cancel(struct zns_base_c *c, sector_t phys, sector_t nr)
{
	if (zone_dispatch_gate(c, phys, nr, dispatch_fire_noop, NULL))
		DMERR("zone dispatch: out of memory cancelling reservation (zone %u, phys %llu) — this zone's dispatch queue will stall",
		      zone_of(c->zp, phys), (unsigned long long)phys);
}

/* Zone Append는 실제 기록 위치를 컨트롤러가 결정하므로 같은 zone에 여러
 * bio를 동시 제출해도 된다. 단, 오프셋 0의 zone header 쓰기가 먼저 완료돼야
 * 하므로 새 zone의 append는 header 완료 콜백까지 이 큐에서 대기한다. */
static int zone_append_write(struct zns_base_c *c, unsigned int zid, struct bio *bio)
{
	struct append_waiter *w;

	spin_lock_irq(&c->lock);
	if (c->zp->append_ready[zid]) {
		if (metrics_enabled)
			atomic64_add(bio->bi_iter.bi_size,
				&c->metric_write_bytes[c->zp->zone_tag[zid]]);
		spin_unlock_irq(&c->lock);
		submit_bio_deferred(bio);
		return 0;
	}
	w = kzalloc(sizeof(*w), GFP_ATOMIC);
	if (!w) {
		spin_unlock_irq(&c->lock);
		return -ENOMEM;
	}
	w->bio = bio;
	if (metrics_enabled)
		atomic64_add(bio->bi_iter.bi_size,
			&c->metric_write_bytes[c->zp->zone_tag[zid]]);
	list_add_tail(&w->link, &c->zp->append_waiters[zid]);
	spin_unlock_irq(&c->lock);
	return 0;
}

static void zone_append_header_done(struct zns_base_c *c, unsigned int zid,
				    blk_status_t status)
{
	LIST_HEAD(ready);
	struct append_waiter *w, *tmp;

	spin_lock_irq(&c->lock);
	if (!status)
		c->zp->append_ready[zid] = true;
	list_splice_init(&c->zp->append_waiters[zid], &ready);
	spin_unlock_irq(&c->lock);

	list_for_each_entry_safe(w, tmp, &ready, link) {
		list_del(&w->link);
		if (status) {
			w->bio->bi_status = status;
			bio_endio(w->bio);
		} else {
			submit_bio_deferred(w->bio);
		}
		kfree(w);
	}
}

static void zone_quarantine(struct zns_base_c *c, unsigned int zid,
			    enum zone_tag tag, blk_status_t status)
{
	spin_lock_irq(&c->lock);
	if (tag < ZONE_TAG_COUNT && c->zp->active_zone[tag] == zid)
		c->zp->active_zone[tag] = ZONE_NONE;
	spin_unlock_irq(&c->lock);
	zone_append_header_done(c, zid, status);
}

/* process context에서 예약 범위의 제출 차례를 기다린다. 반환 뒤 호출자가 동기 I/O를 제출한다.
 * 완료 콜백이나 spinlock 보유 상태에서는 호출하면 안 된다. */
static void zone_dispatch_wait_turn(struct zns_base_c *c, sector_t phys, sector_t nr)
{
	struct completion done;

	init_completion(&done);
	if (zone_dispatch_gate(c, phys, nr, dispatch_fire_complete, &done)) {
		/* 게이트 할당 실패 시 순서 보장 없이 진행하는 기존 fallback이다. 오류는 아래에 기록한다. */
		DMERR("zone dispatch: out of memory queuing compaction write (zone %u, phys %llu) — proceeding without ordering guarantee",
		      zone_of(c->zp, phys), (unsigned long long)phys);
		return;
	}
	wait_for_completion(&done);
}

/* zone_id 섹터 0을 동기적으로 읽어 헤더를 얻는다. ctr()(process context) 전용 */
static int read_zone_header(struct zns_base_c *c, unsigned int zone_id, struct zone_header *hdr_out)
{
	struct zone_header *buf;
	struct bio *bio;
	struct page *page;
	int ret;

	buf = kzalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	bio = bio_alloc(GFP_KERNEL, 1);
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = (sector_t)zone_id * c->zp->zone_sectors;
	bio->bi_opf = REQ_OP_READ;
	page = virt_to_page(buf);
	bio_add_page(bio, page, 512, offset_in_page(buf));

	ret = submit_bio_wait(bio);
	bio_put(bio);
	if (!ret)
		*hdr_out = *buf;
	kfree(buf);
	return ret;
}

/* kmalloc·vmalloc 버퍼를 모두 bio에 붙일 수 있도록 해당 오프셋의 페이지를 구한다. */
static struct page *buf_page(void *buf, size_t off)
{
	void *addr = (char *)buf + off;

	return is_vmalloc_addr(addr) ? vmalloc_to_page(addr) : virt_to_page(addr);
}

/* kvmalloc 버퍼를 BIO_MAX_VECS 한도에 맞춰 나누어 동기 전송한다. process context 전용이다.
 * WRITE 호출자는 전체 범위의 제출 순서를 zone_dispatch_wait_turn으로 먼저 확보해야 한다. */
static int sstable_io_sync(struct zns_base_c *c, unsigned int op,
			   sector_t phys, void *buf, sector_t nr_sectors)
{
	size_t total_bytes = (size_t)nr_sectors * 512;
	size_t done = 0;
	sector_t cur = phys;

	while (done < total_bytes) {
		size_t first_off = offset_in_page((char *)buf + done);
		size_t chunk = min(total_bytes - done,
				   (size_t)BIO_MAX_VECS * PAGE_SIZE - first_off);
		unsigned int nr_pages = DIV_ROUND_UP(first_off + chunk, PAGE_SIZE);
		struct bio *bio;
		size_t rem = chunk;
		size_t page_done = 0;
		unsigned int p;
		int ret;

		bio = bio_alloc(GFP_KERNEL, nr_pages);
		if (!bio)
			return -ENOMEM;
		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = cur;
		bio->bi_opf = op;
		for (p = 0; p < nr_pages; p++) {
			void *addr = (char *)buf + done + page_done;
			size_t page_off = offset_in_page(addr);
			size_t len = min(rem, PAGE_SIZE - page_off);

			if (bio_add_page(bio, buf_page(buf, done + page_done),
					 len, page_off) != len) {
				bio_put(bio);
				return -EIO;
			}
			rem -= len;
			page_done += len;
		}
		if (metrics_enabled && (op & REQ_OP_MASK) == REQ_OP_WRITE)
			atomic64_add(chunk, &c->metric_write_bytes[
				READ_ONCE(c->zp->zone_tag[zone_of(c->zp, cur)])]);
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			return ret;
		done += chunk;
		cur += chunk / 512;
	}
	return 0;
}

/* 절대 섹터 phys에서 512B를 동기적으로 읽어 SSTable 헤더를 얻는다.
 * read_zone_header와 같은 이유로 ctr() 전용(process context). */
static int read_sstable_header(struct zns_base_c *c, sector_t phys, struct sstable_header *hdr_out)
{
	struct sstable_header *buf;
	struct bio *bio;
	struct page *page;
	int ret;

	buf = kzalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	bio = bio_alloc(GFP_KERNEL, 1);
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = phys;
	bio->bi_opf = REQ_OP_READ;
	page = virt_to_page(buf);
	bio_add_page(bio, page, 512, offset_in_page(buf));

	ret = submit_bio_wait(bio);
	bio_put(bio);
	if (!ret)
		*hdr_out = *buf;
	kfree(buf);
	return ret;
}

/* 호출자가 c->lock을 보유해야 한다. memtable 매핑을 갱신하고 기존 물리 위치의 무효 추정치를 올린다. */
static int mapping_put(struct zns_base_c *c, u64 lba, u64 phys)
{
	u64 old_phys;
	int ret = skiplist_upsert(c->memtable, lba, phys, &old_phys);

	if (ret < 0)
		return ret;
	/* SSTable에만 있는 이전 값은 여기서 조회하지 않는다. 무효 추정치는 이후 compaction에서 반영한다. */
	if (ret == 1 && old_phys != MAPPING_TOMBSTONE)
		c->zp->invalid_count[zone_of(c->zp, old_phys)]++;
	return 0;
}

/* 호출자가 c->lock을 보유해야 한다. active, frozen 순서로 조회하며 SSTable은 읽지 않는다. */
static int mapping_get(struct zns_base_c *c, u64 lba, u64 *phys_out)
{
	if (skiplist_lookup(c->memtable, lba, phys_out))
		return 1;
	return c->frozen_memtable && skiplist_lookup(c->frozen_memtable, lba, phys_out);
}

/* flush 종료 시 frozen map의 공개 여부를 확인한다. 공개되지 않은 map은 detach까지 보존한다.
 * checkpoint 실패 시 WAL 회수 frontier를 전진시키지 않는다. */
static void release_frozen_memtable(struct zns_base_c *c,
				    struct skiplist *old, bool durable)
{
	bool release;

	spin_lock_irq(&c->lock);
	if (!durable)
		c->checkpoint_failed = true;
	release = c->frozen_published;
	if (release) {
		c->frozen_memtable = NULL;
		c->frozen_published = false;
	} else {
		/* 복구 전까지 map을 보존하고 I/O와 GC를 fail-closed 상태로 유지한다. */
		c->metadata_failed = true;
	}
	spin_unlock_irq(&c->lock);
	if (!durable)
		DMERR("flush chain failed: WAL reclamation frontier frozen until reload (published=%u)",
		      release ? 1 : 0);
	if (release) {
		skiplist_destroy(old);
		kfree(old);
	}
}

/* 호출자가 c->lock을 보유해야 한다. 메모리의 현재 매핑이 expected_old와 다르면 이주본을 무효화한다.
 * 메모리에서 miss이면 SSTable의 기존 위치를 이주한 것으로 보고 삽입한다.
 * 0은 반영, 1은 경합으로 건너뜀, 음수는 오류다. replay의 경합 보호는 WAL_REC_GC_PUT가 담당한다. */
static int mapping_put_if_match(struct zns_base_c *c, u64 lba,
				u64 expected_old, u64 new_phys)
{
	u64 cur;

	if (mapping_get(c, lba, &cur) && cur != expected_old) {
		c->zp->invalid_count[zone_of(c->zp, new_phys)]++;
		return 1;
	}
	return mapping_put(c, lba, new_phys);
}

/* c->sstables[]에 SSTable 하나를 등록, 필요하면 배열을 2배로 키운다.
 * 그 zone의 sstable_live_count도 같이 올린다(compaction의 zone 회수 판단 근거).
 * 호출자가 c->lock을 쥐고 있어야 한다. gfp는 호출 컨텍스트에 맞게. */
static int sstable_register(struct zns_base_c *c, sector_t phys, u64 seq_no,
			    u64 record_count, u64 min_lba, u64 max_lba, gfp_t gfp)
{
	struct sstable_info *si;

	if (c->nr_sstables == c->sstables_cap) {
		unsigned int new_cap = c->sstables_cap ? c->sstables_cap * 2 : 16;
		struct sstable_info *grown = krealloc(c->sstables, new_cap * sizeof(*grown), gfp);

		if (!grown)
			return -ENOMEM;
		c->sstables = grown;
		c->sstables_cap = new_cap;
	}

	si = &c->sstables[c->nr_sstables++];
	si->phys = phys;
	si->seq_no = seq_no;
	si->record_count = record_count;
	si->min_lba = min_lba;
	si->max_lba = max_lba;
	c->zp->sstable_live_count[zone_of(c->zp, phys)]++;
	c->read_view_epoch++;
	return 0;
}

/* 호출자가 c->lock을 보유해야 한다. phys로 색인을 찾아 마지막 항목과 교체하고 zone의 live 참조 수를 줄인다. */
static void sstable_remove_by_phys(struct zns_base_c *c, sector_t phys)
{
	unsigned int i;

	for (i = 0; i < c->nr_sstables; i++) {
		if (c->sstables[i].phys != phys)
			continue;

		if (c->zp->sstable_live_count[zone_of(c->zp, phys)] > 0)
			c->zp->sstable_live_count[zone_of(c->zp, phys)]--;

		c->sstables[i] = c->sstables[c->nr_sstables - 1];
		c->nr_sstables--;
		return;
	}
}

/* checkpoint 탐색과 replay가 공유하는 WAL 순회 콜백이다.
 * start부터 wp까지 기록 순서대로 호출하며, ctr()의 process context에서만 사용한다. */
typedef void (*wal_record_fn)(void *fn_ctx, struct wal_record *rec, sector_t sector);

static void wal_zone_for_each_record(struct zns_base_c *c, unsigned int zone_id,
				     sector_t start, sector_t wp,
				     wal_record_fn fn, void *fn_ctx)
{
	sector_t cur = start;
	void *buf;
	struct page *page;

	if (cur >= wp)
		return;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		DMERR("WAL scan: out of memory, skipping zone %u from sector %llu", zone_id, (unsigned long long)start);
		return;
	}
	page = virt_to_page(buf);

	while (cur < wp) {
		sector_t chunk = min_t(sector_t, wp - cur, WAL_PAGE_SECTORS);
		struct bio *bio;
		int ret;

		bio = bio_alloc(GFP_KERNEL, 1);
		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = (sector_t)zone_id * c->zp->zone_sectors + cur;
		bio->bi_opf = REQ_OP_READ;
		bio_add_page(bio, page, chunk * 512, 0);

		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret) {
			DMERR("WAL scan: read failed at zone %u sector %llu", zone_id, (unsigned long long)cur);
			break;
		}

		if (le32_to_cpu(((struct wal_page *)buf)->magic) == WAL_PAGE_MAGIC) {
			struct wal_page *page_rec = buf;
			u16 count = le16_to_cpu(page_rec->count);
			u32 stored_crc = le32_to_cpu(page_rec->crc32);
			u32 actual_crc;
			unsigned int i;

			page_rec->crc32 = 0;
			actual_crc = crc32(~0U, buf, PAGE_SIZE);
			if (chunk < WAL_PAGE_SECTORS || count > WAL_PAGE_MAX_RECORDS ||
			    le16_to_cpu(page_rec->version) != WAL_PAGE_VERSION ||
			    actual_crc != stored_crc) {
				DMERR("WAL scan: invalid/torn page at zone %u sector %llu",
				      zone_id, (unsigned long long)cur);
				break;
			}
			for (i = 0; i < count; i++)
				fn(fn_ctx, &page_rec->records[i], cur);
			cur += WAL_PAGE_SECTORS;
		} else {
			/* 이전 형식은 첫 512B 섹터의 단일 레코드로 읽는다. */
			fn(fn_ctx, (struct wal_record *)buf, cur);
			cur++;
		}
	}

	kfree(buf);
}

struct checkpoint_scan_state {
	int found;
	u64 seq_no;
	u64 split_gen;
	sector_t split_off;
};

/* WAL 순회 중 마지막 CHECKPOINT의 seq_no와 split 위치를 저장한다. 매핑은 적용하지 않는다. */
static void checkpoint_scan_cb(void *fn_ctx, struct wal_record *rec, sector_t sector)
{
	struct checkpoint_scan_state *st = fn_ctx;

	if (le32_to_cpu(rec->type) == WAL_REC_CHECKPOINT) {
		st->found = 1;
		st->seq_no = le64_to_cpu(rec->checkpoint.seq_no);
		st->split_gen = le64_to_cpu(rec->checkpoint.split_gen);
		st->split_off = le64_to_cpu(rec->checkpoint.split_off);
	}
}

/* wal_zone_for_each_record 콜백 — PUT을 memtable에 적용(replay 본체) */
static void wal_replay_cb(void *fn_ctx, struct wal_record *rec, sector_t sector)
{
	struct zns_base_c *c = fn_ctx;

	if (le32_to_cpu(rec->type) == WAL_REC_PUT) {
		u64 old_phys;

		if (skiplist_upsert(c->memtable, le64_to_cpu(rec->put.lba),
				    le64_to_cpu(rec->put.phys), &old_phys) == 1)
			if (old_phys != MAPPING_TOMBSTONE)
				c->zp->invalid_count[zone_of(c->zp, old_phys)]++;
	} else if (le32_to_cpu(rec->type) == WAL_REC_DELETE) {
		u64 old_phys;

		if (skiplist_upsert(c->memtable, le64_to_cpu(rec->put.lba),
				    MAPPING_TOMBSTONE, &old_phys) == 1 &&
		    old_phys != MAPPING_TOMBSTONE)
			c->zp->invalid_count[zone_of(c->zp, old_phys)]++;
	} else if (le32_to_cpu(rec->type) == WAL_REC_DELETE_RANGE) {
		u64 lba = le64_to_cpu(rec->put.lba);
		u64 blocks = le64_to_cpu(rec->put.phys);
		u64 i;

		for (i = 0; i < blocks; i++) {
			u64 old_phys;

			if (skiplist_upsert(c->memtable,
					    lba + i * BLOCK_SECTORS,
					    MAPPING_TOMBSTONE, &old_phys) == 1 &&
			    old_phys != MAPPING_TOMBSTONE)
				c->zp->invalid_count[zone_of(c->zp, old_phys)]++;
		}
	} else if (le32_to_cpu(rec->type) == WAL_REC_GC_PUT) {
		u64 lba = le64_to_cpu(rec->gc_put.lba);
		u64 phys = le64_to_cpu(rec->gc_put.phys);
		u64 expected_old = le64_to_cpu(rec->gc_put.expected_old);
		u64 cur, old_phys;

		/* GC WAL이 foreground overwrite보다 뒤에 durable해져도,
		 * replay 시 현재 mapping이 GC가 읽은 예전 위치와 다르면
		 * stale 재배치로 보고 무시한다. memtable miss는 checkpoint
		 * SSTable에 expected_old가 있는 일반적 경우이므로 적용한다. */
		if (skiplist_lookup(c->memtable, lba, &cur) && cur != expected_old)
			return;
		if (skiplist_upsert(c->memtable, lba, phys, &old_phys) == 1)
			c->zp->invalid_count[zone_of(c->zp, old_phys)]++;
	}
}

/* zone ID는 재사용되므로 WAL은 generation 순서로 재생한다.
 * 최신 generation부터 마지막 checkpoint를 찾은 뒤, 그 경계 이후를 generation 오름차순으로 적용한다. */
static void replay_wal_zones(struct zns_base_c *c, unsigned int *wal_zones, unsigned int count)
{
	u64 split_gen = 0;
	sector_t split_off = 0;
	u64 ckpt_seq = 0;
	int found = 0;
	int i, j;

	/* generation 오름차순 정렬. */
	for (i = 1; i < (int)count; i++) {
		unsigned int key = wal_zones[i];
		u64 key_gen = c->zp->wal_gen[key];

		for (j = i - 1; j >= 0 && c->zp->wal_gen[wal_zones[j]] > key_gen; j--)
			wal_zones[j + 1] = wal_zones[j];
		wal_zones[j + 1] = key;
	}

	/* 1단계: generation 큰 것부터 거꾸로 — 마지막 체크포인트를 찾으면 멈춤. */
	for (i = (int)count - 1; i >= 0; i--) {
		unsigned int zone_id = wal_zones[i];
		struct checkpoint_scan_state st = { 0 };

		wal_zone_for_each_record(c, zone_id, 1, c->zp->wp[zone_id], checkpoint_scan_cb, &st);
		if (st.found) {
			found = 1;
			ckpt_seq = st.seq_no;
			split_gen = st.split_gen;
			split_off = st.split_off;
			break;
		}
	}

	if (found)
		DMINFO("WAL replay: last checkpoint seq=%llu (split_gen=%llu, split_off=%llu) — skipping WAL entries before it",
		       (unsigned long long)ckpt_seq, (unsigned long long)split_gen,
		       (unsigned long long)split_off);
	else
		DMINFO("WAL replay: no checkpoint found — replaying all %u WAL zone(s) in full", count);

	/* 2단계: generation 오름차순으로 재생. */
	for (i = 0; i < (int)count; i++) {
		unsigned int zone_id = wal_zones[i];
		u64 gen = c->zp->wal_gen[zone_id];
		sector_t wp = c->zp->wp[zone_id];
		sector_t start = 1;

		if (found) {
			if (gen < split_gen)
				continue; /* 이 zone 전체가 체크포인트 이전 */
			if (gen == split_gen)
				start = split_off; /* 걸쳐있는 zone — split 지점부터 */
			/* gen > split_gen이면 전부 재생(start=1) */
		}

		wal_zone_for_each_record(c, zone_id, start, wp, wal_replay_cb, c);
	}

	/* 복구된 마지막 체크포인트 이후부터 seq_no를 이어가야 다음 flush가
	 * 아직 회수 안 된 옛 SSTable과 seq_no가 겹치지 않는다. */
	c->next_seq_no = found ? ckpt_seq + 1 : 0;

	/* 복구한 checkpoint를 durable frontier로 사용해 다음 회수에서 이전 WAL을 정리한다. */
	if (found)
		c->wal_durable_split_gen = split_gen;
}

/* zone_id 안에 순서대로 쌓여있는 SSTable들을 전부 찾아 c->sstables[]에 등록한다.
 * 헤더를 읽을 때마다 record_count로 다음 SSTable 시작 위치를 계산해 이어감.
 * ctr() 전용 process context. */
static void scan_sstable_zone(struct zns_base_c *c, unsigned int zone_id, sector_t wp)
{
	sector_t cur = 1; /* 섹터 0은 zone 태그 헤더 */

	while (cur < wp) {
		sector_t phys = (sector_t)zone_id * c->zp->zone_sectors + cur;
		struct sstable_header hdr;
		sector_t data_sectors;

		if (read_sstable_header(c, phys, &hdr) || le32_to_cpu(hdr.magic) != SSTABLE_MAGIC)
			break; /* 손상되었거나 여기서 SSTable들이 끝남 */

		if (sstable_register(c, phys, le64_to_cpu(hdr.seq_no), le64_to_cpu(hdr.record_count),
				       le64_to_cpu(hdr.min_lba), le64_to_cpu(hdr.max_lba), GFP_KERNEL))
			DMERR("SSTable scan: out of memory registering zone %u sector %llu",
			      zone_id, (unsigned long long)cur);

		data_sectors = round_up(le64_to_cpu(hdr.record_count) * sizeof(struct sstable_record), 512) / 512;
		cur += 1 + data_sectors;
	}
}

struct recovery_scan_ctx {
	struct zns_base_c *c;
	unsigned int *wal_zones;
	unsigned int nr_wal_zones;
	unsigned int *sstable_zones;
	unsigned int nr_sstable_zones;
};

/* zone report의 하드웨어 wp와 header로 할당·제출 상태를 복원한다.
 * WAL과 SSTable zone은 목록에 모아 전체 report 이후에 스캔한다. */
static int recovery_zone_cb(struct blk_zone *zone, unsigned int idx, void *data)
{
	struct recovery_scan_ctx *rctx = data;
	struct zns_base_c *c = rctx->c;
	struct zone_header hdr;
	u32 magic, tag;
	u64 gen;
	sector_t real_wp;
	int ret;

	if (zone->wp <= zone->start)
		return 0; /* 한 번도 안 쓰인 zone */

	ret = read_zone_header(c, idx, &hdr);
	if (ret)
		return 0;
	/* 온디스크는 __le — 여기서 한 번에 CPU 엔디안으로 디코드 */
	magic = le32_to_cpu(hdr.magic);
	tag = le32_to_cpu(hdr.tag);
	gen = le64_to_cpu(hdr.gen);
	if (magic != ZONE_HEADER_MAGIC)
		return 0; /* 헤더 없음/손상 — 이 zone은 복원 대상 아님 */

	real_wp = zone->wp - zone->start; /* zone 기준 상대 wp */

	c->zp->zone_tag[idx] = tag;
	c->zp->append_ready[idx] = true;
	c->zp->wp[idx] = real_wp;
	/* 복구한 dispatch_wp를 하드웨어 wp와 맞춰 기존 예약을 다시 기다리지 않도록 한다. */
	c->zp->dispatch_wp[idx] = (sector_t)idx * c->zp->zone_sectors + real_wp;

	/* 용량이 남은 zone을 해당 태그의 active slot으로 복원한다. */
	if (real_wp < c->zp->zone_sectors)
		c->zp->active_zone[tag] = idx;

	if (tag == ZONE_TAG_WAL) {
		/* generation 복원 — 새 WAL zone이 이어서 더 큰 gen을 받도록 wal_next_gen도 max(gen)+1로 끌어올린다. */
		c->zp->wal_gen[idx] = gen;
		if (gen >= c->zp->wal_next_gen)
			c->zp->wal_next_gen = gen + 1;
		rctx->wal_zones[rctx->nr_wal_zones++] = idx;
	} else if (tag == ZONE_TAG_SSTABLE) {
		rctx->sstable_zones[rctx->nr_sstable_zones++] = idx;
	}

	return 0;
}

/* 한 bio에 담을 수 있는 최대 섹터 수(BIO_MAX_VECS 페이지). SSTable I/O를 이 단위로 쪼개 nr_vecs 초과 BUG를 피한다. */
#define SSTABLE_IO_MAX_SECTORS ((sector_t)BIO_MAX_VECS * (PAGE_SIZE / 512))

static void header_write_done(struct bio *bio);
static void wal_commit_ctx(struct zns_io_ctx *ctx, blk_status_t wal_status,
			   bool allow_flush);
static void submit_wal_async(struct zns_io_ctx *ctx);
static void submit_data_append_async(struct zns_io_ctx *ctx);
static void data_append_done(struct bio *bio);
static void extent_append_done(struct bio *bio);
static void submit_sstable_write_async(struct zns_io_ctx *ctx);
static void sstable_write_chunk_done(struct bio *bio);
static void sstable_flush_complete(struct zns_io_ctx *ctx, blk_status_t status);
static void submit_checkpoint_async(struct zns_io_ctx *ctx);
static void checkpoint_write_done(struct bio *bio);
static void flush_chain_end(struct zns_base_c *c);
static unsigned int gc_count_free_zones(struct zone_pool *zp);
static void flush_memtable_async(struct zns_base_c *c, struct skiplist *old_memtable,
				 u64 seq_no, u64 split_gen, sector_t split_off);
static void wal_batch_work_fn(struct work_struct *work);
static void pending_flush_work_fn(struct work_struct *work);
static void pending_write_work_fn(struct work_struct *work);

static void foreground_write_done(struct zns_base_c *c)
{
	if (atomic_dec_and_test(&c->foreground_writes)) {
		wake_up_all(&c->flush_waitq);
		mod_delayed_work(zns_wq, &c->pending_flush_work, 0);
	}
}

/* 아직 bio_chain하지 않은 extent 자식만 정리한다. */
static void free_extent_children(struct zns_io_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->extent_count; i++) {
		bio_put(ctx->extent_children[i]->orig_bio);
		kfree(ctx->extent_children[i]);
	}
	kfree(ctx->extent_children);
}

/* 새 zone의 헤더가 없으면 그 뒤의 데이터는 재시작 후 식별할 수 없다.
 * 해당 zone들을 현재 태그의 active slot에서 떼어 재사용하지 못하게 하고,
 * 아직 제출하지 않은 헤더 예약과 본 작업 예약을 모두 완료 처리한다. */
static void abort_after_header_failure(struct zns_io_ctx *ctx,
				       blk_status_t status, bool current_unsubmitted)
{
	struct zns_base_c *c = ctx->c;
	int i;

	for (i = ctx->header_idx; i < ctx->nr_headers; i++) {
		struct pending_header *h = &ctx->headers[i];

		if (i > ctx->header_idx || current_unsubmitted)
			zone_dispatch_cancel(c,
				(sector_t)h->zone_id * c->zp->zone_sectors, 1);
		zone_quarantine(c, h->zone_id, h->tag, status);
	}

	if (ctx->on_headers_done == submit_data_append_async) {
		free_extent_children(ctx);
		zone_dispatch_cancel(c, ctx->reserved_phys, ctx->reserved_nr);
		ctx->orig_bio->bi_status = status;
		foreground_write_done(c);
		bio_endio(ctx->orig_bio);
		kfree(ctx);
	} else if (ctx->on_headers_done == submit_wal_async) {
		zone_dispatch_cancel(c, ctx->wal_phys, 1);
		zone_dispatch_cancel(c, ctx->reserved_phys, ctx->reserved_nr);
		ctx->orig_bio->bi_status = status;
		foreground_write_done(c);
		bio_endio(ctx->orig_bio);
		kfree(ctx);
	} else if (ctx->on_headers_done == submit_sstable_write_async) {
		zone_dispatch_cancel(c, ctx->sstable_phys, ctx->sstable_nr_sectors);
		sstable_flush_complete(ctx, status);
	} else if (ctx->on_headers_done == submit_checkpoint_async) {
		zone_dispatch_cancel(c, ctx->wal_phys, 1);
		release_frozen_memtable(c, ctx->old_memtable, false);
		kfree(ctx);
		flush_chain_end(c);
	} else {
		DMERR("zone header failure: unknown continuation, leaking context safely");
	}
}

static void flush_memtable_work_fn(struct work_struct *work)
{
	struct memtable_flush_work *fw = container_of(work, struct memtable_flush_work, work);

	flush_memtable_async(fw->c, fw->old_memtable, fw->seq_no, fw->split_gen, fw->split_off);
	kfree(fw);
}

/* c->lock을 잡은 상태에서 호출. GC가 flush 임계값 통과 순간을 가리고
 * 있었더라도 GC 종료 경로가 같은 스왑을 재시도할 수 있게 한곳에 둔다. */
static struct memtable_flush_work *
schedule_memtable_flush_locked(struct zns_base_c *c, bool force)
{
	struct memtable_flush_work *flush_work;
	struct skiplist *new_memtable;
	struct skiplist *flushed_memtable;
	unsigned int wal_zone;
	u64 flushed_seq;
	u64 flushed_split_gen;
	sector_t flushed_split_off;

	if (c->gc_active || c->sstable_rotation_pending ||
	    c->wal_batch_busy || !list_empty(&c->wal_pending) ||
	    c->wal_ckpt_inflight || c->metadata_failed ||
	    (!force && c->wal_rotation_pending) ||
	    c->stopping || (!force && c->memtable->count < flush_threshold) ||
	    (force && !c->memtable->count))
		return NULL;

	wal_zone = c->zp->active_zone[ZONE_TAG_WAL];
	if (wal_zone == ZONE_NONE)
		return NULL;

	/* WAL 완료 콜백도 이 경로를 사용하므로 sleep 가능한 할당은 금지한다. */
	new_memtable = kzalloc(sizeof(*new_memtable), GFP_ATOMIC);
	flush_work = kzalloc(sizeof(*flush_work), GFP_ATOMIC);
	if (!new_memtable || !flush_work || skiplist_init(new_memtable)) {
		kfree(flush_work);
		kfree(new_memtable);
		return NULL;
	}

	flushed_memtable = c->memtable;
	flushed_seq = c->next_seq_no++;
	flushed_split_gen = c->zp->wal_gen[wal_zone];
	flushed_split_off = c->zp->wp[wal_zone];
	c->memtable = new_memtable;
	c->frozen_memtable = flushed_memtable;
	c->frozen_published = false;
	c->wal_ckpt_inflight++;
	c->wal_highest_issued_split_gen = flushed_split_gen;
	INIT_WORK(&flush_work->work, flush_memtable_work_fn);
	flush_work->c = c;
	flush_work->old_memtable = flushed_memtable;
	flush_work->seq_no = flushed_seq;
	flush_work->split_gen = flushed_split_gen;
	flush_work->split_off = flushed_split_off;

	return flush_work;
}

/* 데이터 append가 끝난 일반 PUT만 조기 완료한다. lock 안에서 매핑과 WAL
 * 소유권을 함께 공개한다. worker가 ctx를 해제할 수 있으므로 unlock 뒤에는
 * ctx를 참조하지 않는다. orig_bio==NULL은 WAL만 소유하는 buffered ctx다.
 * GC/회전/메타데이터 압력에서는 기존 durable 완료 경로로 되돌아간다. */
static bool wal_try_buffer_write(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	struct bio *orig = ctx->orig_bio;
	sector_t phys = ctx->reserved_phys;
	sector_t nr = ctx->reserved_nr;
	bool kick;

	if (ctx->is_discard || (orig->bi_opf & (REQ_FUA | REQ_PREFLUSH)))
		return false;
	spin_lock_irq(&c->lock);
	if (c->metadata_failed || c->stopping ||
	    c->gc_active || c->wal_rotation_pending || c->sstable_rotation_pending ||
	    c->wal_ckpt_inflight || (c->wal_batch_busy && c->wal_batch_has_waiters) ||
	    !list_empty(&c->pending_flush_bios) ||
	    c->wal_pending_count != c->wal_pending_buffered_count ||
	    c->wal_pending_count >= wal_group_capacity() ||
	    c->wal_buffered_count >= 2 * wal_group_capacity() ||
	    c->memtable->count >= flush_threshold ||
	    gc_count_free_zones(c->zp) <= gc_low_watermark + 2) {
		spin_unlock_irq(&c->lock);
		return false;
	}
	if (mapping_put(c, ctx->lba, ctx->phys)) {
		spin_unlock_irq(&c->lock);
		return false; /* 기존 경로에서 삽입 오류를 처리한다. */
	}
	ctx->orig_bio = NULL;
	INIT_LIST_HEAD(&ctx->wal_link);
	if (metrics_enabled && wal_timing_enabled)
		ctx->diag_wal_queued_ns = ktime_get_ns();
	list_add_tail(&ctx->wal_link, &c->wal_pending);
	c->wal_pending_count++;
	c->wal_pending_buffered_count++;
	c->wal_buffered_count++;
	c->wal_buffered_acks++;
	kick = c->wal_pending_count >= wal_group_capacity() ||
		c->memtable->count >= flush_threshold;
	if (kick)
		mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
	spin_unlock_irq(&c->lock);
	zone_dispatch_cancel(c, phys, nr);
	foreground_write_done(c);
	bio_endio(orig);
	return true;
}

/* 완료된 PUT을 WAL 배치에 넣는다. 일반 쓰기는 조기 완료를 시도하고, 나머지는 WAL 기록 뒤 완료한다.
 * process·atomic context에서 모두 호출되므로 이 경로에서 sleep하면 안 된다. */
static void submit_wal_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	bool full;

	if (wal_try_buffer_write(ctx))
		return;

	INIT_LIST_HEAD(&ctx->wal_link);
	if (metrics_enabled && wal_timing_enabled)
		ctx->diag_wal_queued_ns = ktime_get_ns();
	spin_lock_irq(&c->lock);
	if (ctx->diag_wal_queued_ns && !c->wal_batch_busy &&
	    !c->wal_pending_count && !c->diag_wal_ready_ns)
		c->diag_wal_ready_ns = ctx->diag_wal_queued_ns;
	list_add_tail(&ctx->wal_link, &c->wal_pending);
	full = ++c->wal_pending_count >= wal_group_capacity();
	/* 최초 예약의 deadline을 유지한다. 매 요청마다 mod하면 연속 도착이
	 * 설정한 최대 배치 지연을 계속 연장한다. full 배치만 즉시 당긴다.
	 * lock 안에서 예약해 enqueue와 worker drain 사이의 순서도 보존한다. */
	if (full)
		mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
	else
		queue_delayed_work(zns_wal_wq, &c->wal_batch_work,
				   msecs_to_jiffies(wal_batch_delay_ms));
	spin_unlock_irq(&c->lock);
}

/* 현재 zone header를 비동기 제출하고 완료 콜백에서 다음 작업으로 연결한다.
 * .map()과 완료 콜백에서 사용하므로 블로킹과 sleep 가능한 할당은 금지한다. */
static void submit_header_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	struct pending_header *h = &ctx->headers[ctx->header_idx];
	struct zone_header *hdr;
	struct bio *bio;
	struct page *page;

	hdr = kzalloc(512, GFP_ATOMIC);
	if (!hdr)
		goto skip_header;
	hdr->magic = cpu_to_le32(ZONE_HEADER_MAGIC);
	hdr->tag = cpu_to_le32(h->tag);
	hdr->gen = cpu_to_le64(h->gen); /* WAL zone이면 generation, 아니면 0 */
	ctx->hdr_buf = hdr;

	bio = bio_alloc(GFP_ATOMIC, 1);
	if (!bio) {
		kfree(hdr);
		ctx->hdr_buf = NULL;
		goto skip_header;
	}
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = (sector_t)h->zone_id * c->zp->zone_sectors;
	bio->bi_opf = REQ_OP_WRITE;
	page = virt_to_page(hdr);
	bio_add_page(bio, page, 512, offset_in_page(hdr));
	bio->bi_end_io = header_write_done;
	bio->bi_private = ctx;
	/* 첫 섹터의 header가 후속 쓰기보다 먼저 제출되도록 dispatch gate를 사용한다. */
	zone_dispatch_write(c, bio->bi_iter.bi_sector, 1, bio);
	return;

skip_header:
	DMERR("zone header allocation failed (zone %u, tag %u): aborting dependent write",
	      h->zone_id, h->tag);
	abort_after_header_failure(ctx, BLK_STS_RESOURCE, true);
}

/* 헤더 하나가 durable하게 쓰인 뒤 호출. 남은 헤더가 있으면 이어서, 없으면 ctx->on_headers_done으로 넘어간다. */
static void header_write_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	blk_status_t status = bio->bi_status;
	unsigned int zid = ctx->headers[ctx->header_idx].zone_id;

	kfree(ctx->hdr_buf);
	bio_put(bio);

	if (status) {
		DMERR("zone header write failed (zone %u, tag %u): aborting dependent write",
		      ctx->headers[ctx->header_idx].zone_id,
		      ctx->headers[ctx->header_idx].tag);
		abort_after_header_failure(ctx, status, false);
		return;
	}
	zone_append_header_done(ctx->c, zid, 0);

	ctx->header_idx++;
	if (ctx->header_idx < ctx->nr_headers)
		submit_header_async(ctx);
	else
		ctx->on_headers_done(ctx);
}

static void submit_data_append_async(struct zns_io_ctx *ctx)
{
	struct bio *bio = ctx->orig_bio;
	unsigned int zid = zone_of(ctx->c->zp, ctx->reserved_phys);

	if (metrics_enabled && wal_timing_enabled)
		ctx->diag_data_start_ns = ktime_get_ns();
	if (ctx->extent_count) {
		/* 부모 bio는 WAL 자식들의 완료 집계용으로 보유한다. 별도 data
		 * clone을 제출해 이미 끝난 bio의 내부 reference를 재설정하지 않는다. */
		bio = bio_clone_fast(ctx->orig_bio, GFP_ATOMIC, &fs_bio_set);
		if (!bio) {
			abort_after_header_failure(ctx, BLK_STS_RESOURCE, false);
			return;
		}
		bio_set_dev(bio, ctx->c->dev->bdev);
		bio->bi_iter.bi_sector = (sector_t)zid * ctx->c->zp->zone_sectors;
		bio->bi_opf = (bio->bi_opf & ~REQ_OP_MASK) | REQ_OP_ZONE_APPEND;
		bio->bi_end_io = extent_append_done;
		bio->bi_private = ctx;
		if (zone_append_write(ctx->c, zid, bio)) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
		}
		return;
	}

	ctx->orig_sector = bio->bi_iter.bi_sector;
	ctx->orig_opf = bio->bi_opf;
	ctx->orig_end_io = bio->bi_end_io;
	ctx->orig_private = bio->bi_private;
	bio_set_dev(bio, ctx->c->dev->bdev);
	bio->bi_iter.bi_sector = (sector_t)zid * ctx->c->zp->zone_sectors;
	bio->bi_opf = (bio->bi_opf & ~REQ_OP_MASK) | REQ_OP_ZONE_APPEND;
	bio->bi_end_io = data_append_done;
	bio->bi_private = ctx;
	if (zone_append_write(ctx->c, zid, bio)) {
		bio->bi_status = BLK_STS_RESOURCE;
		bio_endio(bio);
	}
}

static void extent_append_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	struct zns_base_c *c = ctx->c;
	struct bio *orig = ctx->orig_bio;
	sector_t base = bio->bi_iter.bi_sector;
	blk_status_t status = bio->bi_status;
	unsigned int i;

	bio_put(bio);
	if (!status && failpoint_hit(1))
		status = BLK_STS_IOERR;
	if (status) {
		zone_quarantine(c, zone_of(c->zp, ctx->reserved_phys), ZONE_TAG_USER_DATA, status);
		free_extent_children(ctx);
		zone_dispatch_cancel(c, ctx->reserved_phys, ctx->reserved_nr);
		kfree(ctx);
		orig->bi_status = status;
		foreground_write_done(c);
		bio_endio(orig);
		return;
	}
	if (ctx->diag_data_start_ns) {
		atomic64_inc(&c->wal_diag[WD_DATA_COUNT]);
		atomic64_add(ktime_get_ns() - ctx->diag_data_start_ns, &c->wal_diag[WD_DATA_NS]);
	}
	atomic_add(ctx->extent_count - 1, &c->foreground_writes);
	/* 모든 자식을 먼저 연결해야 첫 WAL 완료가 부모를 조기 해제하지 않는다. */
	for (i = 0; i < ctx->extent_count; i++)
		bio_chain(ctx->extent_children[i]->orig_bio, orig);
	for (i = 0; i < ctx->extent_count; i++) {
		struct zns_io_ctx *child = ctx->extent_children[i];
		child->phys = base + (sector_t)i * BLOCK_SECTORS;
		submit_wal_async(child);
	}
	kfree(ctx->extent_children);
	kfree(ctx);
	bio_endio(orig);
}

/* Zone Append가 반환한 실제 주소를 WAL에 기록한다.
 * 이 완료만으로 데이터 영속화를 보장하지 않는다. WAL worker가 데이터를 flush한 뒤 복구 포인터를 영속화한다. */
static void data_append_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	struct zns_base_c *c = ctx->c;

	if (!bio->bi_status && failpoint_hit(1))
		bio->bi_status = BLK_STS_IOERR;

	if (bio->bi_status) {
		DMERR("foreground data append failed: err=%d zone=%u reserved=%llu+%llu returned=%llu lba=%llu",
		      blk_status_to_errno(bio->bi_status),
		      zone_of(c->zp, ctx->reserved_phys),
		      (unsigned long long)ctx->reserved_phys,
		      (unsigned long long)ctx->reserved_nr,
		      (unsigned long long)bio->bi_iter.bi_sector,
		      (unsigned long long)ctx->lba);
		zone_dispatch_cancel(c, ctx->reserved_phys, ctx->reserved_nr);
		bio->bi_end_io = ctx->orig_end_io;
		bio->bi_private = ctx->orig_private;
		bio->bi_iter.bi_sector = ctx->orig_sector;
		bio->bi_opf = ctx->orig_opf;
		kfree(ctx);
		foreground_write_done(c);
		bio_endio(bio);
		return;
	}
	ctx->phys = bio->bi_iter.bi_sector;
	if (ctx->diag_data_start_ns) {
		atomic64_inc(&c->wal_diag[WD_DATA_COUNT]);
		atomic64_add(ktime_get_ns() - ctx->diag_data_start_ns,
			     &c->wal_diag[WD_DATA_NS]);
	}
	bio->bi_end_io = ctx->orig_end_io;
	bio->bi_private = ctx->orig_private;
	bio->bi_iter.bi_sector = ctx->orig_sector;
	bio->bi_opf = ctx->orig_opf;

	submit_wal_async(ctx);
}

/* ctx에 원본 bio가 없으면 이미 완료한 volatile PUT이다. 매핑은 건드리지
 * 않고 WAL 소유권만 해제한다. 일반 쓰기의 지연 오류는 이후 barrier로 알린다. */
static bool wal_complete_buffered(struct zns_io_ctx *ctx, blk_status_t status)
{
	struct zns_base_c *c = ctx->c;

	if (ctx->orig_bio)
		return false;
	spin_lock_irq(&c->lock);
	c->wal_buffered_count--;
	if (status) {
		c->wal_buffered_errors++;
		c->metadata_failed = true;
		c->checkpoint_failed = true;
	} else {
		c->wal_buffered_commits++;
	}
	spin_unlock_irq(&c->lock);
	kfree(ctx);
	return true;
}

/* WAL 완료를 기다리는 요청의 매핑과 원본 bio를 완료한다.
 * 이미 조기 완료한 buffered PUT은 매핑을 다시 공개하지 않고 WAL 소유권만 해제한다. */
static void wal_commit_ctx(struct zns_io_ctx *ctx, blk_status_t wal_status,
			   bool allow_flush)
{
	struct zns_base_c *c = ctx->c;
	struct bio *orig = ctx->orig_bio;
	u64 lba = ctx->lba;
	sector_t phys = ctx->phys;
	sector_t reserved_phys = ctx->reserved_phys;
	sector_t reserved_nr = ctx->reserved_nr;
	struct memtable_flush_work *flush_work = NULL;
	bool is_discard = ctx->is_discard;
	u64 discard_blocks = ctx->discard_blocks;
	u64 discard_i;
	u64 diag_commit_start = 0;
	int ret;

	if (wal_complete_buffered(ctx, wal_status))
		return;

	if (wal_status) {
		if (reserved_nr)
			zone_dispatch_cancel(c, reserved_phys, reserved_nr);
		kfree(ctx);
		orig->bi_status = wal_status;
		foreground_write_done(c);
		bio_endio(orig);
		return;
	}

	/* commit 처리 구간: lock 대기, mapping 공개, dispatch 회계 포함. */
	if (ctx->diag_wal_queued_ns)
		diag_commit_start = ktime_get_ns();
	spin_lock_irq(&c->lock);
	ret = 0;
	if (is_discard) {
		for (discard_i = 0; discard_i < discard_blocks; discard_i++) {
			ret = mapping_put(c, lba + discard_i * BLOCK_SECTORS,
					  MAPPING_TOMBSTONE);
			if (ret)
				break;
		}
	} else {
		ret = mapping_put(c, lba, phys);
	}
	if (!ret && allow_flush)
		flush_work = schedule_memtable_flush_locked(c, false);
	spin_unlock_irq(&c->lock);
	/* Zone Append는 일반 dispatch gate를 통과하지 않으므로 완료 시점에
	 * 예약 순서 회계를 직접 전진시킨다. data는 mapping_put 뒤에 완료
	 * 처리해야 dispatch_wp==wp가 곧 "모든 매핑 공개 완료"를 뜻한다. */
	if (reserved_nr)
		zone_dispatch_cancel(c, reserved_phys, reserved_nr);
	if (diag_commit_start && !ret) {
		u64 now = ktime_get_ns();

		atomic64_inc(&c->wal_diag[WD_COMMIT_COUNT]);
		atomic64_add(now - diag_commit_start, &c->wal_diag[WD_COMMIT_NS]);
		atomic64_add(now - ctx->diag_wal_queued_ns, &c->wal_diag[WD_ACK_NS]);
	}
	kfree(ctx);

	if (ret) {
		orig->bi_status = BLK_STS_RESOURCE;
		foreground_write_done(c);
		bio_endio(orig);
		return;
	}

	/* SSTable flush는 별도 작업으로 진행한다. 이 요청의 WAL은 이미 영속화됐으므로 bio를 완료할 수 있다. */
	if (flush_work)
		queue_work(zns_flush_wq, &flush_work->work);

	foreground_write_done(c);
	bio_endio(orig);
}

static int wal_page_append_sync(struct zns_base_c *c, sector_t reserved_phys,
				void *buf, sector_t nr_sectors, bool fua)
{
	struct bio *bio = bio_alloc(GFP_KERNEL, 1);
	u64 started;
	int ret;

	if (!bio)
		return -ENOMEM;
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = (sector_t)zone_of(c->zp, reserved_phys) *
				 c->zp->zone_sectors;
	bio->bi_opf = REQ_OP_ZONE_APPEND | REQ_SYNC | (fua ? REQ_FUA : 0);
	if (fua && wal_group_pages > 1)
		bio->bi_opf |= REQ_PREFLUSH; /* 앞선 non-FUA WAL page도 영속화 */
	if (bio_add_page(bio, virt_to_page(buf), nr_sectors * 512, 0) !=
	    nr_sectors * 512) {
		bio_put(bio);
		zone_dispatch_cancel(c, reserved_phys, nr_sectors);
		return -EIO;
	}
	started = metrics_enabled && wal_timing_enabled ? ktime_get_ns() : 0;
	zone_dispatch_wait_turn(c, reserved_phys, nr_sectors);
	if (started) {
		atomic64_inc(&c->wal_diag[WD_DISPATCH_COUNT]);
		atomic64_add(ktime_get_ns() - started, &c->wal_diag[WD_DISPATCH_NS]);
	}
	if (metrics_enabled)
		atomic64_add((u64)nr_sectors * 512, &c->metric_write_bytes[ZONE_TAG_WAL]);
	started = metrics_enabled && wal_timing_enabled ? ktime_get_ns() : 0;
	ret = submit_bio_wait(bio);
	if (started) {
		atomic64_inc(&c->wal_diag[WD_SUBMIT_COUNT]);
		atomic64_add(ktime_get_ns() - started, &c->wal_diag[WD_SUBMIT_NS]);
	}
	bio_put(bio);
	return ret;
}

/* 호출자가 c->lock을 보유해야 한다. GC에 예약한 WAL tail을 foreground가 소비하지 못하게 한다. */
static bool foreground_wal_allowed(struct zns_base_c *c, sector_t nr)
{
	unsigned int z = c->zp->active_zone[ZONE_TAG_WAL];
	sector_t tail = z == ZONE_NONE ? 0 : c->zp->zone_sectors - c->zp->wp[z];
	sector_t protected = max(c->gc_wal_budget, c->gc_wal_reserve);

	/* 회전 중에는 새 data 할당을 막고 이미 허용된 요청만 drain한다.
	 * 이 요청까지 WAL reserve로 막으면 foreground_writes가 0이 되지 않아 회전이 교착된다. */
	if (c->wal_rotation_pending)
		return tail >= nr;

	return !protected ||
		gc_count_free_zones(c->zp) > gc_reserved_zones ||
		tail >= protected + nr;
}

static void wal_batch_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(to_delayed_work(work),
					     struct zns_base_c, wal_batch_work);
	LIST_HEAD(batch);
	struct zns_io_ctx *ctx, *tmp;

	struct wal_page *page;
	struct wal_page *pages[16] = { 0 };
	unsigned int p, nr_pages, pos;
	sector_t wal_phys = 0;
	int new_zone = -1;
	unsigned int count = 0;
	sector_t wal_sectors;
	int ret;
	bool more;

	struct memtable_flush_work *deferred_flush = NULL;
	bool should_gc;

	u64 diag_io_start;
	u64 diag_worker_start = metrics_enabled && wal_timing_enabled ? ktime_get_ns() : 0;
	u64 diag_complete_start;

	ret = 0;
	for (p = 0; p < wal_group_pages; p++) {
		pages[p] = (struct wal_page *)get_zeroed_page(GFP_KERNEL);
		if (!pages[p]) {
			ret = -ENOMEM;
			break;
		}
	}
	page = pages[0];

	spin_lock_irq(&c->lock);
	c->wal_batch_busy = true;
	c->wal_batch_has_waiters = false;
	/* 이전 배치 처리 중 backlog 대기는 제외한다. 설정 timer, workqueue,
	 * CPU scheduling, 입구 page 할당/lock 대기를 포함하므로 순수 scheduler 지연은 아니다. */
	if (diag_worker_start && c->diag_wal_ready_ns) {
		u64 elapsed = ktime_get_ns() - c->diag_wal_ready_ns;

		atomic64_inc(&c->wal_diag[WD_READY_COUNT]);
		atomic64_add(elapsed, &c->wal_diag[WD_READY_NS]);
		if (elapsed >= 20000000ULL)
			atomic64_inc(&c->wal_diag[WD_READY_SLOW]);
		c->diag_wal_ready_ns = 0;
	}
	while (!list_empty(&c->wal_pending) && count < wal_group_capacity()) {
		ctx = list_first_entry(&c->wal_pending, struct zns_io_ctx, wal_link);
		list_move_tail(&ctx->wal_link, &batch);
		c->wal_pending_count--;
		if (!ctx->orig_bio)
			c->wal_pending_buffered_count--;
		else
			c->wal_batch_has_waiters = true;
		count++;
	}
	more = !list_empty(&c->wal_pending);
	nr_pages = DIV_ROUND_UP(count, wal_batch_max_records);
	wal_sectors = count == 1 ? 1 : nr_pages * WAL_PAGE_SECTORS;
	if (c->metadata_failed)
		ret = -EIO;
	if (!ret && count) {
		if (!foreground_wal_allowed(c, wal_sectors)) {
			ret = -ENOSPC;
		} else if (unlikely(wal_batch_alloc_failures)) {
			wal_batch_alloc_failures--;
			ret = -ENOSPC;
		} else {
			ret = zone_pool_alloc(c->zp, ZONE_TAG_WAL, wal_sectors,
					      &wal_phys, &new_zone, false);
		}
	}
	spin_unlock_irq(&c->lock);

	if (!count)
		goto out;
	if (ret == -ENOSPC) {
		bool stopping;

		/* 원본 bio를 보유한 요청은 공간 부족을 일시 상태로 취급한다.
		 * pending 앞에 원래 순서대로 되돌리고 GC 회수 뒤 WAL 기록을 재시도한다. */
		spin_lock_irq(&c->lock);
		/* volatile overlay가 있으면 GC를 먼저 실행해 이전 복구 경로를
		 * 지울 수 없다. buffered 공간 고갈은 fail-closed한다. */
		stopping = c->stopping || c->wal_buffered_count;
		if (!stopping) {
			list_splice_init(&batch, &c->wal_pending);
			c->wal_pending_count += count;
			/* buffered ENOSPC는 위에서 fail-closed하므로 이 경로는
			 * 원본 bio를 보유한 기존 durable waiter만 되돌린다. */
			c->wal_batch_busy = false;
		}
		spin_unlock_irq(&c->lock);
		if (!stopping) {
			for (p = 0; p < wal_group_pages; p++)
				if (pages[p])
					free_page((unsigned long)pages[p]);
			queue_work(zns_gc_wq, &c->gc_work);
			mod_delayed_work(zns_wal_wq, &c->wal_batch_work,
					 msecs_to_jiffies(100));
			return;
		}
		/* target 종료 중에는 더 이상 재큐잉할 수 없으므로 아래 공통
		 * 완료 경로에서 오류로 정리한다. */
	}
	if (ret)
		goto complete;

	if (metrics_enabled && wal_timing_enabled) {
		u64 now = ktime_get_ns();
		unsigned int bucket = count == 1 ? WD_BATCH_1 :
				      count <= 8 ? WD_BATCH_2_8 :
				      count <= 32 ? WD_BATCH_9_32 : WD_BATCH_33_PLUS;
		atomic64_inc(&c->wal_diag[WD_BATCHES]);
		atomic64_add(count, &c->wal_diag[WD_RECORDS]);
		atomic64_inc(&c->wal_diag[bucket]);
		list_for_each_entry(ctx, &batch, wal_link)
			atomic64_add(now - ctx->diag_wal_queued_ns, &c->wal_diag[WD_QUEUE_NS]);
	}
	count = 0;
	list_for_each_entry(ctx, &batch, wal_link) {
		struct wal_record *rec;

		p = count / wal_batch_max_records;
		pos = count % wal_batch_max_records;
		rec = wal_sectors == 1 ? (struct wal_record *)page : &pages[p]->records[pos];

		rec->type = cpu_to_le32(ctx->is_discard ? WAL_REC_DELETE_RANGE : WAL_REC_PUT);
		rec->put.lba = cpu_to_le64(ctx->lba);
		rec->put.phys = cpu_to_le64(ctx->is_discard ? ctx->discard_blocks : ctx->phys);
		count++;
	}
	if (wal_sectors > 1) {
		for (p = 0; p < nr_pages; p++) {
			pages[p]->magic = cpu_to_le32(WAL_PAGE_MAGIC);
			pages[p]->version = cpu_to_le16(WAL_PAGE_VERSION);
			pages[p]->count = cpu_to_le16(min(wal_batch_max_records,
					count - p * wal_batch_max_records));
			pages[p]->crc32 = 0;
			pages[p]->crc32 = cpu_to_le32(crc32(~0U, pages[p], PAGE_SIZE));
		}
	}

	if (new_zone >= 0) {
		struct zone_header *hdr = kzalloc(512, GFP_KERNEL);

		if (!hdr) {
			zone_dispatch_cancel(c, (sector_t)new_zone * c->zp->zone_sectors, 1);
			ret = -ENOMEM;
		} else {
			hdr->magic = cpu_to_le32(ZONE_HEADER_MAGIC);
			hdr->tag = cpu_to_le32(ZONE_TAG_WAL);
			hdr->gen = cpu_to_le64(c->zp->wal_gen[new_zone]);
			zone_dispatch_wait_turn(c,
				(sector_t)new_zone * c->zp->zone_sectors, 1);
			ret = sstable_io_sync(c, REQ_OP_WRITE,
				(sector_t)new_zone * c->zp->zone_sectors, hdr, 1);
			kfree(hdr);
		}
		if (ret) {
			zone_quarantine(c, new_zone, ZONE_TAG_WAL, BLK_STS_IOERR);
			zone_dispatch_cancel(c, wal_phys, wal_sectors);
		} else
			zone_append_header_done(c, new_zone, 0);
	}
	if (!ret) {
		diag_io_start = metrics_enabled && wal_timing_enabled ? ktime_get_ns() : 0;
		/* FUA는 이전 데이터 쓰기를 flush하지 않는다. 데이터/header가
		 * 영속화된 다음에만 복구 가능한 WAL 포인터를 게시한다. */
		ret = blkdev_issue_flush(c->dev->bdev);
		if (!ret && failpoint_hit(2))
			ret = -EIO;
		if (!ret && nr_pages == 1)
			ret = wal_page_append_sync(c, wal_phys, page, wal_sectors, true);
		else if (ret)
			zone_dispatch_cancel(c, wal_phys, wal_sectors);
		else {
			for (p = 0; p < nr_pages; p++) {
				ret = wal_page_append_sync(c, wal_phys + p * WAL_PAGE_SECTORS,
					pages[p], WAL_PAGE_SECTORS, p == nr_pages - 1);
				if (ret) {
					if (p + 1 < nr_pages)
						zone_dispatch_cancel(c, wal_phys + (p + 1) * WAL_PAGE_SECTORS,
							(nr_pages - p - 1) * WAL_PAGE_SECTORS);
					break;
				}
			}
		}
		if (diag_io_start) {
			atomic64_inc(&c->wal_diag[WD_IO_COUNT]);
			atomic64_add(ktime_get_ns() - diag_io_start, &c->wal_diag[WD_IO_NS]);
			if (ret)
				atomic64_inc(&c->wal_diag[WD_IO_ERRORS]);
		}
		if (ret)
			DMERR("foreground WAL append failed: err=%d zone=%u reserved=%llu+%llu records=%u new_zone=%d",
			      ret, zone_of(c->zp, wal_phys),
			      (unsigned long long)wal_phys,
			      (unsigned long long)wal_sectors, count, new_zone);
	}

complete:
	if (ret) {
		spin_lock_irq(&c->lock);
		c->metadata_failed = true;
		c->checkpoint_failed = true;
		spin_unlock_irq(&c->lock);
	}
	diag_complete_start = diag_worker_start ? ktime_get_ns() : 0;
	list_for_each_entry_safe(ctx, tmp, &batch, wal_link) {
		bool last = list_is_last(&ctx->wal_link, &batch);

		list_del_init(&ctx->wal_link);
		wal_commit_ctx(ctx, ret ? BLK_STS_IOERR : 0, last);
	}
	if (diag_complete_start) {
		u64 elapsed = ktime_get_ns() - diag_complete_start;

		atomic64_inc(&c->wal_diag[WD_COMPLETE_COUNT]);
		atomic64_add(elapsed, &c->wal_diag[WD_COMPLETE_NS]);
		if (elapsed >= 20000000ULL)
			atomic64_inc(&c->wal_diag[WD_COMPLETE_SLOW]);
	}
out:
	for (p = 0; p < wal_group_pages; p++)
		if (pages[p])
			free_page((unsigned long)pages[p]);
	spin_lock_irq(&c->lock);
	c->wal_batch_busy = false;
	c->wal_batch_has_waiters = false;
	more = !list_empty(&c->wal_pending);
	if (!more)
		deferred_flush = schedule_memtable_flush_locked(c, false);
	should_gc = !c->metadata_failed &&
		gc_count_free_zones(c->zp) <= gc_low_watermark;
	/* 다음 volatile 그룹은 full/명시적 요청/내부 경계까지 대기한다.
	 * 한 그룹을 기록했다는 이유로 뒤의 partial 그룹을 강제하지 않는다. */
	if (more &&
	    c->wal_pending_count == c->wal_pending_buffered_count &&
	    c->wal_pending_count < wal_group_capacity() &&
	    list_empty(&c->pending_flush_bios) && !c->wal_rotation_pending &&
	    !c->stopping && !c->metadata_failed && !should_gc &&
	    c->memtable->count < flush_threshold)
		more = false;
	if (diag_worker_start) {
		u64 now = ktime_get_ns();

		atomic64_inc(&c->wal_diag[WD_WORKER_COUNT]);
		atomic64_add(now - diag_worker_start, &c->wal_diag[WD_WORKER_NS]);
		if (now - diag_worker_start >= 20000000ULL)
			atomic64_inc(&c->wal_diag[WD_WORKER_SLOW]);
		if (more)
			c->diag_wal_ready_ns = now;
	}
	spin_unlock_irq(&c->lock);
	wake_up_all(&c->flush_waitq);
	if (deferred_flush)
		queue_work(zns_flush_wq, &deferred_flush->work);
	if (should_gc)
		queue_work(zns_gc_wq, &c->gc_work);
	if (more)
		mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
	else
		mod_delayed_work(zns_wq, &c->pending_flush_work, 0);
}

/* flush bio를 DM core에 반복 반환하면 대형-zone GC 동안 재큐잉이 수만 번
 * 누적되어 상위 파일시스템에 IOERR로 끝날 수 있다. 매핑 PUT의 WAL이 전부
 * durable해진 뒤에만 하위 장치로 제출하되, 그동안 bio 소유권은 타깃이
 * 유지한다. 이 worker는 기다리지 않고 재예약하며 WAL 전용 큐의
 * 실행 슬롯을 점유하지 않는다. */
static void pending_flush_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(to_delayed_work(work),
					     struct zns_base_c, pending_flush_work);
	LIST_HEAD(ready);
	struct pending_flush_bio *pending, *tmp;
	bool wal_busy;
	bool failed;

	spin_lock_irq(&c->lock);
	failed = c->metadata_failed;
	/* 이 worker는 WAL 완료 뒤에도 호출된다. 실제 barrier가 없는데
	 * foreground가 있다는 이유만으로 partial volatile 그룹을 밀면
	 * 요청 기반 경계가 사라지고 매 배치 FUA 정책으로 되돌아간다. */
	if (list_empty(&c->pending_flush_bios)) {
		spin_unlock_irq(&c->lock);
		return;
	}
	wal_busy = c->wal_batch_busy || !list_empty(&c->wal_pending) ||
		!list_empty(&c->pending_write_bios) ||
		atomic_read(&c->foreground_writes) > 0;
	if (!wal_busy || failed)
		list_splice_init(&c->pending_flush_bios, &ready);
	spin_unlock_irq(&c->lock);

	if (wal_busy && !failed) {
		mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
		mod_delayed_work(zns_wq, &c->pending_flush_work,
				 msecs_to_jiffies(20));
		return;
	}

	list_for_each_entry_safe(pending, tmp, &ready, link) {
		list_del(&pending->link);
		if (failed) {
			pending->bio->bi_status = BLK_STS_IOERR;
			bio_endio(pending->bio);
		} else {
			bio_set_dev(pending->bio, c->dev->bdev);
			submit_bio_deferred(pending->bio);
		}
		kfree(pending);
	}
}

/* 성공·실패와 관계없이 flush 체인마다 한 번 호출해 checkpoint in-flight 수를 줄인다.
 * 모든 checkpoint가 완료되고 실패가 없을 때만 WAL 회수 frontier를 전진시킨다. */
static void flush_chain_end(struct zns_base_c *c)
{
	bool advanced = false;
	bool need_gc;
	u64 started = metrics_enabled ? atomic64_xchg(&c->metric_flush_started_ns, 0) : 0;

	if (started)
		atomic64_add(ktime_get_ns() - started, &c->metric_flush_ns);

	spin_lock_irq(&c->lock);
	if (--c->wal_ckpt_inflight == 0 && !c->checkpoint_failed &&
	    c->wal_highest_issued_split_gen > c->wal_durable_split_gen) {
		c->wal_durable_split_gen = c->wal_highest_issued_split_gen;
		advanced = true;
	}
	need_gc = c->wal_ckpt_inflight == 0 &&
		gc_count_free_zones(c->zp) <= gc_low_watermark;
	spin_unlock_irq(&c->lock);
	wake_up_all(&c->flush_waitq);

	if (advanced)
		queue_work(zns_wal_reclaim_wq, &c->wal_reclaim_work);
	/* GC가 frozen memtable 때문에 연기됐다면 마지막 flush가
	 * 끝나는 시점에 다시 실행. 새 write가 와야만 재시도되는 행을 막는다. */
	if (need_gc)
		queue_work(zns_gc_wq, &c->gc_work);
}

/* SSTable 청크 기록 종료를 처리한다. 성공하면 색인을 공개하고 checkpoint를 기록한다.
 * 실패하면 checkpoint 없이 frozen map을 보존한다. 완료된 bio는 호출자가 해제한 상태다. */
static void sstable_flush_complete(struct zns_io_ctx *ctx, blk_status_t status)
{
	struct zns_base_c *c = ctx->c;
	sector_t wal_phys;
	int new_wal_zone;
	int ret;

	if (!status && failpoint_hit(3))
		status = BLK_STS_IOERR;

	if (status) {
		DMERR("SSTable flush write failed (seq=%llu): this generation's data is lost from the SSTable, but remains recoverable from WAL replay",
		      (unsigned long long)ctx->checkpoint_seq);
	} else {
		struct sstable_header *hdr = ctx->sstable_buf;
		int rret;
		bool should_compact;

		/* 기록을 마친 SSTable을 읽기 경로에 공개한다. checkpoint 전이어도 데이터는 영속화된 상태다. */
		spin_lock_irq(&c->lock);
		rret = sstable_register(c, ctx->sstable_phys, le64_to_cpu(hdr->seq_no),
					  le64_to_cpu(hdr->record_count),
					  le64_to_cpu(hdr->min_lba), le64_to_cpu(hdr->max_lba), GFP_ATOMIC);
		if (!rret) {
			c->frozen_published = true;
		} else {
			status = BLK_STS_RESOURCE;
		}
		should_compact = !rret && c->nr_sstables >= compaction_k;
		spin_unlock_irq(&c->lock);
		if (rret)
			DMERR("SSTable flush: failed to register index (seq=%llu); retaining frozen map and failing I/O closed",
			      (unsigned long long)ctx->checkpoint_seq);

		/* 완료 콜백에서는 작업만 큐잉하고 실제 병합은 compaction worker에서 수행한다. */
		if (should_compact)
			queue_work(zns_compaction_wq, &c->compaction_work);
	}

	kvfree(ctx->sstable_buf);

	if (status) {
		release_frozen_memtable(c, ctx->old_memtable, false);
		kfree(ctx);
		flush_chain_end(c);
		return;
	}

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_WAL, 1, &wal_phys, &new_wal_zone, false);
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("checkpoint alloc failed (%d, seq=%llu): replay will just do extra work next time, no data lost",
		      ret, (unsigned long long)ctx->checkpoint_seq);
		release_frozen_memtable(c, ctx->old_memtable, false);
		kfree(ctx);
		flush_chain_end(c);
		return;
	}

	ctx->wal_phys = wal_phys;
	ctx->nr_headers = 0;
	if (new_wal_zone >= 0) {
		ctx->headers[ctx->nr_headers].zone_id = new_wal_zone;
		ctx->headers[ctx->nr_headers].tag = ZONE_TAG_WAL;
		ctx->headers[ctx->nr_headers].gen = c->zp->wal_gen[new_wal_zone];
		ctx->nr_headers++;
	}
	ctx->header_idx = 0;
	ctx->on_headers_done = submit_checkpoint_async;

	if (ctx->nr_headers > 0)
		submit_header_async(ctx);
	else
		submit_checkpoint_async(ctx);
}

/* CHECKPOINT(seq_no, split_gen, split_off) 레코드를 비동기로 append.
 * flush 체인의 마지막 단계, GFP_ATOMIC 원칙은 submit_wal_async와 동일. */
static void submit_checkpoint_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	struct wal_record *rec;
	struct bio *bio;
	struct page *page;

	if (failpoint_hit(4)) {
		zone_dispatch_cancel(c, ctx->wal_phys, 1);
		release_frozen_memtable(c, ctx->old_memtable, false);
		kfree(ctx);
		flush_chain_end(c);
		return;
	}

	rec = kzalloc(512, GFP_ATOMIC);
	if (!rec) {
		DMERR("checkpoint write: out of memory (seq=%llu), skipping — replay will just do extra work next time",
		      (unsigned long long)ctx->checkpoint_seq);
		zone_dispatch_cancel(c, ctx->wal_phys, 1);
		release_frozen_memtable(c, ctx->old_memtable, false);
		kfree(ctx);
		flush_chain_end(c);
		return;
	}
	rec->type = cpu_to_le32(WAL_REC_CHECKPOINT);
	rec->checkpoint.seq_no = cpu_to_le64(ctx->checkpoint_seq);
	rec->checkpoint.split_gen = cpu_to_le64(ctx->checkpoint_split_gen);
	rec->checkpoint.split_off = cpu_to_le64(ctx->checkpoint_split_off);
	ctx->wal_buf = rec;

	bio = bio_alloc(GFP_ATOMIC, 1);
	if (!bio) {
		kfree(rec);
		ctx->wal_buf = NULL;
		zone_dispatch_cancel(c, ctx->wal_phys, 1);
		release_frozen_memtable(c, ctx->old_memtable, false);
		kfree(ctx);
		flush_chain_end(c);
		return;
	}
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = (zone_of(c->zp, ctx->wal_phys) * c->zp->zone_sectors);
	bio->bi_opf = REQ_OP_ZONE_APPEND | REQ_PREFLUSH | REQ_FUA;
	page = virt_to_page(rec);
	bio_add_page(bio, page, 512, offset_in_page(rec));
	bio->bi_end_io = checkpoint_write_done;
	bio->bi_private = ctx;
	if (zone_append_write(c, zone_of(c->zp, ctx->wal_phys), bio)) {
		bio->bi_status = BLK_STS_RESOURCE;
		bio_endio(bio);
	}
}

/* checkpoint 결과를 반영하고 flush 체인을 종료한다. frozen map 해제 여부는 공개 상태와 오류에 따라 판단한다. */
static void checkpoint_write_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	struct zns_base_c *c = ctx->c;
	blk_status_t status = bio->bi_status;
	sector_t wal_phys = ctx->wal_phys;

	kfree(ctx->wal_buf);
	bio_put(bio);
	zone_dispatch_cancel(c, wal_phys, 1);

	if (status)
		DMERR("checkpoint write failed (seq=%llu): replay will just do extra work next time, no data lost",
		      (unsigned long long)ctx->checkpoint_seq);
	else
		DMINFO("checkpoint written (seq=%llu, split_gen=%llu, split_off=%llu)",
		       (unsigned long long)ctx->checkpoint_seq,
		       (unsigned long long)ctx->checkpoint_split_gen,
		       (unsigned long long)ctx->checkpoint_split_off);

	release_frozen_memtable(c, ctx->old_memtable, !status);
	kfree(ctx);
	flush_chain_end(c);
}

/* 직렬화 버퍼를 페이지 경계에 맞춰 bio에 붙인다. 마지막 페이지는 남은 바이트만 기록한다. */
static void submit_sstable_write_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	sector_t remaining_sectors = ctx->sstable_nr_sectors - ctx->sstable_done_sectors;
	sector_t chunk_sectors = min_t(sector_t, remaining_sectors, SSTABLE_IO_MAX_SECTORS);
	sector_t chunk_phys = ctx->sstable_phys + ctx->sstable_done_sectors;
	size_t off_bytes = (size_t)ctx->sstable_done_sectors * 512;
	size_t chunk_bytes = (size_t)chunk_sectors * 512;
	unsigned int nr_pages = DIV_ROUND_UP(chunk_bytes, PAGE_SIZE);
	size_t remaining = chunk_bytes;
	struct bio *bio;
	unsigned int i;

	/* BIO_MAX_VECS 한도에 맞춰 청크를 제출한다. 각 완료 콜백에서 다음 청크를 이어서 제출한다. */
	bio = bio_alloc(GFP_ATOMIC, nr_pages);
	if (!bio) {
		DMERR("SSTable flush: bio_alloc failed; retaining frozen map and failing I/O closed");
		/* 이미 배정된 SSTable phys 중 아직 안 나간 부분은 취소해 dispatch 정지를 막는다. */
		if (remaining_sectors > 0)
			zone_dispatch_cancel(c, chunk_phys, remaining_sectors);
		kvfree(ctx->sstable_buf);
		release_frozen_memtable(c, ctx->old_memtable, false);
		kfree(ctx);
		flush_chain_end(c);
		return;
	}
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = chunk_phys;
	bio->bi_opf = REQ_OP_WRITE;

	for (i = 0; i < nr_pages; i++) {
		struct page *page = buf_page(ctx->sstable_buf, off_bytes + (size_t)i * PAGE_SIZE);
		size_t len = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;

		bio_add_page(bio, page, len, 0);
		remaining -= len;
	}

	bio->bi_end_io = sstable_write_chunk_done;
	bio->bi_private = ctx;
	if (metrics_enabled)
		atomic64_add(chunk_bytes, &c->metric_flush_write_bytes);
	zone_dispatch_write(c, chunk_phys, chunk_sectors, bio);
}

/* 청크 완료 시 다음 청크를 제출한다. 마지막 청크나 오류는 sstable_flush_complete로 넘긴다. */
static void sstable_write_chunk_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	struct zns_base_c *c = ctx->c;
	blk_status_t status = bio->bi_status;
	sector_t remaining_sectors = ctx->sstable_nr_sectors - ctx->sstable_done_sectors;
	sector_t chunk_sectors = min_t(sector_t, remaining_sectors, SSTABLE_IO_MAX_SECTORS);

	bio_put(bio);
	ctx->sstable_done_sectors += chunk_sectors;

	if (status) {
		/* 실패한 이번 청크 이후로 아직 안 나간 배정분이 있으면 취소 — 안 그러면 그 SSTable zone의 dispatch가 영구히 멈춘다. */
		sector_t left = ctx->sstable_nr_sectors - ctx->sstable_done_sectors;

		if (left > 0)
			zone_dispatch_cancel(c, ctx->sstable_phys + ctx->sstable_done_sectors, left);
		sstable_flush_complete(ctx, status);
		return;
	}
	if (ctx->sstable_done_sectors < ctx->sstable_nr_sectors) {
		submit_sstable_write_async(ctx);
		return;
	}
	sstable_flush_complete(ctx, 0); /* 전 청크 durable */
}

/* 호출자가 c->lock을 보유하고 compaction 큐를 drain한 상태여야 한다.
 * live table이 모두 이전 active zone에 있고 이번 flush가 k번째 table일 때만 reserve를 차용한다.
 * 새 zone에 이번 flush와 중복 제거 없는 최악 병합 결과가 함께 들어가야 다음 compaction으로 이전 zone을 회수할 수 있다. */
static bool sstable_flush_can_borrow_reserve(struct zns_base_c *c,
					     u64 flush_records,
					     sector_t flush_sectors,
					     unsigned int old_zone)
{
	u64 merged_records = flush_records;
	sector_t merged_sectors;
	unsigned int free_zones = gc_count_free_zones(c->zp);
	unsigned int i;

	if (old_zone == ZONE_NONE || compaction_k < 2 ||
	    c->nr_sstables != compaction_k - 1 ||
	    c->zp->sstable_live_count[old_zone] != c->nr_sstables ||
	    !free_zones || free_zones > gc_reserved_zones ||
	    c->gc_active || c->wal_rotation_pending ||
	    c->wal_ckpt_inflight != 1 ||
	    c->zp->dispatch_wp[old_zone] !=
		(sector_t)old_zone * c->zp->zone_sectors + c->zp->wp[old_zone])
		return false;

	for (i = 0; i < c->nr_sstables; i++) {
		if (zone_of(c->zp, c->sstables[i].phys) != old_zone)
			return false;
		merged_records += c->sstables[i].record_count;
	}

	merged_sectors = (512 + round_up(merged_records *
					     sizeof(struct sstable_record), 512)) / 512;
	return 1 + flush_sectors + merged_sectors <= c->zp->zone_sectors;
}

/* memtable 하나를 SSTable 한 세대로 직렬화해서 zone에 기록. 전용 flush worker의
 * process context에서 호출되므로 큰 버퍼는 kvzalloc(GFP_KERNEL)로 잡을 수 있다.
 * old_memtable은 이미 c->memtable에서 떼어져 나온 상태라 락 없이 순회해도 안전.
 * split_gen/off는 ctx에 실어 뒷단 체크포인트 레코드에 쓴다. */
static void flush_memtable_async(struct zns_base_c *c, struct skiplist *old_memtable,
				 u64 seq_no, u64 split_gen, sector_t split_off)
{
	struct sstable_header *hdr;
	struct sstable_record *rec;
	struct skiplist_node *node;
	void *buf;
	size_t data_bytes;
	size_t alloc_bytes;
	sector_t nr_sectors;
	sector_t phys;
	int new_sstable_zone;
	unsigned int old_sstable_zone = ZONE_NONE;
	bool borrowed_reserve = false;
	struct zns_io_ctx *ctx;
	u64 i = 0;
	int ret;

	if (old_memtable->count == 0) {
		release_frozen_memtable(c, old_memtable, false);
		flush_chain_end(c);
		return;
	}
	if (metrics_enabled) {
		atomic64_set(&c->metric_flush_started_ns, ktime_get_ns());
		atomic64_inc(&c->metric_flush_runs);
	}

	/* 512(헤더 1섹터) + 레코드들(16B*count)을 섹터 단위로 올림 */
	data_bytes = 512 + round_up(old_memtable->count * sizeof(struct sstable_record), 512);
	nr_sectors = data_bytes / 512;
	alloc_bytes = round_up(data_bytes, PAGE_SIZE);

	/* kvzalloc은 큰 버퍼가 물리 연속 할당에 실패하면 vmalloc로 폴백한다.
	 * 아래 비동기 SSTable writer는 buf_page()로 두 종류 버퍼를 모두 지원한다. */
	buf = kvzalloc(alloc_bytes, GFP_KERNEL);
	if (!buf) {
		DMERR("SSTable flush: out of memory (seq=%llu); retaining frozen map and failing I/O closed",
		      (unsigned long long)seq_no);
		release_frozen_memtable(c, old_memtable, false);
		flush_chain_end(c);
		return;
	}

	hdr = buf;
	hdr->magic = cpu_to_le32(SSTABLE_MAGIC);
	hdr->seq_no = cpu_to_le64(seq_no);
	hdr->record_count = cpu_to_le64(old_memtable->count);

	rec = (struct sstable_record *)((char *)buf + 512);
	node = old_memtable->head->forward[0];
	hdr->min_lba = cpu_to_le64(node ? node->lba : 0);
	hdr->max_lba = 0;
	while (node) {
		rec[i].lba = cpu_to_le64(node->lba);
		rec[i].phys = cpu_to_le64(node->phys);
		hdr->max_lba = cpu_to_le64(node->lba);
		node = node->forward[0];
		i++;
	}

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors,
			      &phys, &new_sstable_zone, false);
	spin_unlock_irq(&c->lock);
	if (ret == -ENOSPC) {
		/* 대기 중인 compaction이 공간을 회수할 수 있으므로 drain한 뒤 reserve 차용 가능성을 판단한다. */
		flush_workqueue(zns_compaction_wq);
		spin_lock_irq(&c->lock);
		ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors,
				      &phys, &new_sstable_zone, false);
		if (ret == -ENOSPC) {
			old_sstable_zone = c->zp->active_zone[ZONE_TAG_SSTABLE];
			if (sstable_flush_can_borrow_reserve(c, old_memtable->count,
					   nr_sectors, old_sstable_zone)) {
				ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors,
					      &phys, &new_sstable_zone, true);
				borrowed_reserve = !ret;
			}
		}
		spin_unlock_irq(&c->lock);
	}
	if (ret) {
		DMERR("SSTable flush: zone_pool_alloc failed (%d, seq=%llu); retaining frozen map and failing I/O closed",
		      ret, (unsigned long long)seq_no);
		kvfree(buf);
		release_frozen_memtable(c, old_memtable, false);
		flush_chain_end(c);
		return;
	}
	if (borrowed_reserve)
		DMINFO("SSTable flush: borrowed reserve zone %d to rotate full SSTable zone %u",
		       new_sstable_zone, old_sstable_zone);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		DMERR("SSTable flush: out of memory building ctx (seq=%llu); retaining frozen map and failing I/O closed",
		      (unsigned long long)seq_no);
		kvfree(buf);
		release_frozen_memtable(c, old_memtable, false);
		flush_chain_end(c);
		return;
	}
	ctx->c = c;
	ctx->old_memtable = old_memtable;
	ctx->sstable_buf = buf;
	ctx->sstable_phys = phys;
	ctx->sstable_nr_sectors = nr_sectors;
	ctx->checkpoint_seq = seq_no;
	ctx->checkpoint_split_gen = split_gen;
	ctx->checkpoint_split_off = split_off;

	ctx->nr_headers = 0;
	if (new_sstable_zone >= 0) {
		ctx->headers[ctx->nr_headers].zone_id = new_sstable_zone;
		ctx->headers[ctx->nr_headers].tag = ZONE_TAG_SSTABLE;
		ctx->nr_headers++;
	}
	ctx->header_idx = 0;
	ctx->on_headers_done = submit_sstable_write_async;

	if (ctx->nr_headers > 0)
		submit_header_async(ctx);
	else
		submit_sstable_write_async(ctx);
}

/* 메모리 매핑 miss 이후의 비동기 SSTable 탐색 상태다. 후보별 이진 탐색으로 가장 최신 seq_no를 찾는다.
 * 512B probe 버퍼를 재사용하며, 탐색 중 색인이 바뀌면 read_view_epoch로 감지해 다시 조회한다. */
struct sstable_read_ctx {
	struct zns_base_c *c;
	struct bio *orig_bio;
	u64 lba;
	sector_t offset_in_block;
	struct sstable_info *candidates;
	unsigned int nr_candidates;
	unsigned int idx;
	void *sec_buf; /* 512B — probe가 읽는 섹터(모든 probe/후보가 재사용) */
	long bs_lo, bs_hi; /* 현재 후보의 이진 탐색 범위(레코드 인덱스) */
	long bs_mid; /* 이번 probe가 읽은 mid — 콜백이 레코드 위치 계산에 씀 */
	int best_found;
	u64 best_seq;
	sector_t best_phys;
	u64 view_epoch;
	blk_status_t error;
	sector_t cached_sector;
	bool cached_valid;
};

static void sstable_read_probe(struct sstable_read_ctx *rctx);
static void sstable_probe_done(struct bio *bio);
static void sstable_read_next_candidate(struct sstable_read_ctx *rctx);
static void sstable_probe_consume(struct sstable_read_ctx *rctx);

/* 후보를 다 훑었으면 결과를 원본 bio에 반영하고 체인을 끝낸다 */
static void sstable_read_finish(struct sstable_read_ctx *rctx)
{
	struct bio *orig = rctx->orig_bio;
	struct zns_base_c *c = rctx->c;
	unsigned int i;
	struct zns_read_pin *pin = dm_per_bio_data(orig, sizeof(*pin));
	u64 cur;
	bool found;

	spin_lock_irq(&c->lock);
	if (c->metadata_failed || rctx->error) {
		spin_unlock_irq(&c->lock);
		orig->bi_status = rctx->error ? rctx->error : BLK_STS_IOERR;
		bio_endio(orig);
		goto out_unpin;
	}
	found = mapping_get(c, rctx->lba, &cur);
	if (!found && rctx->view_epoch != c->read_view_epoch) {
		struct sstable_info *fresh;
		unsigned int count = c->nr_sstables;

		/* 탐색 중 flush가 frozen map을 공개·해제했으므로 최신 SSTable 목록으로 다시 시작한다. */
		fresh = kmalloc_array(count ? count : 1, sizeof(*fresh), GFP_ATOMIC);
		if (!fresh) {
			spin_unlock_irq(&c->lock);
			orig->bi_status = BLK_STS_RESOURCE;
			bio_endio(orig);
			goto out_unpin;
		}
		memcpy(fresh, c->sstables, count * sizeof(*fresh));
		for (i = 0; i < count; i++)
			zone_read_get(c->zp, zone_of(c->zp, fresh[i].phys));
		rctx->view_epoch = c->read_view_epoch;
		spin_unlock_irq(&c->lock);
		for (i = 0; i < rctx->nr_candidates; i++)
			zone_read_put(c->zp, zone_of(c->zp, rctx->candidates[i].phys));
		kfree(rctx->candidates);
		rctx->candidates = fresh;
		rctx->nr_candidates = count;
		rctx->idx = 0;
		rctx->best_found = 0;
		rctx->best_seq = 0;
		rctx->cached_valid = false;
		sstable_read_next_candidate(rctx);
		return;
	}
	if (!found) {
		found = rctx->best_found;
		cur = rctx->best_phys;
	}
	if (found && cur != MAPPING_TOMBSTONE) {
		pin->zone = zone_of(c->zp, cur);
		zone_read_get(c->zp, pin->zone);
	}
	spin_unlock_irq(&c->lock);
	if (found && cur != MAPPING_TOMBSTONE) {
		orig->bi_iter.bi_sector = cur + rctx->offset_in_block;
		bio_set_dev(orig, c->dev->bdev);
		submit_bio_deferred(orig);
	} else {
		/* 최신 매핑이 없거나 tombstone이면 zero-fill한다. */
		zero_fill_bio(orig);
		bio_endio(orig);
	}
out_unpin:
	/* probe 동안 잡아둔 SSTable zone pin 전부 해제 */
	for (i = 0; i < rctx->nr_candidates; i++)
		zone_read_put(c->zp, zone_of(c->zp, rctx->candidates[i].phys));
	kfree(rctx->sec_buf);
	kfree(rctx->candidates);
	kfree(rctx);
}

/* 다음 in-range 후보로 넘어가며 그 후보의 이진 탐색을 초기화한다. */
static void sstable_read_next_candidate(struct sstable_read_ctx *rctx)
{
	struct sstable_info *si;

	while (rctx->idx < rctx->nr_candidates) {
		si = &rctx->candidates[rctx->idx];
		if (rctx->lba >= si->min_lba && rctx->lba <= si->max_lba)
			break;
		rctx->idx++; /* 범위 밖 — 이 후보는 애초에 볼 필요 없음 */
	}
	if (rctx->idx >= rctx->nr_candidates) {
		sstable_read_finish(rctx);
		return;
	}
	si = &rctx->candidates[rctx->idx];
	rctx->bs_lo = 0;
	rctx->bs_hi = (long)si->record_count - 1;
	sstable_read_probe(rctx);
}

/* 이진 탐색의 중간 레코드가 든 섹터를 비동기로 읽는다. 완료 콜백에서도 호출되므로 GFP_ATOMIC을 사용한다. */
static void sstable_read_probe(struct sstable_read_ctx *rctx)
{
	struct sstable_info *si = &rctx->candidates[rctx->idx];
	const unsigned int per_sec = 512 / sizeof(struct sstable_record);
	sector_t sec_no;
	struct bio *bio;

	if (rctx->bs_lo > rctx->bs_hi) {  /* 이 후보엔 없음 → 다음 후보 */
		rctx->idx++;
		sstable_read_next_candidate(rctx);
		return;
	}
	rctx->bs_mid = rctx->bs_lo + (rctx->bs_hi - rctx->bs_lo) / 2;
	sec_no = si->phys + 1 + rctx->bs_mid / per_sec;
	if (rctx->cached_valid && rctx->cached_sector == sec_no) {
		atomic64_inc(&rctx->c->sstable_probe_reuses);
		sstable_probe_consume(rctx);
		return;
	}

	bio = bio_alloc(GFP_ATOMIC, 1);
	if (!bio) {
		rctx->error = BLK_STS_RESOURCE;
		sstable_read_finish(rctx);
		return;
	}
	bio_set_dev(bio, rctx->c->dev->bdev);
	bio->bi_iter.bi_sector = sec_no;
	rctx->cached_sector = sec_no;
	rctx->cached_valid = false;
	bio->bi_opf = REQ_OP_READ;
	bio_add_page(bio, virt_to_page(rctx->sec_buf), 512, offset_in_page(rctx->sec_buf));
	bio->bi_end_io = sstable_probe_done;
	bio->bi_private = rctx;
	atomic64_inc(&rctx->c->sstable_probe_ios);
	submit_bio_deferred(bio);
}

/* probe 결과로 탐색 범위를 좁힌다. 일치하면 best 매핑을 갱신하고 다음 후보로 이동한다. */
static void sstable_probe_done(struct bio *bio)
{
	struct sstable_read_ctx *rctx = bio->bi_private;
	blk_status_t status = bio->bi_status;

	bio_put(bio);

	if (status) {
		rctx->error = status;
		sstable_read_finish(rctx);
		return;
	}
	rctx->cached_valid = true;
	sstable_probe_consume(rctx);
}

static void sstable_probe_consume(struct sstable_read_ctx *rctx)
{
	struct sstable_info *si = &rctx->candidates[rctx->idx];
	const unsigned int per_sec = 512 / sizeof(struct sstable_record);
	struct sstable_record *r;
	r = (struct sstable_record *)((char *)rctx->sec_buf +
	    (rctx->bs_mid % per_sec) * sizeof(struct sstable_record));
	if (le64_to_cpu(r->lba) == rctx->lba) {
		if (!rctx->best_found || si->seq_no > rctx->best_seq) {
			rctx->best_found = 1;
			rctx->best_seq = si->seq_no;
			rctx->best_phys = le64_to_cpu(r->phys);
		}
		rctx->idx++;
		sstable_read_next_candidate(rctx);
	} else if (le64_to_cpu(r->lba) < rctx->lba) {
		rctx->bs_lo = rctx->bs_mid + 1;
		sstable_read_probe(rctx);
	} else {
		rctx->bs_hi = rctx->bs_mid - 1;
		sstable_read_probe(rctx);
	}
}

/* LBA 정렬 레코드와 k-way merge의 진행 위치다. 같은 LBA는 seq_no로 우선순위를 판단한다. */
struct compaction_source {
	struct sstable_record *recs;
	u64 count;
	u64 idx; /* 다음으로 볼 레코드 인덱스 */
	u64 seq_no; /* 이 소스의 세대 — 동률(같은 lba) 시 우선순위 판단 */
};

/* sort()용 비교자 — seq_no 오름차순(가장 오래된 것부터) */
static int sstable_info_cmp_seq_asc(const void *a, const void *b)
{
	const struct sstable_info *sa = a, *sb = b;

	if (sa->seq_no < sb->seq_no)
		return -1;
	if (sa->seq_no > sb->seq_no)
		return 1;
	return 0;
}

/* victim 하나의 레코드 전체를 동기적으로 읽어 src에 채운다.
 * compaction_work_fn 전용 process context */
static int compaction_read_source(struct zns_base_c *c, struct sstable_info *victim,
				  struct compaction_source *src)
{
	size_t total_bytes = victim->record_count * sizeof(struct sstable_record);
	size_t alloc_bytes = round_up(total_bytes, PAGE_SIZE);
	sector_t nr_sectors = DIV_ROUND_UP(total_bytes, 512);
	int ret;

	/* 병합 SSTable은 수 MB까지 커질 수 있어 kvmalloc(vmalloc 폴백) 사용 */
	src->recs = kvzalloc(alloc_bytes, GFP_KERNEL);
	if (!src->recs)
		return -ENOMEM;

	ret = sstable_io_sync(c, REQ_OP_READ, victim->phys + 1, src->recs, nr_sectors);
	if (ret) {
		kvfree(src->recs);
		src->recs = NULL;
		return ret;
	}

	src->count = victim->record_count;
	src->idx = 0;
	src->seq_no = victim->seq_no;
	return 0;
}

/* 락 밖에서 merge를 마친 뒤 폐기된 물리 위치의 무효 추정치를 모아 반영한다. */
static void apply_discarded_invalid_counts(struct zns_base_c *c, sector_t *discarded, unsigned int count)
{
	unsigned int i;

	if (count == 0)
		return;
	spin_lock_irq(&c->lock);
	for (i = 0; i < count; i++)
		if (discarded[i] != MAPPING_TOMBSTONE)
			c->zp->invalid_count[zone_of(c->zp, discarded[i])]++;
	spin_unlock_irq(&c->lock);
}

/* 정렬된 소스를 병합하고 같은 LBA에서는 최신 seq_no만 남긴다.
 * 폐기한 물리 위치는 discarded_out에 모은다. 호출자가 출력과 폐기 목록의 최대 크기를 미리 확보해야 한다. */
static u64 merge_sstable_sources(struct compaction_source *srcs, unsigned int nr_srcs,
				 struct sstable_record *out,
				 sector_t *discarded_out, unsigned int *discarded_count)
{
	u64 out_count = 0;
	unsigned int ndisc = 0;

	for (;;) {
		int min_i = -1;
		u64 min_lba = 0;
		unsigned int s;
		int best_src = -1;
		u64 best_seq = 0;
		u64 best_phys = 0;

		/* 아직 안 끝난 소스들 중 가장 작은 lba를 찾는다 */
		for (s = 0; s < nr_srcs; s++) {
			if (srcs[s].idx >= srcs[s].count)
				continue;
			if (min_i < 0 || le64_to_cpu(srcs[s].recs[srcs[s].idx].lba) < min_lba) {
				min_i = (int)s;
				min_lba = le64_to_cpu(srcs[s].recs[srcs[s].idx].lba);
			}
		}
		if (min_i < 0)
			break; /* 모든 소스 소진 */

		/* 그 lba를 지금 가리키는 모든 소스를 확인 — seq_no 최댓값만 채택, 나머지는 폐기(그 데이터 phys를 invalid_count 대상으로 기록) */
		for (s = 0; s < nr_srcs; s++) {
			if (srcs[s].idx >= srcs[s].count)
				continue;
			if (le64_to_cpu(srcs[s].recs[srcs[s].idx].lba) != min_lba)
				continue;

			if (best_src < 0 || srcs[s].seq_no > best_seq) {
				if (best_src >= 0)
					discarded_out[ndisc++] = best_phys; /* 이전 최선이 밀려남 */
				best_src = (int)s;
				best_seq = srcs[s].seq_no;
				best_phys = le64_to_cpu(srcs[s].recs[srcs[s].idx].phys);
			} else {
				discarded_out[ndisc++] = le64_to_cpu(srcs[s].recs[srcs[s].idx].phys);
			}
			srcs[s].idx++;
		}

		out[out_count].lba = cpu_to_le64(min_lba);
		out[out_count].phys = cpu_to_le64(best_phys);
		out_count++;
	}

	*discarded_count = ndisc;
	return out_count;
}

/* 호출자가 c->lock을 보유해야 한다. 이번 compaction이 이전 zone의 모든 live 참조를 제거하는지 검사한다.
 * 미제출 예약이 없어야 하며, 새 출력의 durable commit으로 이전 zone을 반환할 수 있을 때 reserve 차용을 허용한다.
 * 할당 시 같은 락에서 active slot이 바뀌므로 이후 flush는 새 zone을 사용한다. */
static bool sstable_compaction_reserve_reclaimable(struct zns_base_c *c,
						   struct sstable_info *victims,
						   unsigned int nr_victims,
						   unsigned int old_zone)
{
	unsigned int free_zones = gc_count_free_zones(c->zp);
	unsigned int victim_refs = 0;
	unsigned int i;

	if (old_zone == ZONE_NONE || !free_zones ||
	    free_zones > gc_reserved_zones ||
	    c->zp->dispatch_wp[old_zone] !=
		(sector_t)old_zone * c->zp->zone_sectors + c->zp->wp[old_zone])
		return false;

	for (i = 0; i < nr_victims; i++)
		if (zone_of(c->zp, victims[i].phys) == old_zone)
			victim_refs++;

	return victim_refs &&
		victim_refs == c->zp->sstable_live_count[old_zone];
}

/* 호출자가 c->lock을 보유해야 한다. 구조적인 회수 보장과 GC·WAL·checkpoint의 일시적 점유를 구분한다.
 * GC에 양보를 요청한 경우 종료를 기다린 뒤 이 조건을 다시 검사한다. */
static bool sstable_compaction_can_borrow_reserve(struct zns_base_c *c,
						  struct sstable_info *victims,
						  unsigned int nr_victims,
						  unsigned int old_zone)
{
	return !c->gc_active && !c->wal_rotation_pending &&
		!c->wal_ckpt_inflight &&
		sstable_compaction_reserve_reclaimable(c, victims, nr_victims,
						       old_zone);
}

/* process context에서 오래된 k개 SSTable을 병합한다. 새 출력이 영속화된 뒤에만 이전 색인을 제거한다.
 * 참조와 미제출 예약이 없어진 zone은 읽기를 drain한 뒤 reset한다. */
static void compaction_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(work, struct zns_base_c, compaction_work);
	struct sstable_info *snapshot = NULL;
	unsigned int snap_count = 0;
	unsigned int snap_cap;

	struct compaction_source *srcs = NULL;
	struct sstable_record *merged = NULL;
	sector_t *discarded = NULL;
	unsigned int discarded_count = 0;
	u64 merged_count = 0;
	u64 total_input_records = 0;
	unsigned int k = compaction_k;
	unsigned int i;
	int ret;

	void *out_buf = NULL;
	struct sstable_header *out_hdr;
	struct sstable_record *out_rec;
	size_t data_bytes, alloc_bytes;
	sector_t nr_sectors = 0;
	sector_t new_phys = 0;
	int new_zone = -1;

	unsigned int old_sstable_zone = ZONE_NONE;
	bool borrowed_reserve = false;
	bool proactive_rotation = false;
	bool rotation_pending_owned = false;
	struct memtable_flush_work *deferred_flush = NULL;
	bool retry_gc = false;

	u64 out_seq_no = 0;
	u64 merged_min_lba = 0, merged_max_lba = 0;

	u64 metric_started = 0;

	spin_lock_irq(&c->lock);
	if (c->nr_sstables < k) {
		spin_unlock_irq(&c->lock);
		return; /* 그 사이 조건이 이미 해소됨(레이스) — 할 일 없음 */
	}
	snap_cap = c->nr_sstables;
	spin_unlock_irq(&c->lock);

	snapshot = kvmalloc_array(snap_cap, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot) {
		DMERR("compaction: out of memory snapshotting SSTable index, will retry on next trigger");
		return;
	}
	spin_lock_irq(&c->lock);
	snap_count = min(snap_cap, c->nr_sstables);
	if (snap_count < k) {
		spin_unlock_irq(&c->lock);
		kvfree(snapshot);
		return;
	}
	memcpy(snapshot, c->sstables, snap_count * sizeof(*snapshot));
	spin_unlock_irq(&c->lock);
	if (metrics_enabled) {
		metric_started = ktime_get_ns();
		atomic64_inc(&c->metric_compaction_runs);
	}

	/* 가장 오래된(seq_no 최소) k개를 victim으로 — 정렬 후 앞 k개 */
	sort(snapshot, snap_count, sizeof(*snapshot), sstable_info_cmp_seq_asc, NULL);

	srcs = kzalloc(k * sizeof(*srcs), GFP_KERNEL);
	if (!srcs) {
		DMERR("compaction: out of memory allocating sources, aborting this run");
		goto out_free_snapshot;
	}

	for (i = 0; i < k; i++) {
		ret = compaction_read_source(c, &snapshot[i], &srcs[i]);
		if (ret) {
			DMERR("compaction: failed to read victim SSTable (seq=%llu, err=%d), aborting this run — old SSTables remain valid",
			      (unsigned long long)snapshot[i].seq_no, ret);
			goto out_free_sources;
		}
		total_input_records += snapshot[i].record_count;
	}

	/* 큰 병합 버퍼는 물리 연속 할당에 의존하지 않도록 kvmalloc을 사용한다. */
	merged = kvzalloc(round_up(total_input_records * sizeof(struct sstable_record), 512), GFP_KERNEL);
	discarded = kvmalloc_array(total_input_records ? total_input_records : 1, sizeof(*discarded), GFP_KERNEL);
	if (!merged || !discarded) {
		DMERR("compaction: out of memory allocating merge buffers, aborting this run");
		goto out_free_sources;
	}

	merged_count = merge_sstable_sources(srcs, k, merged, discarded, &discarded_count);
	if (metrics_enabled)
		atomic64_add(total_input_records * sizeof(struct sstable_record),
			&c->metric_compaction_input_bytes);
	apply_discarded_invalid_counts(c, discarded, discarded_count);

	if (merged_count == 0) {
		/* 이론상 안 생김(입력 SSTable들은 항상 count>0으로만 만들어짐) — 방어적 처리 */
		DMERR("compaction: merge produced zero records, aborting this run");
		goto out_free_sources;
	}

	merged_min_lba = le64_to_cpu(merged[0].lba);
	merged_max_lba = le64_to_cpu(merged[merged_count - 1].lba);

	data_bytes = 512 + round_up(merged_count * sizeof(struct sstable_record), 512);
	nr_sectors = data_bytes / 512;
	alloc_bytes = round_up(data_bytes, PAGE_SIZE);

	/* 병합 결과가 수 MB까지 커질 수 있어 kvmalloc(vmalloc 폴백) */
	out_buf = kvzalloc(alloc_bytes, GFP_KERNEL);
	if (!out_buf) {
		DMERR("compaction: out of memory serializing merged SSTable, aborting this run");
		goto out_free_sources;
	}
	if (metrics_enabled) {
		u64 scratch = (u64)snap_cap * sizeof(*snapshot) + (u64)k * sizeof(*srcs) +
			round_up(total_input_records * sizeof(*merged), 512) +
			max_t(u64, total_input_records, 1) * sizeof(*discarded) + alloc_bytes;
		for (i = 0; i < k; i++)
			scratch += round_up(snapshot[i].record_count * sizeof(struct sstable_record), 512);
		atomic64_set(&c->metric_compaction_scratch_bytes, scratch);
	}

	/* 병합 출력은 새 논리 쓰기가 아니므로 victim 중 최대 seq_no를 유지한다.
	 * 새 번호를 부여하면 병합 밖의 더 최신 SSTable보다 오래된 매핑이 우선할 수 있다. */
	out_seq_no = snapshot[k - 1].seq_no;

	out_hdr = out_buf;
	out_hdr->magic = cpu_to_le32(SSTABLE_MAGIC);
	out_hdr->seq_no = cpu_to_le64(out_seq_no);
	out_hdr->record_count = cpu_to_le64(merged_count);
	out_hdr->min_lba = cpu_to_le64(merged_min_lba);
	out_hdr->max_lba = cpu_to_le64(merged_max_lba);

	out_rec = (struct sstable_record *)((char *)out_buf + 512);
	memcpy(out_rec, merged, merged_count * sizeof(struct sstable_record));

	spin_lock_irq(&c->lock);
	old_sstable_zone = c->zp->active_zone[ZONE_TAG_SSTABLE];
	/* 이전 zone의 모든 live 참조를 포함하는 compaction은 reserve 경계에서 선제 회전한다.
	 * 출력을 같은 tail에 계속 쌓아 다음 flush가 공간을 잃는 일을 막고, commit 뒤 이전 zone으로 차용분을 상환한다. */
	if (sstable_compaction_can_borrow_reserve(c, snapshot, k,
						 old_sstable_zone)) {
		ret = zone_pool_alloc_fresh(c->zp, ZONE_TAG_SSTABLE, nr_sectors,
					    &new_phys, &new_zone, true);
		borrowed_reserve = !ret;
		proactive_rotation = !ret;
	} else if (c->gc_active && !c->wal_rotation_pending &&
		   !c->wal_ckpt_inflight && !c->sstable_rotation_pending &&
		   sstable_compaction_reserve_reclaimable(c, snapshot, k,
							 old_sstable_zone)) {
		/* memtable swap을 보류하고 GC가 victim 경계에서 양보할 때까지 기다린다.
		 * 이미 만든 merge 버퍼를 보존해 회전 할당만 재시도한다. */
		c->sstable_rotation_pending = true;
		rotation_pending_owned = true;
		ret = -EAGAIN;
	} else {
		ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors,
				      &new_phys, &new_zone, false);
		if (ret == -ENOSPC &&
		    sstable_compaction_can_borrow_reserve(c, snapshot, k,
							 old_sstable_zone)) {
			ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors,
					      &new_phys, &new_zone, true);
			borrowed_reserve = !ret;
		}
	}
	spin_unlock_irq(&c->lock);

	if (rotation_pending_owned) {
		DMINFO("compaction: waiting for active GC to yield before reserve-boundary SSTable rotation");
		wait_event(c->flush_waitq,
			   !READ_ONCE(c->gc_active) || READ_ONCE(c->stopping) ||
			   READ_ONCE(c->metadata_failed));
		spin_lock_irq(&c->lock);
		if (c->stopping || c->metadata_failed) {
			ret = -EIO;
		} else if (sstable_compaction_can_borrow_reserve(c, snapshot, k,
							 old_sstable_zone)) {
			ret = zone_pool_alloc_fresh(c->zp, ZONE_TAG_SSTABLE,
						    nr_sectors, &new_phys,
						    &new_zone, true);
			borrowed_reserve = !ret;
			proactive_rotation = !ret;
		} else {
			ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors,
					      &new_phys, &new_zone, false);
		}
		spin_unlock_irq(&c->lock);
	}
	if (ret) {
		DMERR("compaction: zone_pool_alloc failed (%d), aborting this run — old SSTables remain valid", ret);
		goto out_free_out_buf;
	}
	if (borrowed_reserve)
		DMINFO("compaction: borrowed reserve zone %d to rotate full SSTable zone %u (proactive=%u)",
		       new_zone, old_sstable_zone, proactive_rotation ? 1 : 0);

	if (new_zone >= 0) {
		struct zone_header *zhdr = kzalloc(512, GFP_KERNEL);

		if (!zhdr) {
			DMERR("compaction: out of memory writing zone header (zone %d), aborting", new_zone);
			zone_dispatch_cancel(c, (sector_t)new_zone * c->zp->zone_sectors, 1);
			ret = -ENOMEM;
		} else {
			struct bio *hbio = bio_alloc(GFP_KERNEL, 1);

			if (!hbio) {
				kfree(zhdr);
				zone_dispatch_cancel(c, (sector_t)new_zone * c->zp->zone_sectors, 1);
				ret = -ENOMEM;
				goto compaction_header_failed;
			}

			zhdr->magic = cpu_to_le32(ZONE_HEADER_MAGIC);
			zhdr->tag = cpu_to_le32(ZONE_TAG_SSTABLE);
			bio_set_dev(hbio, c->dev->bdev);
			hbio->bi_iter.bi_sector = (sector_t)new_zone * c->zp->zone_sectors;
			hbio->bi_opf = REQ_OP_WRITE;
			bio_add_page(hbio, virt_to_page(zhdr), 512, offset_in_page(zhdr));
			/* flush 경로(async)와 같은 SSTABLE 태그 zone을 두고 경쟁할 수
			 * 있으므로 순서 게이트를 반드시 거친다 */
			zone_dispatch_wait_turn(c, hbio->bi_iter.bi_sector, 1);
			if (metrics_enabled)
				atomic64_add(512, &c->metric_write_bytes[ZONE_TAG_SSTABLE]);
			ret = submit_bio_wait(hbio);
			bio_put(hbio);
			kfree(zhdr);
			if (ret)
				DMERR("compaction: zone header write failed (zone %d), aborting", new_zone);
		}
		if (ret) {
compaction_header_failed:
			zone_quarantine(c, new_zone, ZONE_TAG_SSTABLE, BLK_STS_IOERR);
			zone_dispatch_cancel(c, new_phys, nr_sectors);
			goto out_free_out_buf;
		}
		zone_append_header_done(c, new_zone, 0);
	}

	/* 전체 출력 범위의 제출 차례를 확보한 뒤 sstable_io_sync로 나누어 영속화한다. */
	zone_dispatch_wait_turn(c, new_phys, nr_sectors);
	ret = sstable_io_sync(c, REQ_OP_WRITE, new_phys, out_buf, nr_sectors);
	if (!ret)
		ret = blkdev_issue_flush(c->dev->bdev);
	if (ret) {
		DMERR("compaction: merged SSTable write failed (err=%d), aborting this run — old SSTables remain valid", ret);
		goto out_free_out_buf;
	}

	/* 여기서부터 커밋 — 새 SSTable은 이제 완전히 durable함 */
	if (metrics_enabled)
		atomic64_add((u64)nr_sectors * 512, &c->metric_compaction_output_bytes);
	{
		unsigned int *zones_to_check = kmalloc_array(k, sizeof(unsigned int), GFP_KERNEL);
		unsigned int nr_zones_to_check = 0;

		if (!zones_to_check)
			DMERR("compaction: out of memory tracking zones to reclaim, skipping zone reset this run");

		spin_lock_irq(&c->lock);
		ret = sstable_register(c, new_phys, out_seq_no, merged_count,
					 merged_min_lba, merged_max_lba, GFP_ATOMIC);
		if (ret) {
			spin_unlock_irq(&c->lock);
			DMERR("compaction: failed to register merged SSTable in memory (seq=%llu) — durable on disk, will be picked up by next restart's recovery scan",
			      (unsigned long long)out_seq_no);
			kfree(zones_to_check);
			goto out_free_out_buf;
		}

		for (i = 0; i < k; i++) {
			unsigned int zid = zone_of(c->zp, snapshot[i].phys);

			sstable_remove_by_phys(c, snapshot[i].phys);
			/* 미제출 예약이 남으면 회수하지 않는다. 예약된 쓰기를 reset으로 잃거나 dispatch 대기열을 정체시키면 안 된다. */
			if (zones_to_check && c->zp->sstable_live_count[zid] == 0
			    && zid != c->zp->active_zone[ZONE_TAG_SSTABLE]
			    && c->zp->dispatch_wp[zid] == (sector_t)zid * c->zp->zone_sectors + c->zp->wp[zid])
				zones_to_check[nr_zones_to_check++] = zid;
		}
		spin_unlock_irq(&c->lock);

		DMINFO("compaction: merged %u SSTables (seq %llu..%llu) into seq=%llu, %llu records (%u discarded duplicates)",
		       k, (unsigned long long)snapshot[0].seq_no, (unsigned long long)snapshot[k - 1].seq_no,
		       (unsigned long long)out_seq_no, (unsigned long long)merged_count, discarded_count);

		for (i = 0; i < nr_zones_to_check; i++) {
			unsigned int zid = zones_to_check[i];

			/* reset 직전, 이 SSTable zone을 읽는 중인 읽기(read pin)가 전부 끝나길 대기 */
			zone_wait_reads_drained(c->zp, zid);
			if (zone_reset_hw(c, zid)) {
				DMERR("compaction: hardware zone reset failed for zone %u — zone leaked until manually recovered", zid);
				continue;
			}
			spin_lock_irq(&c->lock);
			zone_pool_mark_free(c->zp, zid);
			spin_unlock_irq(&c->lock);
		}
		kfree(zones_to_check);
	}

out_free_out_buf:
	kvfree(out_buf);
out_free_sources:
	for (i = 0; i < k; i++)
		kvfree(srcs[i].recs);
	kfree(srcs);
	kvfree(merged);
	kvfree(discarded);
out_free_snapshot:
	kvfree(snapshot);
	if (metric_started)
		atomic64_add(ktime_get_ns() - metric_started, &c->metric_compaction_ns);
	atomic64_set(&c->metric_compaction_scratch_bytes, 0);
	if (rotation_pending_owned) {
		spin_lock_irq(&c->lock);
		c->sstable_rotation_pending = false;
		deferred_flush = schedule_memtable_flush_locked(c, false);
		retry_gc = !c->stopping && !c->metadata_failed &&
			gc_count_free_zones(c->zp) <= gc_low_watermark;
		spin_unlock_irq(&c->lock);
		wake_up_all(&c->flush_waitq);
		if (deferred_flush)
			queue_work(zns_flush_wq, &deferred_flush->work);
		if (retry_gc)
			queue_work(zns_gc_wq, &c->gc_work);
	}
}

/* GC: 닫힌 데이터 zone의 live 매핑을 이주하고 공간을 회수한다. */

/* memtable·SSTable에서 모은 LBA와 물리 주소의 스냅샷 항목이다. */
struct gc_candidate {
	u64 lba;
	sector_t phys;
};

/* GC cycle이 공유하는 latest-map 항목이다. SSTable과 memtable을 합쳐 LBA별 최신 위치를 유지한다. */
struct gc_live_entry {
	u64 lba;
	sector_t phys;
	bool used;
};

struct gc_live_map {
	struct gc_live_entry *entries;
	unsigned int capacity; /* 항상 2의 거듭제곱 */
	unsigned int count;
};

struct gc_cycle {
	struct gc_live_map latest;
	struct sstable_info *sstables;
	unsigned int nr_sstables;
	bool built;
	bool *excluded;
};

static int gc_diag_phys_cmp(const void *a, const void *b)
{
	const struct gc_live_entry *x = a, *y = b;

	if (x->phys != y->phys)
		return x->phys < y->phys ? -1 : 1;
	return x->lba < y->lba ? -1 : x->lba != y->lba;
}

/* overflow 때만 GC가 사용한 view를 제한된 크기로 진단한다. 할당과 출력은 c->lock 밖에서 수행한다.
 * 캐시된 view는 오래됐을 수 있으므로 중복 주소만으로 온디스크 손상을 확정하지 않는다. */
static void gc_diag_audit(struct zns_base_c *c, struct gc_live_map *map,
			  unsigned int victim, u64 used, u64 live, bool cached)
{
	struct gc_live_entry *rows;
	sector_t start = (sector_t)victim * c->zp->zone_sectors;
	sector_t end = start + c->zp->zone_sectors;
	unsigned int i, n = 0, aliases = 0, beyond = 0, shown = 0;
	unsigned int event;

	if (!gc_diag_budget || live <= used ||
	    atomic_read(&gc_diag_events) >= gc_diag_budget)
		return;
	event = atomic_inc_return(&gc_diag_events);
	if (event > gc_diag_budget)
		return;
	rows = kvmalloc_array(live / BLOCK_SECTORS, sizeof(*rows), GFP_KERNEL);
	if (!rows) {
		DMINFO("gc audit: event=%u allocation failed", event);
		return;
	}
	for (i = 0; i < map->capacity; i++) {
		struct gc_live_entry *e = &map->entries[i];

		if (e->used && e->phys >= start && e->phys < end)
			rows[n++] = *e;
	}
	sort(rows, n, sizeof(*rows), gc_diag_phys_cmp, NULL);
	for (i = 0; i < n; i++) {
		bool alias = i && rows[i].phys < rows[i - 1].phys + BLOCK_SECTORS;
		bool outside = rows[i].phys < start + 1 ||
			rows[i].phys + BLOCK_SECTORS > start + used + 1;

		aliases += alias;
		beyond += outside;
		if ((alias || outside) && shown++ < 16)
			DMINFO("gc audit: event=%u zone=%u lba=%llu phys=%llu prev_lba=%llu prev_phys=%llu overlap=%u outside_wp=%u",
			       event, victim, rows[i].lba, (u64)rows[i].phys,
			       i ? rows[i - 1].lba : 0,
			       i ? (u64)rows[i - 1].phys : 0, alias, outside);
	}
	DMINFO("gc audit: event=%u zone=%u cached=%u used=%llu live=%llu entries=%u overlaps=%u outside_wp=%u",
	       event, victim, cached, used, live, n, aliases, beyond);
	kvfree(rows);
}

static unsigned int gc_live_hash(u64 lba, unsigned int capacity)
{
	/* Fibonacci hashing. LBA가 4KB 정렬이어도 하위 비트가 고르게 섞인다. */
	return (unsigned int)((lba * 11400714819323198485ULL) & (capacity - 1));
}

static struct gc_live_entry *gc_live_map_find(struct gc_live_map *map, u64 lba)
{
	unsigned int idx, probes;

	if (!map->capacity)
		return NULL;
	idx = gc_live_hash(lba, map->capacity);
	for (probes = 0; probes < map->capacity; probes++) {
		struct gc_live_entry *entry = &map->entries[idx];

		if (!entry->used)
			return NULL;
		if (entry->lba == lba)
			return entry;
		idx = (idx + 1) & (map->capacity - 1);
	}
	return NULL;
}

static int gc_live_map_resize(struct gc_live_map *map, unsigned int new_capacity)
{
	struct gc_live_entry *old = map->entries;
	unsigned int old_capacity = map->capacity;
	unsigned int i;

	map->entries = kvcalloc(new_capacity, sizeof(*map->entries), GFP_KERNEL);
	if (!map->entries) {
		map->entries = old;
		return -ENOMEM;
	}
	map->capacity = new_capacity;
	map->count = 0;
	for (i = 0; i < old_capacity; i++) {
		unsigned int idx;

		if (!old[i].used)
			continue;
		idx = gc_live_hash(old[i].lba, map->capacity);
		while (map->entries[idx].used)
			idx = (idx + 1) & (map->capacity - 1);
		map->entries[idx] = old[i];
		map->count++;
	}
	kvfree(old);
	return 0;
}

/* create=false면 이미 victim 후보인 LBA만 갱신. create=true면
 * victim을 가리킨 첫 레코드이므로 없으면 새로 등록한다. */
static int gc_live_map_update(struct gc_live_map *map, u64 lba,
			      sector_t phys, bool create)
{
	struct gc_live_entry *entry = gc_live_map_find(map, lba);
	unsigned int idx;

	if (entry) {
		entry->phys = phys;
		return 0;
	}
	if (!create)
		return 0;
	if (!map->capacity || (map->count + 1) * 2 >= map->capacity) {
		unsigned int new_capacity = map->capacity ? map->capacity << 1 : 1024;

		if (new_capacity < map->capacity ||
		    gc_live_map_resize(map, new_capacity))
			return -ENOMEM;
	}
	idx = gc_live_hash(lba, map->capacity);
	while (map->entries[idx].used)
		idx = (idx + 1) & (map->capacity - 1);
	map->entries[idx].used = true;
	map->entries[idx].lba = lba;
	map->entries[idx].phys = phys;
	map->count++;
	return 0;
}

/* memtable을 락 아래에서 복사하고 해시 확장은 process context에서 수행한다.
 * 할당 중 엔트리가 늘어 버퍼를 넘으면 잘린 스냅샷을 사용하지 않고 재시도하도록 반환한다. */
static int gc_refresh_live_map(struct zns_base_c *c, struct gc_live_map *map)
{
	struct gc_candidate *rows;
	struct skiplist_node *node;
	unsigned int cap, n = 0, i;
	int ret = 0;

	spin_lock_irq(&c->lock);
	cap = c->memtable->count;
	spin_unlock_irq(&c->lock);
	/* 할당 중 foreground 증가를 위한 여유를 두되, 아래에서 실제 초과 여부도 검사한다. */
	if (cap < UINT_MAX - 65536)
		cap += 65536;
	rows = kvmalloc_array(cap ? cap : 1, sizeof(*rows), GFP_KERNEL);
	if (!rows)
		return -ENOMEM;
	spin_lock_irq(&c->lock);
	for (node = c->memtable->head->forward[0]; node && n < cap;
	     node = node->forward[0]) {
		rows[n].lba = node->lba;
		rows[n++].phys = node->phys;
	}
	if (node)
		ret = -EAGAIN;
	spin_unlock_irq(&c->lock);
	for (i = 0; !ret && i < n; i++)
		ret = gc_live_map_update(map, rows[i].lba, rows[i].phys, true);
	kvfree(rows);
	return ret;
}

/* [락] 호출자가 c->lock을 쥐고 있어야 한다. */
static unsigned int gc_count_free_zones(struct zone_pool *zp)
{
	unsigned int z, count = 0;

	for (z = 0; z < zp->nr_zones; z++)
		if (zp->zone_tag[z] == ZONE_TAG_FREE)
			count++;
	return count;
}

struct gc_space_state_row {
	enum zone_tag tag;
	sector_t wp;
	sector_t dispatch_wp;
	unsigned int invalid_count;
};

/* 무진전 구간마다 reserve 경계 상태를 한 번 수집한다.
 * 콘솔 출력이 foreground를 막지 않도록 printk는 c->lock 밖에서 수행한다. */
static void gc_log_no_progress_state(struct zns_base_c *c,
				     unsigned int no_progress)
{
	struct gc_space_state_row *rows;
	unsigned int active[ZONE_TAG_COUNT];
	sector_t active_tail[ZONE_TAG_COUNT] = { 0 };
	unsigned int free_zones;
	unsigned int nr_zones = c->zp->nr_zones;
	unsigned int tag, z;

	rows = kvmalloc_array(nr_zones, sizeof(*rows), GFP_KERNEL);
	if (!rows) {
		DMERR("gc: no-progress state snapshot allocation failed (zones=%u)",
		      nr_zones);
		return;
	}

	spin_lock_irq(&c->lock);
	free_zones = gc_count_free_zones(c->zp);
	for (tag = 0; tag < ZONE_TAG_COUNT; tag++)
		active[tag] = c->zp->active_zone[tag];
	for (z = 0; z < nr_zones; z++) {
		rows[z].tag = c->zp->zone_tag[z];
		rows[z].wp = c->zp->wp[z];
		rows[z].dispatch_wp = c->zp->dispatch_wp[z];
		rows[z].invalid_count = c->zp->invalid_count[z];
	}
	for (tag = 0; tag < ZONE_TAG_COUNT; tag++) {
		z = active[tag];
		if (z < nr_zones && rows[z].wp <= c->zp->zone_sectors)
			active_tail[tag] = c->zp->zone_sectors - rows[z].wp;
	}
	spin_unlock_irq(&c->lock);

	DMERR("gc: no-progress state (count=%u free=%u reserve=%u zones=%u zone_sectors=%llu)",
	      no_progress, free_zones, gc_reserved_zones, nr_zones,
	      (unsigned long long)c->zp->zone_sectors);
	for (tag = 0; tag < ZONE_TAG_COUNT; tag++)
		DMERR("gc: no-progress active tag=%u zone=%u tail=%llu",
		      tag, active[tag], (unsigned long long)active_tail[tag]);
	for (z = 0; z < nr_zones; z++) {
		bool is_active = false;

		for (tag = 0; tag < ZONE_TAG_COUNT; tag++)
			if (active[tag] == z) {
				is_active = true;
				break;
			}
		DMERR("gc: no-progress zone=%u tag=%u active=%u wp=%llu dispatch_wp=%llu invalid_hint=%u",
		      z, rows[z].tag, is_active ? 1 : 0,
		      (unsigned long long)rows[z].wp,
		      (unsigned long long)rows[z].dispatch_wp,
		      rows[z].invalid_count);
	}
	kvfree(rows);
}

static u64 gc_wal_sectors_for_data(u64 data_sectors)
{
	return DIV_ROUND_UP_ULL(data_sectors / BLOCK_SECTORS,
				WAL_PAGE_MAX_RECORDS) * WAL_PAGE_SECTORS;
}

/* 호출자가 c->lock을 보유해야 한다. 현재 victim WAL과 다음 victim의 floor가 tail에 함께 들어가야 한다.
 * 부족하면 이주 전에 WAL을 회전해 GC도 floor를 소비하지 않도록 한다. */
static bool gc_wal_tail_needs_roll(struct zns_base_c *c, u64 wal_sectors)
{
	unsigned int z = c->zp->active_zone[ZONE_TAG_WAL];
	u64 tail;

	if (!c->gc_wal_reserve || z == ZONE_NONE)
		return false;
	tail = c->zp->zone_sectors - c->zp->wp[z];
	return tail < wal_sectors + c->gc_wal_reserve;
}

/* 호출자가 c->lock을 보유해야 한다. 이주 데이터와 WAL에 필요한 추가 zone 수를 계산한다.
 * 계산에 포함한 WAL tail은 같은 락에서 gc_wal_budget으로 보호해야 foreground가 소비하지 못한다. */
static unsigned int gc_workspace_required(struct zns_base_c *c, u64 live_sectors)
{
	unsigned int data_z = c->zp->active_zone[ZONE_TAG_GC_DATA];
	unsigned int wal_z = c->zp->active_zone[ZONE_TAG_WAL];
	u64 data_tail = data_z == ZONE_NONE ? 0 : c->zp->zone_sectors - c->zp->wp[data_z];
	u64 wal_tail = wal_z == ZONE_NONE ? 0 : c->zp->zone_sectors - c->zp->wp[wal_z];
	u64 wal_sectors = gc_wal_sectors_for_data(live_sectors);
	u64 wal_required = wal_sectors + c->gc_wal_reserve;

	return (live_sectors > data_tail ? 1 : 0) +
		(wal_required > wal_tail ? 1 : 0);
}

/* 평상시에는 데이터와 WAL 이주 비용을 즉시 상환하는 victim만 허용한다.
 * reserve 경계에서는 데이터 자체에 무효 공간이 있으면 bootstrap을 허용하되, 100% live인 victim은 거절한다.
 * 호출자는 전체 data·WAL workspace를 별도로 확인하고 회수 뒤 대기 중인 foreground에 양보한다. */
static bool gc_reclaim_gain_allowed(u64 used_sectors, u64 live_sectors,
				    unsigned int free_zones)
{
	u64 wal_sectors;

	if (live_sectors >= used_sectors)
		return false;
	wal_sectors = gc_wal_sectors_for_data(live_sectors);
	return live_sectors + wal_sectors < used_sectors ||
		free_zones <= gc_reserved_zones;
}

/* 호출자가 c->lock을 보유해야 한다. seed의 모든 overwrite와 full-victim 이주를 감당할 WAL을 계산한다.
 * reserve zone 하나를 빌려도 다른 free zone은 이주 데이터용으로 남아 있어야 한다. */
static u64 gc_seed_wal_required(struct zns_base_c *c)
{
	u64 max_data = c->zp->zone_sectors - 1;
	u64 full_gc_wal = gc_wal_sectors_for_data(max_data);
	u64 gc_wal = c->gc_wal_budget > full_gc_wal ? c->gc_wal_budget : full_gc_wal;
	/* 배치 효과에 의존하지 않고 4KiB 쓰기마다 WAL 한 섹터를 소비하는 최악값을 잡는다. checkpoint 한 섹터도 남긴다. */
	u64 seed_wal = max_data / BLOCK_SECTORS + 1;

	return gc_wal + seed_wal;
}

static bool gc_seed_borrow_allowed(struct zns_base_c *c)
{
	unsigned int wal_z = c->zp->active_zone[ZONE_TAG_WAL];
	u64 wal_tail;

	if (gc_count_free_zones(c->zp) != gc_reserved_zones ||
	    gc_reserved_zones < 2 || wal_z == ZONE_NONE)
		return false;
	wal_tail = c->zp->zone_sectors - c->zp->wp[wal_z];
	return wal_tail >= gc_seed_wal_required(c);
}

/* 호출자가 c->lock을 보유해야 한다. seed나 GC tail을 넘기기 전에 다음 victim까지 보호할 WAL이 있는지 확인한다.
 * free=1에서도 회전을 허용한다. 새 WAL이 마지막 zone을 잠시 사용하고 checkpoint 뒤 이전 WAL을 회수한다.
 * 강제 flush는 기존 SSTable tail에 들어가야 하며, 완료 credit은 한 번의 handoff·seed에만 사용한다. */
static bool gc_wal_rotation_needed(struct zns_base_c *c)
{
	unsigned int wal_z = c->zp->active_zone[ZONE_TAG_WAL];
	unsigned int sst_z = c->zp->active_zone[ZONE_TAG_SSTABLE];
	unsigned int free_zones = gc_count_free_zones(c->zp);
	u64 wal_tail;
	bool wal_short;
	u64 sstable_sectors;
	u64 sstable_tail;

	if (c->wal_rotation_pending || c->wal_ckpt_inflight ||
	    c->wal_rotation_credit ||
	    !free_zones || free_zones > gc_reserved_zones ||
	    gc_reserved_zones < 2 || wal_z == ZONE_NONE ||
	    sst_z == ZONE_NONE || !c->memtable->count)
		return false;

	wal_tail = c->zp->zone_sectors - c->zp->wp[wal_z];
	if (free_zones == gc_reserved_zones) {
		/* fully-live 상태에서는 foreground overwrite를 허용해야 새 무효 공간이 생긴다. */
		wal_short = wal_tail < gc_seed_wal_required(c);
	} else {
		if (!c->gc_wal_reserve)
			return false;
		wal_short = gc_wal_tail_needs_roll(c, c->gc_wal_reserve);
	}
	if (!wal_short)
		return false;

	sstable_sectors = (512 + round_up(c->memtable->count *
					 sizeof(struct sstable_record), 512)) / 512;
	sstable_tail = c->zp->zone_sectors - c->zp->wp[sst_z];
	return sstable_sectors <= sstable_tail;
}

/* 호출자가 c->lock을 보유해야 한다. GC cycle 동안에는 frozen memtable이 생기지 않도록 제어한다. */
static bool gc_memtable_references_zone(struct zns_base_c *c,
					sector_t start, sector_t end)
{
	struct skiplist_node *node;

	for (node = c->memtable->head->forward[0]; node; node = node->forward[0])
		if (node->phys >= start && node->phys < end)
			return true;
	return false;
}

/* free zone 수가 low watermark 이하이면 GC를 큐잉한다. 이 함수가 c->lock을 잡는다. */
static void maybe_trigger_gc(struct zns_base_c *c)
{
	bool should_gc;

	spin_lock_irq(&c->lock);
	should_gc = gc_count_free_zones(c->zp) <= gc_low_watermark;
	spin_unlock_irq(&c->lock);

	if (should_gc)
		queue_work(zns_gc_wq, &c->gc_work);
}

/* 닫힌 USER_DATA·GC_DATA zone 중 무효 추정치가 가장 큰 후보를 고른다. 이 함수가 c->lock을 잡는다.
 * active zone과 미제출 예약은 제외한다. SSTable은 compaction, WAL은 checkpoint 기반 회수 경로에서 처리한다. */
static unsigned int gc_select_victim(struct zns_base_c *c, const bool *excluded)
{
	unsigned int z, victim = ZONE_NONE;
	unsigned int fallback = ZONE_NONE;
	unsigned int best_invalid = 0;

	spin_lock_irq(&c->lock);
	for (z = 0; z < c->zp->nr_zones; z++) {
		enum zone_tag tag = c->zp->zone_tag[z];

		if (excluded && excluded[z])
			continue;
		if (tag != ZONE_TAG_USER_DATA && tag != ZONE_TAG_GC_DATA)
			continue;
		/* active slot으로 열려 있는 zone은 제외한다. rollover가 tail을 남길 수 있으므로 wp가 용량과 같은지로 판정하지 않는다. */
		if (zone_pool_zone_is_active(c->zp, z))
			continue;
		/* 미제출·미완료 예약이 남은 zone은 제외한다.
		 * foreground는 매핑을 공개한 뒤 예약 회계를 전진시키므로, 두 wp가 일치해야 live-map을 검증할 수 있다. */
		if (c->zp->dispatch_wp[z] != (sector_t)z * c->zp->zone_sectors + c->zp->wp[z])
			continue;
		/* SSTable에 내려간 매핑의 overwrite는 atomic PUT callback에서 옛 phys를
		 * 읽을 수 없어 invalid_count에 즉시 반영되지 않는다. 따라서 알려진
		 * invalid가 없는 경우에도, 닫히고 drain된 data zone 하나를 fallback으로
		 * 실제 live-map 검증 대상으로 허용한다. */
		if (fallback == ZONE_NONE)
			fallback = z;
		if (c->zp->invalid_count[z] == 0)
			continue;
		if (victim == ZONE_NONE || c->zp->invalid_count[z] > best_invalid) {
			victim = z;
			best_invalid = c->zp->invalid_count[z];
		}
	}
	if (victim == ZONE_NONE)
		victim = fallback;
	spin_unlock_irq(&c->lock);
	if (victim != ZONE_NONE && best_invalid == 0)
		DMINFO("gc: no invalid_count candidate; probing fallback data zone %u", victim);
	return victim;
}

static void sstable_snapshot_unpin(struct zns_base_c *c,
				   struct sstable_info *snapshot, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		zone_read_put(c->zp, zone_of(c->zp, snapshot[i].phys));
}

/* GC가 512B를 phys에 써서 durable까지 블로킹. 반드시 async gate (zone_dispatch_write)로 제출.
 * wait_turn을 쓰면 dispatch_wp만 먼저 전진해 뒤따르는 async 쓰기가 디바이스에 먼저 나가 순차쓰기 위반(EIO)이 난다.
 * [반환값] 0/-EIO. process context 전용. */
struct gc_gate_write {
	struct completion done;
	blk_status_t status;
};

static void gc_gate_write_end(struct bio *bio)
{
	struct gc_gate_write *w = bio->bi_private;

	w->status = bio->bi_status;
	bio_put(bio);
	complete(&w->done);
}

static int gc_sync_gate_write(struct zns_base_c *c, sector_t phys, void *buf512,
			      bool append)
{
	struct gc_gate_write w;
	struct bio *bio = bio_alloc(GFP_KERNEL, 1);

	if (!bio) {
		zone_dispatch_cancel(c, phys, 1);
		return -ENOMEM;
	}
	init_completion(&w.done);
	w.status = 0;
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = append ? (zone_of(c->zp, phys) * c->zp->zone_sectors) : phys;
	bio->bi_opf = append ? REQ_OP_ZONE_APPEND : REQ_OP_WRITE;
	bio_add_page(bio, virt_to_page(buf512), 512, offset_in_page(buf512));
	bio->bi_end_io = gc_gate_write_end;
	bio->bi_private = &w;
	if (append) {
		if (zone_append_write(c, zone_of(c->zp, phys), bio)) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
		}
	} else {
		zone_dispatch_write(c, phys, 1, bio);
	}
	wait_for_completion(&w.done);
	if (append)
		zone_dispatch_cancel(c, phys, 1);
	return w.status ? -EIO : 0;
}

/* reserve 경계에서 새 매핑을 보류하고 이미 허용된 append를 drain한다.
 * 새 WAL header를 영속화한 뒤 memtable을 강제 flush하고 새 세대에 checkpoint를 기록한다.
 * wal_reclaim_work_fn이 이전 WAL zone을 회수한 뒤에만 foreground 할당을 재개한다. */
static void wal_rotate_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(to_delayed_work(work),
					     struct zns_base_c, wal_rotate_work);
	struct memtable_flush_work *flush_work;
	struct zone_header *hdr;
	unsigned int old_zone, new_zone;
	sector_t hdr_phys;
	bool drained;
	int ret;

	spin_lock_irq(&c->lock);
	if (!c->wal_rotation_pending || c->metadata_failed || c->stopping) {
		spin_unlock_irq(&c->lock);
		return;
	}
	drained = !c->gc_active && !c->wal_ckpt_inflight &&
		!c->wal_batch_busy && list_empty(&c->wal_pending) &&
		atomic_read(&c->foreground_writes) == 0;
	if (!drained) {
		spin_unlock_irq(&c->lock);
		mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
		mod_delayed_work(zns_gc_wq, &c->wal_rotate_work,
				 msecs_to_jiffies(20));
		return;
	}
	if (c->wal_rotation_old_zone != ZONE_NONE) {
		old_zone = c->wal_rotation_old_zone;
		new_zone = c->zp->active_zone[ZONE_TAG_WAL];
		spin_unlock_irq(&c->lock);
		goto schedule_flush;
	}
	old_zone = c->zp->active_zone[ZONE_TAG_WAL];
	if (old_zone == ZONE_NONE || !gc_count_free_zones(c->zp) ||
	    gc_count_free_zones(c->zp) > gc_reserved_zones) {
		c->wal_rotation_pending = false;
		spin_unlock_irq(&c->lock);
		mod_delayed_work(zns_wq, &c->pending_write_work, 0);
		return;
	}
	new_zone = zone_pool_acquire_free(c->zp, ZONE_TAG_WAL, true);
	if (new_zone == ZONE_NONE) {
		spin_unlock_irq(&c->lock);
		mod_delayed_work(zns_gc_wq, &c->wal_rotate_work,
				 msecs_to_jiffies(20));
		return;
	}
	c->zp->active_zone[ZONE_TAG_WAL] = new_zone;
	c->zp->wal_gen[new_zone] = c->zp->wal_next_gen++;
	c->wal_rotation_old_zone = old_zone;
	hdr_phys = (sector_t)new_zone * c->zp->zone_sectors;
	spin_unlock_irq(&c->lock);

	hdr = kzalloc(512, GFP_KERNEL);
	if (!hdr) {
		zone_dispatch_cancel(c, hdr_phys, 1);
		ret = -ENOMEM;
	} else {
		hdr->magic = cpu_to_le32(ZONE_HEADER_MAGIC);
		hdr->tag = cpu_to_le32(ZONE_TAG_WAL);
		hdr->gen = cpu_to_le64(c->zp->wal_gen[new_zone]);
		ret = gc_sync_gate_write(c, hdr_phys, hdr, false);
		kfree(hdr);
	}
	if (ret) {
		zone_quarantine(c, new_zone, ZONE_TAG_WAL, BLK_STS_IOERR);
		spin_lock_irq(&c->lock);
		if (c->zp->active_zone[ZONE_TAG_WAL] == ZONE_NONE)
			c->zp->active_zone[ZONE_TAG_WAL] = old_zone;
		c->wal_rotation_pending = false;
		c->wal_rotation_old_zone = ZONE_NONE;
		spin_unlock_irq(&c->lock);
		DMERR("WAL rotation: new zone %u header failed (%d); restored old zone %u",
		      new_zone, ret, old_zone);
		mod_delayed_work(zns_wq, &c->pending_write_work, 0);
		return;
	}
	zone_append_header_done(c, new_zone, 0);

schedule_flush:
	spin_lock_irq(&c->lock);
	flush_work = schedule_memtable_flush_locked(c, true);
	spin_unlock_irq(&c->lock);
	if (!flush_work) {
		/* atomic 할당 실패는 재시도한다. foreground를 보류한 채 새 generation을 중복 생성하지 않는다. */
		mod_delayed_work(zns_gc_wq, &c->wal_rotate_work,
				 msecs_to_jiffies(20));
		return;
	}

	DMINFO("WAL rotation: old zone %u -> new zone %u; forcing checkpoint",
	       old_zone, new_zone);
	queue_work(zns_flush_wq, &flush_work->work);
}

/* c->lock을 보유해야 한다. 복사와 WAL 기록이 durable해도 매핑 삽입에 실패하면 원본 zone을 reset하면 안 된다. */
static int gc_commit_relocation(struct zns_base_c *c,
				struct gc_live_entry **entries, unsigned int count, sector_t data_phys)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		sector_t new_phys = data_phys + (sector_t)i * BLOCK_SECTORS;
		int put = mapping_put_if_match(c, entries[i]->lba,
					       entries[i]->phys, new_phys);

		if (put < 0)
			return put;
		if (put == 0)
			entries[i]->phys = new_phys;
		else if (!mapping_get(c, entries[i]->lba, &entries[i]->phys))
			return -EAGAIN;
	}
	return 0;
}

/* 최대 한 WAL page에 들어가는 live block을 연속 GC_DATA 공간에 기록하고,
 * 같은 묶음의 조건부 mapping 갱신을 WAL page 하나로 durable하게 만든다. */
static int gc_relocate_batch(struct zns_base_c *c,
			     struct gc_live_entry **entries, unsigned int count)
{
	void *data = NULL;
	struct wal_page **wal_pages = NULL;
	sector_t data_phys, wal_phys;
	sector_t data_sectors = (sector_t)count * BLOCK_SECTORS;
	sector_t wal_sectors;
	int new_data_zone = -1, new_wal_zone = -1;
	unsigned int nr_wal_pages, i, p;
	int ret = 0;

	if (!count || count > WAL_PAGE_MAX_RECORDS * GC_RELOC_WAL_PAGES)
		return -EINVAL;
	nr_wal_pages = DIV_ROUND_UP(count, WAL_PAGE_MAX_RECORDS);
	wal_sectors = (sector_t)nr_wal_pages * WAL_PAGE_SECTORS;
	data = kvzalloc((size_t)data_sectors * 512, GFP_KERNEL);
	wal_pages = kcalloc(nr_wal_pages, sizeof(*wal_pages), GFP_KERNEL);
	if (!data || !wal_pages) {
		ret = -ENOMEM;
		goto out;
	}
	for (p = 0; p < nr_wal_pages; p++) {
		wal_pages[p] = (struct wal_page *)get_zeroed_page(GFP_KERNEL);
		if (!wal_pages[p]) {
			ret = -ENOMEM;
			goto out;
		}
	}
	for (i = 0; i < count;) {
		unsigned int run = 1;

		/* victim 엔트리는 phys 순으로 정렬되어 있다. 연속 블록을
		 * 개별 4KiB sync read로 내지 않고 하나의 큰 read로 합쳐 FEMU
		 * 왕복 비용과 GC 시간을 줄인다. */
		while (i + run < count &&
		       entries[i + run]->phys ==
		       entries[i]->phys + (sector_t)run * BLOCK_SECTORS)
			run++;
		ret = sstable_io_sync(c, REQ_OP_READ, entries[i]->phys,
				       (char *)data + (size_t)i * PAGE_SIZE,
				       (sector_t)run * BLOCK_SECTORS);
		if (ret) {
			DMERR("gc: batch source read failed at phys=%llu (%d)",
			      (unsigned long long)entries[i]->phys, ret);
			goto out;
		}
		i += run;
	}

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_GC_DATA, data_sectors,
			      &data_phys, &new_data_zone, true);
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("gc: data allocation failed (sectors=%llu err=%d)",
		      (unsigned long long)data_sectors, ret);
		goto out;
	}
	if (new_data_zone >= 0) {
		struct zone_header *hdr = kzalloc(512, GFP_KERNEL);
		sector_t hdr_phys = (sector_t)new_data_zone * c->zp->zone_sectors;

		if (!hdr) {
			zone_dispatch_cancel(c, hdr_phys, 1);
			zone_dispatch_cancel(c, data_phys, data_sectors);
			zone_quarantine(c, new_data_zone, ZONE_TAG_GC_DATA,
					BLK_STS_RESOURCE);
			ret = -ENOMEM;
			goto out;
		}
		hdr->magic = cpu_to_le32(ZONE_HEADER_MAGIC);
		hdr->tag = cpu_to_le32(ZONE_TAG_GC_DATA);
		ret = gc_sync_gate_write(c, hdr_phys, hdr, false);
		kfree(hdr);
		if (ret) {
			zone_dispatch_cancel(c, data_phys, data_sectors);
			zone_quarantine(c, new_data_zone, ZONE_TAG_GC_DATA, BLK_STS_IOERR);
			goto out;
		}
		zone_append_header_done(c, new_data_zone, 0);
	}
	zone_dispatch_wait_turn(c, data_phys, data_sectors);
	ret = sstable_io_sync(c, REQ_OP_WRITE | REQ_FUA, data_phys, data,
			      data_sectors);
	if (ret) {
		DMERR("gc: batch data write failed (%u entries, err=%d)", count, ret);
		zone_quarantine(c, zone_of(c->zp, data_phys), ZONE_TAG_GC_DATA,
				BLK_STS_IOERR);
		goto out;
	}

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_WAL, wal_sectors,
			      &wal_phys, &new_wal_zone, true);
	if (!ret && c->gc_wal_budget >= wal_sectors)
		c->gc_wal_budget -= wal_sectors;
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("gc: WAL allocation failed (err=%d)", ret);
		goto out;
	}
	if (new_wal_zone >= 0) {
		struct zone_header *hdr = kzalloc(512, GFP_KERNEL);
		sector_t hdr_phys = (sector_t)new_wal_zone * c->zp->zone_sectors;

		if (!hdr) {
			zone_dispatch_cancel(c, hdr_phys, 1);
			zone_dispatch_cancel(c, wal_phys, wal_sectors);
			zone_quarantine(c, new_wal_zone, ZONE_TAG_WAL,
					BLK_STS_RESOURCE);
			ret = -ENOMEM;
			goto out;
		}
		hdr->magic = cpu_to_le32(ZONE_HEADER_MAGIC);
		hdr->tag = cpu_to_le32(ZONE_TAG_WAL);
		spin_lock_irq(&c->lock);
		hdr->gen = cpu_to_le64(c->zp->wal_gen[new_wal_zone]);
		spin_unlock_irq(&c->lock);
		ret = gc_sync_gate_write(c, hdr_phys, hdr, false);
		kfree(hdr);
		if (ret) {
			zone_dispatch_cancel(c, wal_phys, wal_sectors);
			zone_quarantine(c, new_wal_zone, ZONE_TAG_WAL, BLK_STS_IOERR);
			goto out;
		}
		zone_append_header_done(c, new_wal_zone, 0);
	}

	for (p = 0; p < nr_wal_pages; p++) {
		struct wal_page *wal = wal_pages[p];
		unsigned int first = p * WAL_PAGE_MAX_RECORDS;
		unsigned int page_count = min_t(unsigned int,
			WAL_PAGE_MAX_RECORDS, count - first);

		wal->magic = cpu_to_le32(WAL_PAGE_MAGIC);
		wal->version = cpu_to_le16(WAL_PAGE_VERSION);
		wal->count = cpu_to_le16(page_count);
		for (i = 0; i < page_count; i++) {
			unsigned int entry = first + i;

			wal->records[i].type = cpu_to_le32(WAL_REC_GC_PUT);
			wal->records[i].gc_put.lba = cpu_to_le64(entries[entry]->lba);
			wal->records[i].gc_put.phys = cpu_to_le64(data_phys +
							 (sector_t)entry * BLOCK_SECTORS);
			wal->records[i].gc_put.expected_old =
				cpu_to_le64(entries[entry]->phys);
		}
		wal->crc32 = cpu_to_le32(crc32(~0U, wal, PAGE_SIZE));
	}
	/* 데이터 영속화 뒤 CRC를 가진 WAL 페이지를 기록한다. 마지막 페이지의 PREFLUSH/FUA로 앞선 페이지까지 영속화한다. */
	for (p = 0; p < nr_wal_pages; p++) {
		ret = wal_page_append_sync(c,
			wal_phys + (sector_t)p * WAL_PAGE_SECTORS,
			wal_pages[p], WAL_PAGE_SECTORS, false);
		if (ret) {
			DMERR("gc: grouped WAL append failed (page=%u/%u entries=%u err=%d)",
			      p + 1, nr_wal_pages, count, ret);
			zone_quarantine(c, zone_of(c->zp, wal_phys), ZONE_TAG_WAL,
					BLK_STS_IOERR);
			goto out;
		}
	}
	ret = blkdev_issue_flush(c->dev->bdev);
	if (ret) {
		DMERR("gc: grouped WAL flush failed (%u pages, err=%d)",
		      nr_wal_pages, ret);
		zone_quarantine(c, zone_of(c->zp, wal_phys), ZONE_TAG_WAL,
				BLK_STS_IOERR);
		goto out;
	}

	for (p = 0; p < nr_wal_pages; p++) {
		unsigned int first = p * WAL_PAGE_MAX_RECORDS;
		unsigned int page_count = min_t(unsigned int,
			WAL_PAGE_MAX_RECORDS, count - first);

		spin_lock_irq(&c->lock);
		ret = gc_commit_relocation(c, &entries[first], page_count,
					   data_phys + (sector_t)first * BLOCK_SECTORS);
		spin_unlock_irq(&c->lock);
		if (ret)
			break;
	}
out:
	if (wal_pages) {
		for (p = 0; p < nr_wal_pages; p++)
			if (wal_pages[p])
				free_page((unsigned long)wal_pages[p]);
		kfree(wal_pages);
	}
	kvfree(data);
	return ret;
}

/* victim 회수 결과다. 이주와 매핑 갱신이 성공한 뒤에만 원본 zone을 reset한다. */
enum gc_reclaim_result {
	GC_RECLAIM_STOP = 0,
	GC_RECLAIM_DONE,
	GC_RECLAIM_TRY_NEXT,
};

static int gc_live_entry_phys_cmp(const void *a, const void *b)
{
	const struct gc_live_entry * const *ea = a;
	const struct gc_live_entry * const *eb = b;

	if ((*ea)->phys < (*eb)->phys)
		return -1;
	if ((*ea)->phys > (*eb)->phys)
		return 1;
	return 0;
}

/* cycle의 latest-map을 재사용하며 victim 하나를 회수한다. I/O는 락 밖에서 수행한다.
 * 매핑 커밋은 조건부 갱신하며 WAL_REC_GC_PUT의 expected_old로 replay 중에도 최신 매핑을 보호한다. */
static enum gc_reclaim_result gc_reclaim_one_victim(struct zns_base_c *c,
						    struct gc_cycle *cycle)
{
	unsigned int victim;
	unsigned int invalid_hint;
	unsigned int free_at_start;
	sector_t vstart, vend;

	struct gc_candidate *mt_candidates = NULL;
	unsigned int nr_mt = 0, mt_cap = 0;
	struct sstable_info *snapshot = NULL;
	unsigned int snap_count = 0;
	unsigned int snap_cap = 0;
	struct gc_live_map live_map = { 0 };
	struct skiplist_node *node;
	unsigned int i;

	unsigned int nr_relocated = 0;
	struct gc_live_entry **reloc_batch = NULL;
	unsigned int reloc_count = 0;
	u64 live_sectors = 0;
	u64 used_sectors;
	unsigned int threshold;
	sector_t active_used = 0;

	bool cached_at_entry = cycle->built;
	bool ok = true;
	bool reclaimed = false;
	bool foreground_paused = false;

	unsigned long victim_started = jiffies;
	unsigned long relocation_started;

	victim = gc_select_victim(c, cycle->excluded);
	if (victim == ZONE_NONE)
		return GC_RECLAIM_STOP; /* 회수할 만한 zone이 없음 */

	spin_lock_irq(&c->lock);
	invalid_hint = c->zp->invalid_count[victim];
	free_at_start = gc_count_free_zones(c->zp);
	spin_unlock_irq(&c->lock);
	DMINFO("gc: selected victim %u (invalid_hint=%u, free_zones=%u, cached_map=%u)",
	       victim, invalid_hint, free_at_start, cycle->built ? 1 : 0);

	vstart = (sector_t)victim * c->zp->zone_sectors;
	vend = vstart + c->zp->zone_sectors;
	if (cycle->built) {
		live_map = cycle->latest;
		snapshot = cycle->sstables;
		snap_count = cycle->nr_sstables;
		goto refresh_current;
	}

	/* 1) memtable 스냅샷. worker process context에서 먼저 크기를 세고 큰
	 * 배열은 락 밖에서 GFP_KERNEL로 할당한다. 기존의 spinlock 안
	 * krealloc(GFP_ATOMIC)은 대형 Kafka memtable에서 반복 OOM을 냈다. */
	spin_lock_irq(&c->lock);
	for (node = c->memtable->head->forward[0]; node; node = node->forward[0]) {
		mt_cap++;
	}
	spin_unlock_irq(&c->lock);

	if (mt_cap)
		mt_candidates = kvmalloc_array(mt_cap, sizeof(*mt_candidates), GFP_KERNEL);
	if (mt_cap && !mt_candidates) {
		DMERR("gc: out of memory snapshotting memtable, aborting this round");
		goto out_free_mt;
	}

	spin_lock_irq(&c->lock);
	for (node = c->memtable->head->forward[0]; node && nr_mt < mt_cap;
	     node = node->forward[0]) {
		mt_candidates[nr_mt].lba = node->lba;
		mt_candidates[nr_mt].phys = node->phys;
		nr_mt++;
	}
	spin_unlock_irq(&c->lock);

	/* 2) SSTable 스캔 — 오래된 세대부터 한 번씩 순차 스캔하며
	 * victim을 한 번이라도 가리킨 LBA의 latest phys만 해시에 남긴다. */
	/* SSTable 수에 비례하는 할당은 락 밖 process context에서 한다.
	 * snapshot 중 인덱스가 커지면 이번 라운드의 상한까지만 본다.
	 * 복사와 pin은 같은 락 안에서 해 compaction reset과 경쟁하지 않는다. */
	spin_lock_irq(&c->lock);
	snap_cap = c->nr_sstables;
	spin_unlock_irq(&c->lock);
	if (snap_cap)
		snapshot = kvmalloc_array(snap_cap, sizeof(*snapshot), GFP_KERNEL);
	if (snap_cap && !snapshot) {
		DMERR("gc: out of memory snapshotting SSTable index, aborting this round");
		goto out_free_mt;
	}
	spin_lock_irq(&c->lock);
	snap_count = min(snap_cap, c->nr_sstables);
	if (snap_count)
		memcpy(snapshot, c->sstables, snap_count * sizeof(*snapshot));
	for (i = 0; i < snap_count; i++)
		zone_read_get(c->zp, zone_of(c->zp, snapshot[i].phys));
	spin_unlock_irq(&c->lock);
	/* latest overwrite가 뒤에 오도록 오래된 SSTable부터 처리. */
	if (snap_count > 1)
		sort(snapshot, snap_count, sizeof(*snapshot), sstable_info_cmp_seq_asc, NULL);

	for (i = 0; i < snap_count && ok; i++) {
		struct sstable_info *si = &snapshot[i];
		size_t record_bytes = si->record_count * sizeof(struct sstable_record);
		size_t alloc_bytes = round_up(record_bytes, PAGE_SIZE);
		sector_t record_sectors = DIV_ROUND_UP(record_bytes, 512);
		struct sstable_record *recs;
		void *buf;
		unsigned int r;
		int ret;

		/* 큰 SSTable도 담을 수 있게 kvmalloc + 청크 읽기 */
		buf = kvzalloc(alloc_bytes, GFP_KERNEL);
		if (!buf) {
			DMERR("gc: out of memory reading SSTable (seq=%llu), aborting this round without reset",
			      (unsigned long long)si->seq_no);
			ok = false;
			break;
		}
		ret = sstable_io_sync(c, REQ_OP_READ, si->phys + 1, buf, record_sectors);
		if (ret) {
			DMERR("gc: failed to read SSTable (seq=%llu, err=%d), aborting this round without reset",
			      (unsigned long long)si->seq_no, ret);
			kvfree(buf);
			ok = false;
			break;
		}

		recs = buf;
		for (r = 0; r < si->record_count; r++) {
			sector_t phys = le64_to_cpu(recs[r].phys);
			u64 rlba = le64_to_cpu(recs[r].lba);

			if (gc_live_map_update(&live_map, rlba, phys, true)) {
				DMERR("gc: out of memory building victim latest-map, aborting this round without reset");
				ok = false;
				break;
			}
		}
		kvfree(buf);
	}

	if (!ok) {
		DMERR("gc: failed to build a complete latest-map, aborting without resetting victim zone %u", victim);
		goto out_free_snapshot;
	}

	/* memtable이 모든 SSTable보다 최신. 스냅샷 시점에 victim을
	 * 가리킨 엔트리를 등록/갱신해 SSTable 결과를 덮어쓴다. */
	for (i = 0; i < nr_mt; i++) {
		if (gc_live_map_update(&live_map, mt_candidates[i].lba,
				       mt_candidates[i].phys, true)) {
			DMERR("gc: out of memory merging memtable into latest-map, aborting without reset");
			ok = false;
			break;
		}
	}
	if (!ok)
		goto out_free_snapshot;
	cycle->latest = live_map;
	if (metrics_enabled)
		atomic64_set(&c->metric_gc_scratch_bytes,
			(u64)live_map.capacity * sizeof(*live_map.entries) +
			(u64)snap_count * sizeof(*snapshot));
	cycle->sstables = snapshot;
	cycle->nr_sstables = snap_count;
	cycle->built = true;
	DMINFO("gc: latest-map ready for victim %u (entries=%u, memtable=%u, sstables=%u, elapsed=%ums)",
	       victim, live_map.count, nr_mt, snap_count,
	       jiffies_to_msecs(jiffies - victim_started));

refresh_current:
	/* 이번 cycle에서 zone이 닫힌 새 LBA도 누락하지 않고 latest-map에 삽입한다. */
	{
		int refresh_ret = gc_refresh_live_map(c, &live_map);

		cycle->latest = live_map; /* resize로 버퍼 주소가 바뀔 수 있음 */
		if (metrics_enabled)
			atomic64_set(&c->metric_gc_scratch_bytes,
				(u64)live_map.capacity * sizeof(*live_map.entries) +
				(u64)snap_count * sizeof(*snapshot));
		if (refresh_ret) {
			DMERR("gc: live-map refresh failed (err=%d), no reset", refresh_ret);
			goto out_free_snapshot;
		}
	}
	/* 스냅샷 병합과 경합한 overwrite를 다시 반영한다. 닫히고 drain된 victim에는 새 foreground 쓰기가 들어오지 않는다. */
	spin_lock_irq(&c->lock);
	for (node = c->memtable->head->forward[0]; node; node = node->forward[0]) {
		struct gc_live_entry *entry = gc_live_map_find(&live_map, node->lba);

		if (entry)
			entry->phys = node->phys;
	}
	spin_unlock_irq(&c->lock);

	for (i = 0; i < live_map.capacity; i++) {
		struct gc_live_entry *entry = &live_map.entries[i];

		if (entry->used && entry->phys >= vstart && entry->phys < vend)
			live_sectors += BLOCK_SECTORS;
	}
	used_sectors = READ_ONCE(c->zp->wp[victim]);
	used_sectors = used_sectors > 0 ? used_sectors - 1 : 0;
	spin_lock_irq(&c->lock);
	if (c->zp->active_zone[ZONE_TAG_USER_DATA] != ZONE_NONE)
		active_used = c->zp->wp[c->zp->active_zone[ZONE_TAG_USER_DATA]];
	else
		active_used = c->zp->zone_sectors;
	threshold = gc_reclaim_threshold(gc_count_free_zones(c->zp), gc_reserved_zones,
					active_used, c->zp->zone_sectors);
	spin_unlock_irq(&c->lock);
	gc_diag_audit(c, &live_map, victim, used_sectors, live_sectors,
		      cached_at_entry);
	/* 여유 zone이 reserve보다 남아 있는 동안에는 아주 작은 순이익을
	 * 위해 거의 전체 zone을 복사하지 않는다. FEMU의 2GiB zone에서는
	 * invalid 1% 미만 victim 하나가 foreground를 60초 이상 막을 수 있다.
	 * reserve까지 내려간 긴급 상황에서는 양의 순이익만 있으면 허용한다. */
	if (free_at_start > gc_reserved_zones &&
	    (used_sectors - live_sectors) * 100 < used_sectors * threshold) {
		DMINFO("gc: skipping victim %u: reclaim gain below %u%% (used=%llu, live=%llu)",
		       victim, threshold, (unsigned long long)used_sectors,
		       (unsigned long long)live_sectors);
		cycle->excluded[victim] = true;
		kvfree(mt_candidates);
		return GC_RECLAIM_TRY_NEXT;
	}
	/* 여유가 있을 때는 relocation WAL까지 즉시 상환하는 victim만 고른다.
	 * reserve 경계에서는 data 자체의 작은 순이익으로 foreground tail을
	 * 만드는 bootstrap을 허용한다. 100% live victim은 항상 거절한다. */
	if (!gc_reclaim_gain_allowed(used_sectors, live_sectors, free_at_start)) {
		DMINFO("gc: skipping victim %u: no positive reclaim gain (used=%llu, live=%llu)",
		       victim, (unsigned long long)used_sectors,
		       (unsigned long long)live_sectors);
		cycle->excluded[victim] = true;
		kvfree(mt_candidates);
		return GC_RECLAIM_TRY_NEXT;
	}
	if (live_sectors + gc_wal_sectors_for_data(live_sectors) >= used_sectors)
		DMINFO("gc: reserve-boundary bootstrap victim %u (used=%llu, live=%llu, wal=%llu, free_zones=%u)",
		       victim, (unsigned long long)used_sectors,
		       (unsigned long long)live_sectors,
		       (unsigned long long)gc_wal_sectors_for_data(live_sectors),
		       free_at_start);
	DMINFO("gc: relocating victim %u (used=%llu, live=%llu, invalid=%llu, entries=%llu, free_zones=%u)",
	       victim, (unsigned long long)used_sectors,
	       (unsigned long long)live_sectors,
	       (unsigned long long)(used_sectors - live_sectors),
	       (unsigned long long)(live_sectors / BLOCK_SECTORS),
	       free_at_start);
	relocation_started = jiffies;
	/* 새 foreground 할당을 막고 이미 허용한 쓰기를 먼저 drain한다.
	 * victim WAL budget을 먼저 보호하면 기존 요청의 WAL이 막혀 drain과 교착될 수 있다. */
	spin_lock_irq(&c->lock);
	c->gc_foreground_pause = true;
	foreground_paused = true;
	spin_unlock_irq(&c->lock);
	wait_event(c->flush_waitq,
		   atomic_read(&c->foreground_writes) == 0 ||
		   READ_ONCE(c->stopping));
	if (READ_ONCE(c->stopping))
		goto out_free_snapshot;
	DMINFO("gc: foreground admission drained for victim %u relocation", victim);

	/* data와 최악 WAL workspace를 모두 확보할 수 없으면 이주를 시작하지 않는다. */
	{
		unsigned int need, available;
		u64 victim_wal = gc_wal_sectors_for_data(live_sectors);
		bool roll_wal;
		bool rotation_requested = false;
		bool rotation_pending;

		spin_lock_irq(&c->lock);
		need = gc_workspace_required(c, live_sectors);
		available = gc_count_free_zones(c->zp);
		roll_wal = gc_wal_tail_needs_roll(c, victim_wal);
		rotation_pending = c->wal_rotation_pending;
		if (available >= need) {
			if (roll_wal)
				c->zp->active_zone[ZONE_TAG_WAL] = ZONE_NONE;
			c->gc_wal_budget = victim_wal;
		} else if (!rotation_pending && gc_wal_rotation_needed(c)) {
			/* foreground tail이 남아 있어도 data·WAL workspace가 동시에 부족할 수 있다.
			 * 마지막 free zone으로 WAL 회전을 요청하고 gc_active를 해제해 회전 worker가 drain·checkpoint를 수행하게 한다. */
			c->wal_rotation_pending = true;
			rotation_requested = true;
		}
		spin_unlock_irq(&c->lock);
		if (available < need) {
			if (rotation_requested) {
				DMINFO("gc: requested reserve-boundary WAL generation rotation before victim %u (free=%u need=%u)",
				       victim, available, need);
				mod_delayed_work(zns_gc_wq, &c->wal_rotate_work, 0);
			} else if (rotation_pending) {
				/* 이미 예약된 회전에 양보한다. 복사와 reset을 시작하지 않았으므로 이주 오류로 기록하지 않는다. */
				DMINFO("gc: yielding victim %u for already pending WAL generation rotation (free=%u need=%u)",
				       victim, available, need);
			} else {
				DMERR("gc: insufficient relocation workspace (free=%u need=%u), no reset",
				      available, need);
			}
			goto out_free_snapshot;
		}
		if (roll_wal)
			DMINFO("gc: rolled WAL tail to preserve next-victim floor (victim_wal=%llu reserve=%llu free=%u)",
			       (unsigned long long)victim_wal,
			       (unsigned long long)c->gc_wal_reserve, available);
	}

	reloc_batch = kvmalloc_array(live_sectors / BLOCK_SECTORS,
				     sizeof(*reloc_batch), GFP_KERNEL);
	if (!reloc_batch) {
		ok = false;
		goto out_free_snapshot;
	}

	/* 최신 phys가 victim 안인 엔트리를 모두 모아 물리 주소순으로 정렬한다.
	 * 이후 batch read가 연속 구간을 병합할 수 있다. */
	for (i = 0; i < live_map.capacity; i++) {
		struct gc_live_entry *entry = &live_map.entries[i];

		if (!entry->used || entry->phys < vstart || entry->phys >= vend)
			continue;
		reloc_batch[reloc_count++] = entry;
	}
	sort(reloc_batch, reloc_count, sizeof(*reloc_batch),
	     gc_live_entry_phys_cmp, NULL);

	/* 이주 중 사용자 overwrite는 각 batch 마지막의
	 * mapping_put_if_match가 걸러낸다. */
	for (i = 0; i < reloc_count;
	     i += WAL_PAGE_MAX_RECORDS * GC_RELOC_WAL_PAGES) {
		unsigned int batch_count = min_t(unsigned int,
			WAL_PAGE_MAX_RECORDS * GC_RELOC_WAL_PAGES,
			reloc_count - i);

		int reloc_ret = gc_relocate_batch(c, &reloc_batch[i], batch_count);

		if (reloc_ret) {
			DMERR("gc: relocation failed (err=%d batch=%u), aborting this round without resetting victim zone %u",
			      reloc_ret, i, victim);
			ok = false;
			break;
		}
		nr_relocated += batch_count;
		if (nr_relocated / 32768 !=
		    (nr_relocated - batch_count) / 32768) {
			u64 elapsed_ms = jiffies_to_msecs(jiffies - relocation_started);

			DMINFO("gc: victim %u progress relocated=%u/%llu (%llums, %llu entries/s)",
			       victim, nr_relocated,
			       (unsigned long long)(live_sectors / BLOCK_SECTORS),
			       (unsigned long long)elapsed_ms,
			       (unsigned long long)(elapsed_ms ?
				div64_u64((u64)nr_relocated * 1000, elapsed_ms) : 0));
		}
	}
	kvfree(reloc_batch);
	reloc_batch = NULL;
	if (!ok)
		goto out_free_snapshot;

	/* 모든 이주 LBA가 memtable에 반영됐는지 확인한다. 누락된 새 키가 있으면 reset하지 않는다. */
	spin_lock_irq(&c->lock);
	ok = !gc_memtable_references_zone(c, vstart, vend);
	spin_unlock_irq(&c->lock);
	if (!ok) {
		DMERR("gc: reset blocked by live memtable reference to zone %u", victim);
		goto out_free_snapshot;
	}

	/* 이주와 잔여 참조 검사가 성공한 뒤 기존 읽기를 drain하고 victim을 reset한다. */
	zone_wait_reads_drained(c->zp, victim);
	if (zone_reset_hw(c, victim)) {
		DMERR("gc: hardware zone reset failed for zone %u — zone leaked until manually recovered", victim);
	} else {
		spin_lock_irq(&c->lock);
		zone_pool_mark_free(c->zp, victim);
		spin_unlock_irq(&c->lock);
		DMINFO("gc: reclaimed zone %u (%u live entries relocated, %u cycle mappings, relocation=%ums, total=%ums)",
		       victim, nr_relocated, live_map.count,
		       jiffies_to_msecs(jiffies - relocation_started),
		       jiffies_to_msecs(jiffies - victim_started));
		reclaimed = true;
		if (metrics_enabled) {
			atomic64_inc(&c->metric_gc_reclaims);
			atomic64_add((u64)nr_relocated * BLOCK_SECTORS * 512,
				&c->metric_gc_moved_bytes);
		}
	}

out_free_snapshot:
	spin_lock_irq(&c->lock);
	c->gc_wal_budget = 0;
	if (foreground_paused)
		c->gc_foreground_pause = false;
	spin_unlock_irq(&c->lock);
	if (foreground_paused)
		wake_up_all(&c->flush_waitq);
	kvfree(reloc_batch);
	if (!cycle->built)
		kvfree(live_map.entries);
	if (!cycle->built && snapshot) {
		sstable_snapshot_unpin(c, snapshot, snap_count);
		kvfree(snapshot);
	}
out_free_mt:
	kvfree(mt_candidates);
	return reclaimed ? GC_RECLAIM_DONE : GC_RECLAIM_STOP;
}

/* 공간 부족 시 GC 또는 WAL 회전을 요청하고 기다리지 않는다.
 * -EAGAIN이면 bio 소유권을 유지한 채 pending 경로에서 재시도한다. 반복 GC 무진전은 -ENOSPC로 종료한다.
 * 이 경로에서 worker를 동기 대기하면 상위 writeback과 GC I/O가 순환 대기할 수 있다. */
static int zone_pool_alloc_with_gc_retry(struct zns_base_c *c, enum zone_tag tag, sector_t nr,
					 sector_t *phys_out, int *new_zone_out)
{
	int ret;
	bool borrowed_seed = false;
	bool handed_off_gc_tail = false;
	bool rotation_requested = false;
	bool rotation_credit_used = false;
	unsigned int handoff_zone = ZONE_NONE;

	spin_lock_irq(&c->lock);
	if (c->metadata_failed) {
		spin_unlock_irq(&c->lock);
		return -EIO;
	}
	if (tag == ZONE_TAG_USER_DATA && c->wal_rotation_pending) {
		spin_unlock_irq(&c->lock);
		return -EAGAIN;
	}
	if (tag == ZONE_TAG_USER_DATA && c->gc_foreground_pause) {
		spin_unlock_irq(&c->lock);
		return -EAGAIN;
	}
	ret = zone_pool_alloc(c->zp, tag, nr, phys_out, new_zone_out, false);
	if (ret == -ENOSPC && tag == ZONE_TAG_USER_DATA &&
	    c->wal_rotation_credit && gc_count_free_zones(c->zp) > 0 &&
	    gc_count_free_zones(c->zp) <= gc_reserved_zones) {
		rotation_credit_used = true;
	}
	if (!rotation_credit_used && ret == -ENOSPC && tag == ZONE_TAG_USER_DATA &&
	    gc_wal_rotation_needed(c)) {
		c->wal_rotation_pending = true;
		rotation_requested = true;
		ret = -EAGAIN;
	}
	if (!rotation_requested && ret == -ENOSPC && tag == ZONE_TAG_USER_DATA &&
	    !c->gc_active && gc_count_free_zones(c->zp) > 0 &&
	    c->gc_wal_reserve) {
		ret = zone_pool_handoff_gc_tail(c->zp, nr, phys_out,
						&handoff_zone);
		if (!ret && c->foreground_handoff_gc_zone != handoff_zone) {
			c->foreground_handoff_gc_zone = handoff_zone;
			handed_off_gc_tail = true;
		}
	}
	if (!rotation_requested && ret == -ENOSPC && tag == ZONE_TAG_USER_DATA &&
	    gc_seed_borrow_allowed(c)) {
		ret = zone_pool_alloc(c->zp, tag, nr, phys_out, new_zone_out, true);
		if (!ret) {
			c->gc_wal_reserve = gc_wal_sectors_for_data(
				c->zp->zone_sectors - 1);
			borrowed_seed = true;
		}
	}
	if (!ret) {
		if (rotation_credit_used)
			c->wal_rotation_credit = false;
		c->gc_no_progress = 0;
		/* 공간 예약과 foreground_writes 증가를 같은 락에서 처리한다.
		 * 회전 worker가 예약된 쓰기를 drain 대상에서 누락하면 안 된다. */
		atomic_inc(&c->foreground_writes);
	} else if (ret == -ENOSPC && c->gc_no_progress < 3) {
		/* zone_pool_alloc() 자체가 -ENOSPC를 반환하므로, GC가 아직
		 * 무진전 판정을 충분히 반복하기 전에는 transient 상태로 바꾼다.
		 * gc_active만으로 EAGAIN을 강제하면 회수 불가능한 동일 상태에서
		 * pending worker가 GC를 계속 재큐잉해 영구 D-state에 빠진다. 새
		 * allocation이 성공하면 gc_no_progress가 0으로 초기화되므로, 3회
		 * 연속 무진전 뒤에는 진행 중인 worker가 있더라도 ENOSPC가 맞다. */
		ret = -EAGAIN;
	}
	spin_unlock_irq(&c->lock);
	if (rotation_requested) {
		DMINFO("foreground paused for reserve-boundary WAL generation rotation");
		mod_delayed_work(zns_gc_wq, &c->wal_rotate_work, 0);
		return -EAGAIN;
	}
	if (borrowed_seed)
		DMINFO("foreground borrowed one reserve zone with protected GC data/WAL workspace (new_zone=%d)",
		       new_zone_out ? *new_zone_out : -1);
	if (handed_off_gc_tail)
		DMINFO("foreground took sole ownership of inactive GC_DATA tail without consuming final reserve zone (zone=%u)",
		       handoff_zone);

	if (ret) {
		if (ret == -ENOSPC) {
			DMERR_LIMIT("foreground allocation exhausted after %u GC cycles without progress",
				    READ_ONCE(c->gc_no_progress));
			return ret;
		}
		queue_work(zns_gc_wq, &c->gc_work);
		return -EAGAIN;
	}
	return 0;
}

/* 이미 DM_MAPIO_SUBMITTED로 인계받은 foreground write를 실제 append 경로에
 * 올린다. -EAGAIN이면 bio를 완료하지 않았으므로 pending FIFO에서 재시도한다. */
static int start_foreground_write(struct zns_base_c *c, struct bio *bio,
				  sector_t block_lba, sector_t nr)
{
	sector_t phys;
	int new_data_zone;
	struct zns_io_ctx *ctx;
	int ret;

	ret = zone_pool_alloc_with_gc_retry(c, ZONE_TAG_USER_DATA, nr, &phys,
					    &new_data_zone);
	if (ret == -EAGAIN)
		return ret;
	if (ret) {
		bio->bi_status = errno_to_blk_status(ret);
		bio_endio(bio);
		return 0;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_NOIO);
	if (!ctx) {
		zone_dispatch_cancel(c, phys, nr);
		bio->bi_status = BLK_STS_RESOURCE;
		foreground_write_done(c);
		bio_endio(bio);
		return 0;
	}
	ctx->c = c;
	ctx->orig_bio = bio;
	ctx->lba = block_lba;
	ctx->reserved_phys = phys;
	ctx->reserved_nr = nr;

	if (nr > BLOCK_SECTORS) {
		unsigned int i, count = nr / BLOCK_SECTORS;

		ctx->extent_children = kcalloc(count, sizeof(*ctx->extent_children), GFP_NOIO);
		if (!ctx->extent_children)
			goto extent_nomem;
		for (i = 0; i < count; i++) {
			struct zns_io_ctx *child = kzalloc(sizeof(*child), GFP_NOIO);
			struct bio *clone;

			if (!child)
				goto extent_nomem;
			clone = bio_clone_fast(bio, GFP_NOIO, &fs_bio_set);
			if (!clone) {
				kfree(child);
				goto extent_nomem;
			}
			bio_trim(clone, (sector_t)i * BLOCK_SECTORS, BLOCK_SECTORS);
			child->c = c;
			child->orig_bio = clone;
			child->lba = block_lba + (sector_t)i * BLOCK_SECTORS;
			child->reserved_phys = phys + (sector_t)i * BLOCK_SECTORS;
			child->reserved_nr = BLOCK_SECTORS;
			ctx->extent_children[ctx->extent_count++] = child;
		}
	}
	ctx->nr_headers = 0;
	if (new_data_zone >= 0) {
		ctx->headers[ctx->nr_headers].zone_id = new_data_zone;
		ctx->headers[ctx->nr_headers].tag = ZONE_TAG_USER_DATA;
		ctx->headers[ctx->nr_headers].gen = 0;
		ctx->nr_headers++;
	}
	ctx->header_idx = 0;
	ctx->on_headers_done = submit_data_append_async;

	if (ctx->nr_headers > 0)
		submit_header_async(ctx);
	else
		submit_data_append_async(ctx);
	maybe_trigger_gc(c);
	return 0;
extent_nomem:
	free_extent_children(ctx);
	zone_dispatch_cancel(c, phys, nr);
	kfree(ctx);
	bio->bi_status = BLK_STS_RESOURCE;
	foreground_write_done(c);
	bio_endio(bio);
	return 0;
}

static void pending_write_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(to_delayed_work(work),
					     struct zns_base_c, pending_write_work);
	struct pending_write_bio *pending;
	bool stopping;

	for (;;) {
		spin_lock_irq(&c->lock);
		if (list_empty(&c->pending_write_bios)) {
			spin_unlock_irq(&c->lock);
			mod_delayed_work(zns_wq, &c->pending_flush_work, 0);
			return;
		}
		pending = list_first_entry(&c->pending_write_bios,
					   struct pending_write_bio, link);
		stopping = c->stopping;
		if (stopping)
			list_del(&pending->link);
		spin_unlock_irq(&c->lock);

		if (stopping) {
			pending->bio->bi_status = BLK_STS_IOERR;
			bio_endio(pending->bio);
			if (pending->metric_queued_ns) {
				atomic64_add(ktime_get_ns() - pending->metric_queued_ns, &c->metric_foreground_wait_ns);
				atomic64_inc(&c->metric_foreground_wait_count);
			}
			kfree(pending);
			continue;
		}

		if (start_foreground_write(c, pending->bio, pending->lba,
					   pending->nr) == -EAGAIN) {
			mod_delayed_work(zns_wq, &c->pending_write_work,
					 msecs_to_jiffies(20));
			return;
		}
		if (pending->metric_queued_ns) {
			atomic64_add(ktime_get_ns() - pending->metric_queued_ns, &c->metric_foreground_wait_ns);
			atomic64_inc(&c->metric_foreground_wait_count);
		}

		spin_lock_irq(&c->lock);
		list_del(&pending->link);
		spin_unlock_irq(&c->lock);
		kfree(pending);
	}
}

/* process context에서 victim을 회수하며 foreground 및 metadata 회전에 양보한다.
 * 후보가 없거나 회수가 중단되면 종료하고, 시도 횟수를 nr_zones로 제한해 무한 순환을 막는다. */
static void gc_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(work, struct zns_base_c, gc_work);
	struct memtable_flush_work *flush_work = NULL;
	bool still_low;
	unsigned int attempts;
	unsigned int reclaimed = 0;
	bool deferred = false;
	struct gc_cycle cycle = { 0 };
	enum gc_reclaim_result result;
	unsigned int free_at_start;
	unsigned int ckpt_inflight;
	unsigned int no_progress;
	bool log_no_progress_state = false;
	bool foreground_waiting;
	u64 metric_started = 0;

	if (READ_ONCE(c->metadata_failed) || READ_ONCE(c->stopping) ||
	    READ_ONCE(c->sstable_rotation_pending) ||
	    READ_ONCE(c->wal_rotation_pending))
		return;

	/* frozen memtable은 active memtable에서는 빠졌지만 SSTable 색인에
	 * 아직 등록되지 않은 순간이 있다. 그 때 latest-map을 만들면
	 * 최신 overwrite를 놓칠 수 있으므로 진행 중 flush가 있으면 미루고,
	 * GC가 끝날 때까지 새 memtable swap을 막는다. */
	spin_lock_irq(&c->lock);
	if (c->metadata_failed || c->stopping || c->wal_rotation_pending ||
	    c->sstable_rotation_pending) {
		spin_unlock_irq(&c->lock);
		return;
	}
	free_at_start = gc_count_free_zones(c->zp);
	ckpt_inflight = c->wal_ckpt_inflight;
	if (c->wal_buffered_count) {
		spin_unlock_irq(&c->lock);
		mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
		return; /* WAL worker가 durable 완료 뒤 GC를 다시 큐잉한다. */
	}
	if (c->wal_ckpt_inflight > 0) {
		deferred = true;
	} else {
		/* foreground tail은 allocator가 정상 rollover할 때까지 유지한다.
		 * 무효 추정치만으로 조기 봉인하면 회수 순이익이 없는 zone의 남은 공간을 잃을 수 있다. */
		c->gc_active = true;
	}
	spin_unlock_irq(&c->lock);
	if (deferred) {
		DMINFO("gc: deferred while checkpoint/flush is in flight (inflight=%u, free_zones=%u)",
		       ckpt_inflight, free_at_start);
		return;
	}
	if (metrics_enabled) {
		metric_started = ktime_get_ns();
		atomic64_inc(&c->metric_gc_runs);
		DMINFO("metrics: gc begin monotonic_ns=%llu", metric_started);
	}
	DMINFO("gc: worker started (free_zones=%u, start=%u, stop=%u, reserve=%u)",
	       free_at_start, gc_low_watermark, gc_stop_watermark(),
	       gc_reserved_zones);

	cycle.excluded = kvcalloc(c->zp->nr_zones, sizeof(*cycle.excluded), GFP_KERNEL);
	if (!cycle.excluded) {
		DMERR("gc: out of memory allocating victim exclusion map");
		goto out_finish;
	}

	for (attempts = 0; attempts < c->zp->nr_zones; attempts++) {
		if (READ_ONCE(c->stopping)) {
			DMINFO("gc: stopping at victim boundary");
			break;
		}
		if (READ_ONCE(c->sstable_rotation_pending)) {
			DMINFO("gc: yielding for reserve-boundary SSTable rotation");
			break;
		}
		result = gc_reclaim_one_victim(c, &cycle);
		if (result == GC_RECLAIM_STOP) {
			DMINFO("gc: stopped without a reclaimable victim (attempt=%u, reclaimed=%u)",
			       attempts, reclaimed);
			break;
		}
		if (result == GC_RECLAIM_TRY_NEXT)
			continue;
		reclaimed++;
		spin_lock_irq(&c->lock);
		still_low = gc_count_free_zones(c->zp) < gc_stop_watermark();
		foreground_waiting = !list_empty(&c->pending_write_bios) ||
			c->wal_pending_count;
		spin_unlock_irq(&c->lock);
		/* 회수에 성공하면 대기 중인 foreground에 양보한다. out_finish가 두 pending 경로를 깨운다. */
		if (foreground_waiting) {
			DMINFO("gc: yielding after reclaim for pending foreground I/O");
			break;
		}
		if (!still_low)
			break;
	}
	if (attempts == c->zp->nr_zones)
		DMERR("gc: hit the per-trigger round cap (%u) — free zones may still be low, will retry on next trigger",
		      c->zp->nr_zones);
	if (cycle.built) {
		sstable_snapshot_unpin(c, cycle.sstables, cycle.nr_sstables);
		kvfree(cycle.sstables);
		kvfree(cycle.latest.entries);
	}
	kvfree(cycle.excluded);

out_finish:
	if (metric_started) {
		atomic64_add(ktime_get_ns() - metric_started, &c->metric_gc_ns);
		DMINFO("metrics: gc end monotonic_ns=%llu reclaimed=%u", ktime_get_ns(), reclaimed);
	}
	atomic64_set(&c->metric_gc_scratch_bytes, 0);
	spin_lock_irq(&c->lock);
	if (reclaimed)
		c->gc_no_progress = 0;
	else if (c->gc_no_progress < UINT_MAX)
		c->gc_no_progress++;
	no_progress = c->gc_no_progress;
	if (!reclaimed && no_progress == 3)
		log_no_progress_state = true;
	c->gc_active = false;
	wake_up_all(&c->flush_waitq);
	/* GC 때문에 임계값 flush를 건너뛴 WAL callback이 있었으면 새 PUT을
	 * 기다리지 않고 즉시 재시도한다. */
	flush_work = schedule_memtable_flush_locked(c, false);
	free_at_start = gc_count_free_zones(c->zp);
	spin_unlock_irq(&c->lock);
	DMINFO("gc: worker finished (reclaimed=%u, free_zones=%u, no_progress=%u)",
	       reclaimed, free_at_start, no_progress);
	if (log_no_progress_state)
		gc_log_no_progress_state(c, no_progress);
	if (flush_work) {
		DMINFO("gc: scheduled deferred memtable flush after worker completion");
		queue_work(zns_flush_wq, &flush_work->work);
	}
	/* WAL 공간 부족으로 보류된 foreground bio가 있으면 100ms 타이머를
	 * 기다리지 않고 방금 회수한 공간을 즉시 사용하게 한다. */
	if (reclaimed && READ_ONCE(c->wal_pending_count))
		mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
	if (reclaimed)
		mod_delayed_work(zns_wq, &c->pending_write_work, 0);
}

/* process context에서 checkpoint 이전 WAL을 회수한다. 후보는 락 아래에서 검사한다.
 * wal_gen < wal_durable_split_gen인 세대만 허용하며, 현재 active WAL은 제외한다.
 * 배정된 쓰기가 모두 제출됐는지 확인하고 reset 성공 뒤에만 free로 전환한다. */
static void wal_reclaim_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(work, struct zns_base_c, wal_reclaim_work);
	unsigned int rounds;

	for (rounds = 0; rounds < c->zp->nr_zones; rounds++) {
		unsigned int z, victim = ZONE_NONE;
		u64 durable;
		bool rotation_completed = false;

		spin_lock_irq(&c->lock);
		durable = c->wal_durable_split_gen;
		for (z = 0; z < c->zp->nr_zones; z++) {
			if (c->zp->zone_tag[z] != ZONE_TAG_WAL)
				continue;
			if (z == c->zp->active_zone[ZONE_TAG_WAL])
				continue;
			if (c->zp->wal_gen[z] >= durable)
				continue;
			if (c->zp->dispatch_wp[z] != (sector_t)z * c->zp->zone_sectors + c->zp->wp[z])
				continue;
			victim = z;
			break;
		}
		spin_unlock_irq(&c->lock);

		if (victim == ZONE_NONE)
			break; /* 더 회수할 WAL zone 없음 */

		if (zone_reset_hw(c, victim)) {
			DMERR("wal reclaim: hardware zone reset failed for zone %u — leaked until reboot", victim);
			break; /* 같은 zone을 무한 재선정하지 않도록 중단 */
		}

		spin_lock_irq(&c->lock);
		zone_pool_mark_free(c->zp, victim);
		if (c->wal_rotation_pending &&
		    victim == c->wal_rotation_old_zone) {
			c->wal_rotation_pending = false;
			c->wal_rotation_credit = true;
			c->wal_rotation_old_zone = ZONE_NONE;
			rotation_completed = true;
		}
		spin_unlock_irq(&c->lock);
		DMINFO("wal reclaim: reclaimed zone %u (gen < %llu)",
		       victim, (unsigned long long)durable);
		if (rotation_completed) {
			DMINFO("WAL rotation: old generation reclaimed; foreground resumed");
			mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
			mod_delayed_work(zns_wq, &c->pending_write_work, 0);
		}
	}
}

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
	sector_t physical_sectors;
	int ret;
	unsigned int i;

	if (argc != 1) {
		ti->error = "expected one argument: underlying device";
		return -EINVAL;
	}
	if (gc_reserved_zones < 2) {
		ti->error = "GC requires at least two reserve zones (data and WAL)";
		return -EINVAL;
	}

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		ti->error = "out of memory";
		return -ENOMEM;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);
	if (ret) {
		ti->error = "failed to open underlying device";
		kfree(c);
		return ret;
	}

	c->zp = kzalloc(sizeof(*c->zp), GFP_KERNEL);
	if (!c->zp) {
		ti->error = "out of memory (zone_pool)";
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	c->zp->zone_sectors = bdev_zone_sectors(c->dev->bdev);
	if (gc_low_watermark <= gc_reserved_zones) {
		ti->error = "GC low watermark must be greater than the GC reserve";
		return -EINVAL;
	}
	if (!wal_batch_max_records || wal_batch_max_records > WAL_PAGE_MAX_RECORDS) {
		ti->error = "wal_batch_max_records is outside the WAL page capacity";
		return -EINVAL;
	}
	if (!wal_group_pages || wal_group_pages > 16 ||
	    !data_append_max_blocks || data_append_max_blocks > 256) {
		ti->error = "invalid append/WAL group configuration";
		return -EINVAL;
	}
	if (bdev_max_zone_append_sectors(c->dev->bdev) < WAL_PAGE_SECTORS) {
		ti->error = "underlying device does not support 4KB zone append";
		return -EOPNOTSUPP;
	}
	physical_sectors = bdev_nr_sectors(c->dev->bdev);
	if (ti->len > physical_sectors) {
		ti->error = "logical target is larger than underlying device";
		return -EINVAL;
	}
	if (!c->zp->zone_sectors || physical_sectors % c->zp->zone_sectors) {
		ti->error = "underlying capacity is not zone aligned";
		return -EINVAL;
	}
	c->nr_sectors = ti->len;
	/* ti->len은 상위 논리 주소 범위다. allocator는 하위 장치 전체의 물리 zone을 관리한다. */
	c->zp->nr_zones = physical_sectors / c->zp->zone_sectors;

	c->zp->wp = kcalloc(c->zp->nr_zones, sizeof(sector_t), GFP_KERNEL);
	if (!c->zp->wp) {
		ti->error = "out of memory (wp)";
		return -ENOMEM;
	}

	/* kcalloc이 0으로 채워주므로 전부 ZONE_TAG_FREE(=0)로 시작 */
	c->zp->zone_tag = kcalloc(c->zp->nr_zones, sizeof(enum zone_tag), GFP_KERNEL);
	if (!c->zp->zone_tag) {
		ti->error = "out of memory (zone_tag)";
		return -ENOMEM;
	}

	c->zp->invalid_count = kcalloc(c->zp->nr_zones, sizeof(unsigned int), GFP_KERNEL);
	if (!c->zp->invalid_count) {
		ti->error = "out of memory (invalid_count)";
		return -ENOMEM;
	}

	c->zp->sstable_live_count = kcalloc(c->zp->nr_zones, sizeof(unsigned int), GFP_KERNEL);
	if (!c->zp->sstable_live_count) {
		ti->error = "out of memory (sstable_live_count)";
		return -ENOMEM;
	}

	c->zp->wal_gen = kcalloc(c->zp->nr_zones, sizeof(u64), GFP_KERNEL);
	if (!c->zp->wal_gen) {
		ti->error = "out of memory (wal_gen)";
		return -ENOMEM;
	}
	c->zp->wal_next_gen = 0;

	c->zp->dispatch_wp = kcalloc(c->zp->nr_zones, sizeof(sector_t), GFP_KERNEL);
	if (!c->zp->dispatch_wp) {
		ti->error = "out of memory (dispatch_wp)";
		return -ENOMEM;
	}
	/* dispatch_wp는 절대 주소이므로 각 zone의 시작 섹터로 초기화한다. wp는 zone 내부 예약 길이다. */
	for (i = 0; i < c->zp->nr_zones; i++)
		c->zp->dispatch_wp[i] = (sector_t)i * c->zp->zone_sectors;

	/* list_head 배열은 0-초기화만으로는 "빈 리스트"가 안 됨 — 각각 INIT_LIST_HEAD. */
	c->zp->dispatch_waiters = kmalloc_array(c->zp->nr_zones, sizeof(struct list_head), GFP_KERNEL);
	if (!c->zp->dispatch_waiters) {
		ti->error = "out of memory (dispatch_waiters)";
		return -ENOMEM;
	}
	for (i = 0; i < c->zp->nr_zones; i++)
		INIT_LIST_HEAD(&c->zp->dispatch_waiters[i]);
	c->zp->append_ready = kcalloc(c->zp->nr_zones, sizeof(bool), GFP_KERNEL);
	if (!c->zp->append_ready) {
		ti->error = "out of memory (append_ready)";
		return -ENOMEM;
	}
	c->zp->append_waiters = kmalloc_array(c->zp->nr_zones,
					       sizeof(struct list_head), GFP_KERNEL);
	if (!c->zp->append_waiters) {
		ti->error = "out of memory (append_waiters)";
		return -ENOMEM;
	}
	for (i = 0; i < c->zp->nr_zones; i++)
		INIT_LIST_HEAD(&c->zp->append_waiters[i]);

	/* kcalloc 0-초기화가 곧 atomic_t 0 — 별도 atomic_set 불필요 */
	c->zp->inflight_reads = kcalloc(c->zp->nr_zones, sizeof(atomic_t), GFP_KERNEL);
	if (!c->zp->inflight_reads) {
		ti->error = "out of memory (inflight_reads)";
		return -ENOMEM;
	}
	init_waitqueue_head(&c->zp->reclaim_waitq);

	/* 0은 "zone 0번"이라는 유효한 값이라 "미배정"을 뜻하는 ZONE_NONE으로 명시 초기화 */
	for (i = 0; i < ZONE_TAG_COUNT; i++)
		c->zp->active_zone[i] = ZONE_NONE;
	c->foreground_handoff_gc_zone = ZONE_NONE;
	c->wal_rotation_old_zone = ZONE_NONE;

	c->memtable = kzalloc(sizeof(*c->memtable), GFP_KERNEL);
	if (!c->memtable) {
		ti->error = "out of memory (memtable)";
		return -ENOMEM;
	}
	ret = skiplist_init(c->memtable);
	if (ret) {
		ti->error = "failed to init memtable";
		return ret;
	}
	spin_lock_init(&c->lock);
	INIT_LIST_HEAD(&c->wal_pending);
	INIT_DELAYED_WORK(&c->wal_batch_work, wal_batch_work_fn);
	INIT_LIST_HEAD(&c->pending_flush_bios);
	INIT_DELAYED_WORK(&c->pending_flush_work, pending_flush_work_fn);
	INIT_LIST_HEAD(&c->pending_write_bios);
	INIT_DELAYED_WORK(&c->pending_write_work, pending_write_work_fn);
	INIT_DELAYED_WORK(&c->wal_rotate_work, wal_rotate_work_fn);
	atomic_set(&c->foreground_writes, 0);

	/* zone 스캔으로 크래시/재로드 이전 상태 복원(zone_tag[]/wp[] + WAL
	 * checkpoint 위치 파악 후 replay + 살아있는 SSTable들 색인 재구성).
	 * 완전히 새 디바이스면 모든 zone의 wp가 0이라 사실상 no-op. */
	{
		struct recovery_scan_ctx rctx = {
			.c = c,
			.nr_wal_zones = 0,
			.nr_sstable_zones = 0
		};
		unsigned int i2;

		rctx.wal_zones = kcalloc(c->zp->nr_zones, sizeof(unsigned int), GFP_KERNEL);
		if (!rctx.wal_zones) {
			ti->error = "out of memory (wal_zones scratch)";
			return -ENOMEM;
		}
		rctx.sstable_zones = kcalloc(c->zp->nr_zones, sizeof(unsigned int), GFP_KERNEL);
		if (!rctx.sstable_zones) {
			ti->error = "out of memory (sstable_zones scratch)";
			kfree(rctx.wal_zones);
			return -ENOMEM;
		}

		ret = blkdev_report_zones(c->dev->bdev, 0, c->zp->nr_zones, recovery_zone_cb, &rctx);
		if (ret < 0)
			DMERR("zone report failed during recovery scan: %d", ret);

		replay_wal_zones(c, rctx.wal_zones, rctx.nr_wal_zones);
		for (i2 = 0; i2 < rctx.nr_sstable_zones; i2++) {
			unsigned int zone_id = rctx.sstable_zones[i2];

			scan_sstable_zone(c, zone_id, c->zp->wp[zone_id]);
		}

		kfree(rctx.wal_zones);
		kfree(rctx.sstable_zones);
	}

	INIT_WORK(&c->compaction_work, compaction_work_fn);
	INIT_WORK(&c->gc_work, gc_work_fn);
	INIT_WORK(&c->wal_reclaim_work, wal_reclaim_work_fn);
	init_waitqueue_head(&c->flush_waitq);

	ti->private = c;
	ti->num_flush_bios = 1;
	/* 하위 ZNS가 write-through여도 DM 자체의 메모리 WAL은 volatile하다.
	 * 상위 커널이 FLUSH/FUA를 제거하지 않도록 캐시 지원을 명시한다. */
	ti->flush_supported = true;
	ti->num_discard_bios = 1;
	ti->discards_supported = true;
	/* 읽기 bio가 pin한 zone을 완료 시 unpin하려면 per-bio에 그 zone을 실어둔다(zns_read_pin). */
	ti->per_io_data_size = sizeof(struct zns_read_pin);
	/* 일반 읽기는 블록 경계에서, 정렬 쓰기는 append 한도에서 분할한다. discard는 범위 요청으로 받는다. */
	ti->max_io_len = 0;

	/* 복구한 checkpoint 이전 WAL은 추가 쓰기가 없어도 회수하도록 즉시 작업을 요청한다. */
	if (c->wal_durable_split_gen > 0)
		queue_work(zns_wal_reclaim_wq, &c->wal_reclaim_work);

	DMINFO("ctr: target attached on top of '%s'", argv[0]);
	return 0;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;

	spin_lock_irq(&c->lock);
	c->stopping = true;
	spin_unlock_irq(&c->lock);
	wake_up_all(&c->flush_waitq);
	mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
	flush_delayed_work(&c->wal_batch_work);
	/* 조기 ACK한 ctx는 DM core의 원본 bio drain에 포함되지 않는다.
	 * 한 번의 flush_work로 끝내지 말고 재예약한 모든 그룹까지 drain. */
	wait_event(c->flush_waitq, !READ_ONCE(c->wal_batch_busy) &&
		   !READ_ONCE(c->wal_pending_count) &&
		   !READ_ONCE(c->wal_buffered_count));
	cancel_delayed_work_sync(&c->wal_batch_work);
	mod_delayed_work(zns_wq, &c->pending_write_work, 0);
	flush_delayed_work(&c->pending_write_work);
	flush_delayed_work(&c->pending_flush_work);

	/* worker가 SSTable/checkpoint 비동기 체인을 시작하게 한 뒤, 마지막 callback의
	 * flush_chain_end까지 기다려 c와 frozen memtable의 수명을 보장한다. */
	flush_workqueue(zns_flush_wq);
	wait_event(c->flush_waitq, READ_ONCE(c->wal_ckpt_inflight) == 0);

	/* 아직 큐잉/실행 중인 compaction이 있으면 완전히 끝날 때까지 기다린다(안 그러면 아래에서 c를 해제한 뒤 use-after-free) */
	cancel_work_sync(&c->compaction_work);
	cancel_work_sync(&c->gc_work);
	cancel_delayed_work_sync(&c->wal_rotate_work);
	cancel_work_sync(&c->wal_reclaim_work);

	dm_put_device(ti, c->dev);
	if (c->frozen_memtable) {
		skiplist_destroy(c->frozen_memtable);
		kfree(c->frozen_memtable);
	}
	skiplist_destroy(c->memtable);
	kfree(c->memtable);
	kfree(c->sstables);
	kfree(c->zp->wp);
	kfree(c->zp->zone_tag);
	kfree(c->zp->wal_gen);
	kfree(c->zp->invalid_count);
	kfree(c->zp->sstable_live_count);
	if (c->zp->dispatch_waiters) {
		/* 정상적이면 dtr() 시점엔 대기열이 비어있어야 하지만, 혹시 남아있어도 메모리 누수는 막는다 */
		unsigned int zi;
		struct dispatch_waiter *w, *tmp;

		for (zi = 0; zi < c->zp->nr_zones; zi++) {
			list_for_each_entry_safe(w, tmp, &c->zp->dispatch_waiters[zi], link) {
				list_del(&w->link);
				kfree(w);
			}
		}
	}
	kfree(c->zp->dispatch_wp);
	kfree(c->zp->dispatch_waiters);
	kfree(c->zp->append_ready);
	kfree(c->zp->append_waiters);
	kfree(c->zp->inflight_reads);
	kfree(c->zp);
	kfree(c);
	DMINFO("dtr: target detached");
}

static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;
	struct zns_read_pin *pin = dm_per_bio_data(bio, sizeof(*pin));

	sector_t lba;
	sector_t nr = bio_sectors(bio);
	sector_t block_lba;
	sector_t offset_in_block;

	pin->zone = -1; /* 읽기 pin 없음이 기본 — READ 경로에서 필요 시에만 설정 */
	if (READ_ONCE(c->metadata_failed)) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* 순수 flush 요청(nr=0)은 특정 LBA와 무관하므로 매핑 로직을 타면 안 됨.
	 * bi_sector에 남은 임의값을 lba로 오인해 엉뚱한 매핑을 덮어쓰게 된다. */
	if (nr == 0) {
		bool wal_busy;
		struct pending_flush_bio *pending;

		spin_lock_irq(&c->lock);
		wal_busy = c->wal_batch_busy || !list_empty(&c->wal_pending) ||
			atomic_read(&c->foreground_writes) > 0;
		spin_unlock_irq(&c->lock);
		if (wal_busy) {
			pending = kmalloc(sizeof(*pending), GFP_NOWAIT);
			if (!pending) {
				mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
				return DM_MAPIO_REQUEUE;
			}
			pending->bio = bio;
			INIT_LIST_HEAD(&pending->link);
			spin_lock_irq(&c->lock);
			list_add_tail(&pending->link, &c->pending_flush_bios);
			spin_unlock_irq(&c->lock);
			mod_delayed_work(zns_wal_wq, &c->wal_batch_work, 0);
			mod_delayed_work(zns_wq, &c->pending_flush_work,
					 msecs_to_jiffies(20));
			return DM_MAPIO_SUBMITTED;
		}
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}

	lba = bio->bi_iter.bi_sector;

	/* 매핑 키는 항상 블록(BLOCK_SECTORS) 정렬된 lba — 커널이 블록 정렬 안 된 위치(예: ext4 슈퍼블록 프로브, sector 2부터 2섹터)로 읽을 수 있어서. */
	block_lba = (lba / BLOCK_SECTORS) * BLOCK_SECTORS;
	offset_in_block = lba - block_lba;

	/* extent 쓰기는 실제 Zone Append 한도 안에서만 허용한다. 읽기/부분
	 * 쓰기는 기존 4KiB 분할을 유지한다. zone 경계는 allocator가 보장한다. */
	if (bio_op(bio) == REQ_OP_WRITE && !offset_in_block &&
	    !(nr % BLOCK_SECTORS) && data_append_max_blocks > 1) {
		sector_t sectors = append_extent_sectors(nr, data_append_max_blocks,
			queue_max_zone_append_sectors(bdev_get_queue(c->dev->bdev)),
			queue_max_sectors(bdev_get_queue(c->dev->bdev)));
		if (nr > sectors) {
			dm_accept_partial_bio(bio, sectors);
			nr = sectors;
		}
	} else if (bio_op(bio) != REQ_OP_DISCARD &&
	    nr > BLOCK_SECTORS - offset_in_block) {
		dm_accept_partial_bio(bio, BLOCK_SECTORS - offset_in_block);
		nr = BLOCK_SECTORS - offset_in_block;
	}

	switch (bio_op(bio)) {
	case REQ_OP_WRITE: {
		int ret;
		struct pending_write_bio *pending;
		bool backlog;

		if (metrics_enabled)
			atomic64_add((u64)nr * 512, &c->metric_logical_write_bytes);

		/* 매핑/WAL/GC의 최소 단위는 4KB다. 부분 write를 그대로
		 * append하면 read-modify-write 없이 나머지 섹터를 잃게 되므로
		 * 조용히 잘못된 mapping을 만드는 대신 명시적으로 거절한다. */
		if (offset_in_block || nr % BLOCK_SECTORS) {
			bio->bi_status = BLK_STS_NOTSUPP;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		/* 먼저 보류된 bio가 있으면 새 write가 회수 직후 공간을 가로채지
		 * 않도록 동일 FIFO 뒤에 세운다. */
		spin_lock_irq(&c->lock);
		backlog = !list_empty(&c->pending_write_bios);
		spin_unlock_irq(&c->lock);

		ret = backlog ? -EAGAIN : start_foreground_write(c, bio, block_lba, nr);
		if (ret != -EAGAIN)
			return DM_MAPIO_SUBMITTED;

		pending = kmalloc(sizeof(*pending), GFP_NOIO);
		if (!pending) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		pending->bio = bio;
		pending->lba = block_lba;
		pending->nr = nr;
		pending->metric_queued_ns = metrics_enabled ? ktime_get_ns() : 0;
		INIT_LIST_HEAD(&pending->link);
		spin_lock_irq(&c->lock);
		list_add_tail(&pending->link, &c->pending_write_bios);
		spin_unlock_irq(&c->lock);
		mod_delayed_work(zns_wq, &c->pending_write_work,
				 msecs_to_jiffies(20));
		return DM_MAPIO_SUBMITTED;
	}
	case REQ_OP_DISCARD: {
		struct zns_io_ctx *ctx;

		/* 정렬된 discard 범위 전체를 WAL 레코드 하나로 영속화한다. */
		if (offset_in_block || nr % BLOCK_SECTORS) {
			bio->bi_status = BLK_STS_NOTSUPP;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		ctx = kzalloc(sizeof(*ctx), GFP_NOIO);
		if (!ctx) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		spin_lock_irq(&c->lock);
		if (c->wal_rotation_pending || c->gc_foreground_pause) {
			spin_unlock_irq(&c->lock);
			kfree(ctx);
			return DM_MAPIO_REQUEUE;
		}
		atomic_inc(&c->foreground_writes);
		spin_unlock_irq(&c->lock);
		ctx->c = c;
		ctx->orig_bio = bio;
		ctx->lba = block_lba;
		ctx->phys = MAPPING_TOMBSTONE;
		ctx->is_discard = true;
		ctx->discard_blocks = nr / BLOCK_SECTORS;
		ctx->reserved_nr = 0;
		submit_wal_async(ctx);
		return DM_MAPIO_SUBMITTED;
	}
	case REQ_OP_READ: {
		sector_t phys;
		u64 view_epoch;
		int found;
		unsigned int nr_sst, actual_nr, nmatch, i;
		struct sstable_info *candidates;
		struct sstable_read_ctx *rctx;

		if (metrics_enabled)
			atomic64_add((u64)nr * 512, &c->metric_logical_read_bytes);

		spin_lock_irq(&c->lock);
		found = mapping_get(c, block_lba, &phys);
		if (found && phys != MAPPING_TOMBSTONE) {
			/* 매핑 조회와 같은 락에서 data zone을 pin해 읽기 완료 전 reset을 막는다. */
			pin->zone = zone_of(c->zp, phys);
			zone_read_get(c->zp, pin->zone);
		}
		nr_sst = c->nr_sstables;
		view_epoch = c->read_view_epoch;
		spin_unlock_irq(&c->lock);

		if (found && phys == MAPPING_TOMBSTONE) {
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		if (found) {
			/* 메모리 매핑이 있으면 SSTable 탐색 없이 해당 블록으로 remap한다. */
			bio->bi_iter.bi_sector = phys + offset_in_block;
			break;
		}

		if (nr_sst == 0) {
			/* 한 번도 안 쓴 블록 — 표준 thin-provisioning 관례대로 zero-fill */
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		/* SSTable 후보 스냅샷 — c->sstables는 krealloc으로 커질 수 있어 주소가 바뀔 수 있으므로, 복사는 반드시 락 안에서. */
		candidates = kmalloc_array(nr_sst, sizeof(*candidates), GFP_NOIO);
		if (!candidates) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		spin_lock_irq(&c->lock);
		actual_nr = min(nr_sst, c->nr_sstables);
		/* 스냅샷이 할당 크기로 잘렸으면 이전 epoch를 유지해 종료 경로에서 재탐색하게 한다. */
		if (actual_nr == c->nr_sstables)
			view_epoch = c->read_view_epoch;
		memcpy(candidates, c->sstables, actual_nr * sizeof(*candidates));
		/* 스냅샷과 같은 락에서 후보 SSTable zone을 pin한다. 탐색 종료 시 sstable_read_finish에서 해제한다. */
		nmatch = 0;
		for (i = 0; i < actual_nr; i++) {
			if (block_lba >= candidates[i].min_lba && block_lba <= candidates[i].max_lba) {
				candidates[nmatch] = candidates[i];
				zone_read_get(c->zp, zone_of(c->zp, candidates[nmatch].phys));
				nmatch++;
			}
		}
		spin_unlock_irq(&c->lock);

		rctx = kzalloc(sizeof(*rctx), GFP_NOIO);
		if (!rctx) {
			sstable_snapshot_unpin(c, candidates, nmatch);
			kfree(candidates);
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		/* on-disk binary search probe가 재사용할 512B 섹터 버퍼 */
		rctx->sec_buf = kzalloc(512, GFP_NOIO);
		if (!rctx->sec_buf) {
			kfree(rctx);
			sstable_snapshot_unpin(c, candidates, nmatch);
			kfree(candidates);
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		rctx->c = c;
		rctx->orig_bio = bio;
		rctx->lba = block_lba;
		rctx->offset_in_block = offset_in_block;
		rctx->candidates = candidates;
		rctx->nr_candidates = nmatch;
		rctx->view_epoch = view_epoch;
		rctx->idx = 0;
		sstable_read_next_candidate(rctx);

		/* SSTable 조회는 비동기 — sstable_read_finish가 원본 bio를 마무리 */
		return DM_MAPIO_SUBMITTED;
	}
	}

	/* 위에서 처리하지 않은 요청은 하위 장치로 전달한다. */
	bio_set_dev(bio, c->dev->bdev);
	return DM_MAPIO_REMAPPED;
}

static void zns_base_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flas, char *result, unsigned int maxlen)
{
	struct zns_base_c *c = ti->private;
	unsigned int i;
	unsigned int sz = 0;

	if (type == STATUSTYPE_INFO) {
		DMEMIT("append_max_blocks=%u wal_group_pages=%u adaptive_gc=%u probe_cache=%u probe_ios=%lld probe_reuses=%lld ",
		       data_append_max_blocks, wal_group_pages, 1U, 1U,
		       atomic64_read(&c->sstable_probe_ios), atomic64_read(&c->sstable_probe_reuses));
		DMEMIT("wal_writeback=%u wal_buffered=%u wal_early_acks=%llu wal_early_commits=%llu wal_early_errors=%llu ",
		       1U, READ_ONCE(c->wal_buffered_count),
		       READ_ONCE(c->wal_buffered_acks), READ_ONCE(c->wal_buffered_commits),
		       READ_ONCE(c->wal_buffered_errors));
		if (metrics_enabled) {
			u64 bytes;

			if (wal_timing_enabled) {
				DMEMIT("wal_diag=1 wal_delay_ms=%u wal_max_records=%u ",
				       wal_batch_delay_ms, wal_batch_max_records);
				for (i = 0; i < WD_COUNT; i++)
					DMEMIT("wal_diag_%s=%lld ", wal_diag_names[i],
					       atomic64_read(&c->wal_diag[i]));
			}
			spin_lock_irq(&c->lock);
			bytes = (c->memtable ? c->memtable->allocated_bytes : 0) +
				(c->frozen_memtable ? c->frozen_memtable->allocated_bytes : 0) +
				(u64)c->sstables_cap * sizeof(*c->sstables);
			c->metric_mapping_peak_bytes = max(c->metric_mapping_peak_bytes, bytes);
			DMEMIT("metrics=1 mapping_resident_bytes=%llu mapping_sampled_peak_bytes=%llu ",
			       bytes, c->metric_mapping_peak_bytes);
			DMEMIT("free_zones=%u memtable_entries=%u frozen_entries=%u sstable_count=%u ",
			       gc_count_free_zones(c->zp), c->memtable ? c->memtable->count : 0,
			       c->frozen_memtable ? c->frozen_memtable->count : 0, c->nr_sstables);
			spin_unlock_irq(&c->lock);
			DMEMIT("logical_write_bytes=%lld user_write_bytes=%lld gc_write_bytes=%lld wal_write_bytes=%lld sstable_write_bytes=%lld gc_runs=%lld gc_ns=%lld gc_reclaims=%lld gc_moved_bytes=%lld compaction_runs=%lld compaction_ns=%lld compaction_input_bytes=%lld compaction_output_bytes=%lld ",
			       atomic64_read(&c->metric_logical_write_bytes),
			       atomic64_read(&c->metric_write_bytes[ZONE_TAG_USER_DATA]),
			       atomic64_read(&c->metric_write_bytes[ZONE_TAG_GC_DATA]),
			       atomic64_read(&c->metric_write_bytes[ZONE_TAG_WAL]),
			       atomic64_read(&c->metric_write_bytes[ZONE_TAG_SSTABLE]),
			       atomic64_read(&c->metric_gc_runs), atomic64_read(&c->metric_gc_ns),
			       atomic64_read(&c->metric_gc_reclaims), atomic64_read(&c->metric_gc_moved_bytes),
			       atomic64_read(&c->metric_compaction_runs), atomic64_read(&c->metric_compaction_ns),
			       atomic64_read(&c->metric_compaction_input_bytes), atomic64_read(&c->metric_compaction_output_bytes));
			DMEMIT("logical_read_bytes=%lld ", atomic64_read(&c->metric_logical_read_bytes));
			DMEMIT("gc_scratch_requested_bytes=%lld compaction_scratch_requested_bytes=%lld foreground_wait_ns=%lld foreground_wait_count=%lld ",
			       atomic64_read(&c->metric_gc_scratch_bytes),
			       atomic64_read(&c->metric_compaction_scratch_bytes),
			       atomic64_read(&c->metric_foreground_wait_ns),
			       atomic64_read(&c->metric_foreground_wait_count));
			DMEMIT("flush_runs=%lld flush_ns=%lld flush_write_bytes=%lld ",
			       atomic64_read(&c->metric_flush_runs), atomic64_read(&c->metric_flush_ns),
			       atomic64_read(&c->metric_flush_write_bytes));
		}
		DMEMIT("zones=%u zone_sectors=%llu active[user_data]=%u wp:",
		       c->zp->nr_zones, (unsigned long long)c->zp->zone_sectors,
		       c->zp->active_zone[ZONE_TAG_USER_DATA]);
		for (i = 0; i < c->zp->nr_zones; i++)
			DMEMIT(" %llu/%llu", (unsigned long long)c->zp->wp[i], (unsigned long long)c->zp->zone_sectors);
	} else {
		DMEMIT("%s", c->dev->name);
	}
}

/* DM core가 하위 장치의 기본 queue limit을 수집하게 한다. 상위에 노출할
 * zoned/discard 속성은 이후 io_hints에서 이 변환 타깃에 맞게 덮어쓴다. */
static int zns_base_iterate_devices(struct dm_target *ti,
				    iterate_devices_callout_fn fn, void *data)
{
	struct zns_base_c *c = ti->private;

	return fn(ti, c->dev, 0, ti->len, data);
}

/* 읽기가 보유한 zone pin을 해제한다. pin이 없는 bio는 zone=-1이므로 건너뛴다. */
static int zns_base_end_io(struct dm_target *ti, struct bio *bio, blk_status_t *error)
{
	struct zns_base_c *c = ti->private;
	struct zns_read_pin *pin = dm_per_bio_data(bio, sizeof(*pin));

	if (pin->zone >= 0) {
		zone_read_put(c->zp, pin->zone);
		pin->zone = -1;
	}
	return DM_ENDIO_DONE;
}

/* 이 타깃은 discard를 하위 ZNS 장치로 전달하지 않고 4KiB 논리 매핑의
 * tombstone으로 직접 처리한다. 따라서 하위 장치의 discard capability와
 * 무관하게 상위 queue에 타깃의 처리 단위와 한계를 명시해야 한다. */
static void zns_base_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	(void)ti;
	/* 하위 ZNS의 queue limit은 수집하되 상위 DM은 random-write 가능한
	 * conventional 장치로 노출한다. */
	limits->zoned = BLK_ZONED_NONE;
	limits->chunk_sectors = 0;
	limits->discard_granularity = BLOCK_SECTORS << SECTOR_SHIFT;
	limits->discard_alignment = 0;
	limits->max_hw_discard_sectors = UINT_MAX & ~(BLOCK_SECTORS - 1);
	limits->max_discard_sectors = UINT_MAX & ~(BLOCK_SECTORS - 1);
}

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 1, 0},
	.features        = DM_TARGET_ZONED_HM,
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	.status          = zns_base_status,
	.end_io          = zns_base_end_io,
	.io_hints        = zns_base_io_hints,
	.iterate_devices = zns_base_iterate_devices,
};

static int __init zns_base_init(void)
{
	int ret;

	zns_bio_wq = alloc_workqueue("dm_zns_base_bio",
				    WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!zns_bio_wq)
		return -ENOMEM;

	/* 메모리 회수 중에도 bio 제출과 pending 재시도가 진행되도록 WQ_MEM_RECLAIM을 사용한다.
	 * WQ_UNBOUND와 실행 수 제한은 실행 정책이며, zone 제출 순서는 별도 dispatch gate가 보장한다. */
	zns_wq = alloc_workqueue("dm_zns_base", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!zns_wq) {
		DMERR("failed to allocate workqueue");
		destroy_workqueue(zns_bio_wq);
		return -ENOMEM;
	}
	zns_wal_wq = alloc_workqueue("dm_zns_base_wal",
				     WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!zns_wal_wq) {
		destroy_workqueue(zns_wq);
		destroy_workqueue(zns_bio_wq);
		return -ENOMEM;
	}

	/* compaction 전용 큐다. max_active=1로 같은 CPU 풀에서 실행 수를 제한한다. */
	zns_compaction_wq = alloc_workqueue("dm_zns_base_compaction", 0, 1);
	if (!zns_compaction_wq) {
		DMERR("failed to allocate compaction workqueue");
		destroy_workqueue(zns_wal_wq);
		destroy_workqueue(zns_wq);
		destroy_workqueue(zns_bio_wq);
		return -ENOMEM;
	}

	/* GC 전용 큐다. 타깃별 gc_active 상태와 함께 중복 cycle을 제어한다. */
	zns_gc_wq = alloc_workqueue("dm_zns_base_gc", 0, 1);
	if (!zns_gc_wq) {
		DMERR("failed to allocate gc workqueue");
		destroy_workqueue(zns_wal_wq);
		destroy_workqueue(zns_compaction_wq);
		destroy_workqueue(zns_wq);
		destroy_workqueue(zns_bio_wq);
		return -ENOMEM;
	}

	/* WAL 회수 전용 큐다. foreground·GC의 실행 슬롯과 분리한다. */
	zns_wal_reclaim_wq = alloc_workqueue("dm_zns_base_wal_reclaim", 0, 1);
	if (!zns_wal_reclaim_wq) {
		DMERR("failed to allocate wal reclaim workqueue");
		destroy_workqueue(zns_wal_wq);
		destroy_workqueue(zns_gc_wq);
		destroy_workqueue(zns_compaction_wq);
		destroy_workqueue(zns_wq);
		destroy_workqueue(zns_bio_wq);
		return -ENOMEM;
	}

	zns_flush_wq = alloc_workqueue("dm_zns_base_flush",
				       WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!zns_flush_wq) {
		DMERR("failed to allocate flush workqueue");
		destroy_workqueue(zns_wal_wq);
		destroy_workqueue(zns_wal_reclaim_wq);
		destroy_workqueue(zns_gc_wq);
		destroy_workqueue(zns_compaction_wq);
		destroy_workqueue(zns_wq);
		destroy_workqueue(zns_bio_wq);
		return -ENOMEM;
	}

	ret = dm_register_target(&zns_base_target);
	if (ret < 0) {
		DMERR("target registration failed: %d", ret);
		destroy_workqueue(zns_wal_wq);
		destroy_workqueue(zns_flush_wq);
		destroy_workqueue(zns_wal_reclaim_wq);
		destroy_workqueue(zns_gc_wq);
		destroy_workqueue(zns_compaction_wq);
		destroy_workqueue(zns_wq);
		destroy_workqueue(zns_bio_wq);
	} else {
		DMINFO("target registered");
	}
	return ret;
}

static void __exit zns_base_exit(void)
{
	dm_unregister_target(&zns_base_target);
	destroy_workqueue(zns_wal_wq);
	destroy_workqueue(zns_flush_wq);
	destroy_workqueue(zns_wal_reclaim_wq);
	destroy_workqueue(zns_gc_wq);
	destroy_workqueue(zns_compaction_wq);
	destroy_workqueue(zns_wq);
	destroy_workqueue(zns_bio_wq);
	DMINFO("target unregistered");
}

module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS base dm target (zoned-aware pass-through scaffold)");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
