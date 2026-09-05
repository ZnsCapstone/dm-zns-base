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

#include "skiplist.h"

#define DM_MSG_PREFIX "zns-base"

/* bio 완료 콜백에서 다음 bio를 이어붙일 땐 반드시 이 워크큐로 미뤄 process
 * context에서 제출한다 — submit_bio()는 논블로킹이 아니라 atomic context에서
 * 부르면 죽을 수 있음(admission control이 내부에서 schedule()을 부를 수 있음). */
static struct workqueue_struct *zns_wq;

/* compaction 전용 워크큐 — bio 제출과 큐를 분리해 서로 지연을 안 주게 함 */
static struct workqueue_struct *zns_compaction_wq;

/* GC 전용 워크큐 — compaction과 별개로 둬서 둘이 서로를 못 기다리게 함
 * (같은 워크큐를 썼으면 max_active=1이라 GC가 compaction 끝날 때까지, 혹은
 * 그 반대로 기다려야 했을 것). */
static struct workqueue_struct *zns_gc_wq;

/* WAL zone 회수 전용 워크큐 — 체크포인트가 durable해진 뒤 온전히 flush된 WAL
 * zone을 reset한다. GC/compaction과 별개(서로 안 막게). */
static struct workqueue_struct *zns_wal_reclaim_wq;

/* Frozen memtable 직렬화는 큰 버퍼를 필요로 하므로 bio 완료(atomic) 경로가
 * 아니라 이 process-context 워크큐에서 수행한다. */
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

/* 체인의 "다음 bio 제출"은 전부 이 함수를 거친다 — 호출 시점이 atomic
 * context일 수도, process context일 수도 있지만(같은 함수가 양쪽에서 다
 * 불림) 어느 쪽이든 안전하도록 항상 워크큐로 넘긴다. */
static void submit_bio_deferred(struct bio *bio)
{
	struct deferred_bio_work *dw = kmalloc(sizeof(*dw), GFP_ATOMIC);

	if (!dw) {
		/* 이 정도로 메모리가 없으면 시스템이 이미 위험한 상태 —
		 * 그래도 완전히 누락시키는 것보다 직접 제출을 시도하는 게 낫다 */
		submit_bio(bio);
		return;
	}
	INIT_WORK(&dw->work, deferred_bio_submit_fn);
	dw->bio = bio;
	queue_work(zns_wq, &dw->work);
}

enum zone_tag {
    ZONE_TAG_FREE = 0,   // 아직 아무도 안 쓰는 zone
    ZONE_TAG_USER_DATA,
    ZONE_TAG_GC_DATA,
    ZONE_TAG_WAL,
    ZONE_TAG_SSTABLE,
    ZONE_TAG_COUNT,
};

#define ZONE_NONE UINT_MAX
#define BLOCK_SECTORS 8   // 매핑 단위 = 4KB = 512B 섹터 8개

/* memtable이 이 개수를 넘으면 SSTable로 flush. 기본값이 큰 이유: 일반
 * 테스트가 우연히 flush를 안 건드리게 하려고 — 검증 시 insmod
 * flush_threshold=50 등으로 낮춰서 확인. */
static unsigned int flush_threshold = 1000000;
module_param(flush_threshold, uint, 0444);
MODULE_PARM_DESC(flush_threshold, "memtable entry count that triggers an SSTable flush");

/* 살아있는 SSTable 개수가 이 값에 도달하면 compaction 트리거 — read
 * amplification 기준(공간 기준 아님, FEMU 용량은 사실상 무제한). */
static unsigned int compaction_k = 4;
module_param(compaction_k, uint, 0444);
MODULE_PARM_DESC(compaction_k, "number of oldest SSTables merged per compaction run");

/* 여유(FREE) zone 개수가 이 값 이하로 떨어지면 GC 트리거 — USER_DATA zone을
 * 소비할 때마다 확인(zns_base_map의 WRITE 분기 끝). */
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

/* free zone 중 마지막 이만큼은 GC_DATA 태그에만 내준다 — GC 자신도 재배치할
 * zone이 필요한데 USER_DATA/WAL이 여유 zone을 전부 먹으면 GC가 회수를 못 해
 * 자기순환 데드락에 빠진다. 이 예비분으로 GC는 항상 재배치할 곳을 확보. */
static unsigned int gc_reserved_zones = 2;
module_param(gc_reserved_zones, uint, 0444);
MODULE_PARM_DESC(gc_reserved_zones, "free zones reserved exclusively for GC relocation");

static unsigned int wal_batch_max_records = 127;
module_param(wal_batch_max_records, uint, 0444);
MODULE_PARM_DESC(wal_batch_max_records, "maximum PUT records per 4KB WAL page");

static unsigned int wal_batch_delay_ms = 2;
module_param(wal_batch_delay_ms, uint, 0444);
MODULE_PARM_DESC(wal_batch_delay_ms, "maximum foreground WAL group-commit delay in ms");

/* 회귀 테스트에서 WAL 공간 부족 경로를 결정적으로 재현하기 위한 fault
 * injection. 0(기본값)이면 실제 동작에는 아무 영향이 없다. */
static unsigned int wal_batch_alloc_failures;
module_param(wal_batch_alloc_failures, uint, 0444);
MODULE_PARM_DESC(wal_batch_alloc_failures, "number of foreground WAL allocations to fail for testing");

// zone pool
struct zone_pool {
	sector_t zone_sectors;
	unsigned int nr_zones;
	enum zone_tag *zone_tag; 					// zone_tag[zone_id] — 이 zone이 지금 뭘로 쓰이는지
	sector_t *wp; 								// zone_id별 "할당된"(아직 실제로 안 나갔을 수도 있는) 섹터 수
	sector_t *dispatch_wp; 						// zone_id별 "실제로 디바이스에 나간" 섹터 수
	struct list_head *dispatch_waiters; 		// zone_id별: 아직 자기 차례가 안 된 쓰기 대기열
	unsigned int *invalid_count; 				// zone_id별 무효(죽은) 섹터 수 — GC(M3) victim 선정 근거
	unsigned int *sstable_live_count; 			// zone_id별 그 zone에 저장된 살아있는 SSTable 개수 — 0이 되면 compaction이 zone을 회수 가능
	unsigned int active_zone[ZONE_TAG_COUNT]; 	// 태그별 현재 활성 zone
	u64 *wal_gen; 								// WAL zone에 한해서만 의미 있는 배정 순번(generation) — replay 순서 판정용
	u64 wal_next_gen; 							// 다음 WAL zone에 부여할 generation (단조 증가, c->lock 하에 증가)
	atomic_t *inflight_reads;  // zone_id별 진행 중인 읽기 수 — 회수(reset) 전 drain 대기용
	wait_queue_head_t reclaim_waitq;  // inflight_reads가 0이 되길 기다리는 회수 대기 큐
	bool *append_ready;               // zone header가 durable해 append를 받을 수 있음
	struct list_head *append_waiters; // header 완료 전 대기하는 zone append bio
};

/* zone_dispatch_write() 대기열에 들어가는 항목 하나 — "phys부터 nr섹터를
 * 쓰려는 쓰기가 있는데, 아직 zone의 dispatch_wp가 거기까지 안 왔다"는 뜻.
 * dispatch_wp가 phys에 도달하면 fire(arg)가 호출된다. */
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

	sector_t 		nr_sectors;
	struct zone_pool *zp;
	struct skiplist *memtable;  	// LBA -> phys 매핑 (M1의 map[] flat array를 대체)
	u64              next_seq_no;   // 다음 flush에 붙일 SSTable 세대 번호

	struct sstable_info *sstables;  // 살아있는 SSTable 색인 (append-only, krealloc으로 증가)
	unsigned int nr_sstables;
	unsigned int sstables_cap;      // sstables 배열의 현재 용량

	struct work_struct compaction_work;  // compaction_wq에 큐잉되는 백그라운드 작업
	struct work_struct gc_work;          // gc_wq에 큐잉되는 백그라운드 작업
	struct work_struct wal_reclaim_work; // wal_reclaim_wq에 큐잉되는 백그라운드 작업
	struct delayed_work wal_batch_work;
	struct list_head wal_pending;
	unsigned int wal_pending_count;
	bool wal_batch_busy;
	bool stopping;
	atomic_t foreground_writes;

	/* WAL zone 회수 판정용 — 자세한 건 flush_chain_end/wal_reclaim_work_fn 참고.
	 * split_gen이 durable_split_gen보다 작은(= 온전히 flush된) WAL zone만 회수.
	 * out-of-order 완료 대비: 발행된 체크포인트가 전부 durable(inflight==0)일
	 * 때만 durable 지점을 highest_issued까지 전진시킨다. */
	int wal_ckpt_inflight;              // 아직 durable 안 된 체크포인트 개수
	u64 wal_highest_issued_split_gen;   // 지금까지 발행된 체크포인트 중 최대 split_gen
	u64 wal_durable_split_gen;          // 이 값보다 작은 gen의 WAL zone은 회수 가능
	wait_queue_head_t flush_waitq;      // dtr에서 비동기 flush chain drain 대기
	bool gc_active;                     // GC latest-map 구축/이주 중 memtable swap 금지
	unsigned int gc_no_progress;        // 연속으로 공간을 못 만든 GC cycle 수

	spinlock_t 		lock;
};

struct memtable_flush_work {
	struct work_struct work;
	struct zns_base_c *c;
	struct skiplist *old_memtable;
	u64 seq_no;
	u64 split_gen;
	sector_t split_off;
};

// SSTable - 오래된 걸 압축해서 Zone에 색인포함으로 보관
#define SSTABLE_MAGIC 0x53535442U  /* "SSTB" */

/* flush 시 정렬 순서로 이 형태 그대로 기록 — 고정 16B라 offset=i*16으로
 * binary search 가능. ★ 온디스크 포맷이라 필드는 __le(리틀엔디안 고정), 접근 시
 * cpu_to_le64/le64_to_cpu로 변환한다(이식성 + sparse 검증 가능). */
struct sstable_record {
	__le64 lba;
	__le64 phys;
};

/* SSTable 한 세대의 첫 섹터에 오는 헤더. min/max_lba는 조회 필터용.
 * ★ 온디스크 포맷 — __le 고정(위 sstable_record 참고). */
struct sstable_header {
	__le32 magic;
	__le32 reserved;
	__le64 seq_no;
	__le64 record_count;
	__le64 min_lba;
	__le64 max_lba;
};

/* 살아있는 SSTable 하나를 메모리에서 빠르게 찾기 위한 색인 — 헤더 내용을
 * 그대로 캐싱한 것. phys는 그 SSTable 자신의 헤더가 시작하는 절대 섹터. */
struct sstable_info {
	sector_t phys;
	u64 seq_no;
	u64 record_count;
	u64 min_lba;
	u64 max_lba;
};

// WAL - 복구용
#define WAL_REC_PUT        1
#define WAL_REC_CHECKPOINT 2
#define WAL_REC_GC_PUT     3
#define WAL_PAGE_MAGIC     0x57414C50U /* "WALP" */
#define WAL_PAGE_VERSION   1
#define WAL_PAGE_SECTORS   (PAGE_SIZE / 512)

/* 고정 크기(512/4096에 나머지 없이 나눠떨어짐). 체크포인트는 스왑 시점의
 * WAL 스트림 위치를 (split_gen, split_off) = (활성 WAL zone의 generation,
 * 그 zone 내 다음 쓰기 오프셋)로 남긴다 — replay가 이 지점보다 앞선 PUT은
 * 이미 SSTable에 반영됐다고 보고 건너뛴다. 절대 섹터가 아니라 논리 순번을
 * 쓰는 이유는 WAL zone 회수 후 zone_id 순서 ≠ 기록 순서가 되기 때문. */
/* ★ 온디스크 포맷 — __le 고정. 접근 시 cpu_to_le/le_to_cpu로 변환. */
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
	unsigned int tag;  /* enum zone_tag */
	u64 gen;           /* WAL zone이면 그 generation, 아니면 0 */
};

 /* 비동기 콜백 체인이 단계 사이에 넘기는 상태. 쓰기·flush 두 경로가 "새 zone이면
 * 헤더부터 → 본작업" 패턴을 공유해 앞쪽 헤더 필드는 공용, 뒤쪽은 경로 전용.
 * on_headers_done이 헤더 완료 후 갈 단계를 가리킨다. */
struct zns_io_ctx {
	struct zns_base_c *c;

	/* 헤더 체인 공용 */
	struct pending_header headers[2];  /* 최대 2개(데이터/WAL/SSTable 태그 zone 중 이번에 새로 배정된 것들) */
	int nr_headers;
	int header_idx;
	void *hdr_buf;              /* submit_header_async가 할당, header_write_done이 해제 */
	void (*on_headers_done)(struct zns_io_ctx *ctx);
	struct list_head wal_link;

	/* WAL 레코드 쓰기 공용 — WRITE 경로에선 PUT, flush 경로 뒷단에선
	 * CHECKPOINT를 쓸 때 재사용 */
	sector_t wal_phys;
	struct wal_record *wal_buf;

	/* WRITE 경로 전용 */
	struct bio *orig_bio;
	u64 lba;
	sector_t phys;
	sector_t reserved_phys;      /* capacity 예약용; append 완료 전에는 실제 주소가 아님 */
	sector_t reserved_nr;        /* append 예약 완료 시 dispatch_wp 회계 전진용 */
	sector_t orig_sector;
	unsigned int orig_opf;
	bio_end_io_t *orig_end_io;
	void *orig_private;

	/* SSTable flush 경로 전용 */
	struct skiplist *old_memtable;
	void *sstable_buf;
	sector_t sstable_phys;
	sector_t sstable_nr_sectors;
	sector_t sstable_done_sectors;  /* 청크 기록 진행 오프셋(≤BIO_MAX_VECS씩) */
	u64 checkpoint_seq;
	u64 checkpoint_split_gen;   /* 스왑 시점 활성 WAL zone의 generation */
	sector_t checkpoint_split_off;  /* 그 zone 내 다음 WAL 쓰기 오프셋 */
};

/* zone이 새로 태그를 배정받을 때 섹터 0에 기록하는 헤더 — zone_tag[]/wp[]는
 * 메모리에만 있어서 크래시 시 사라지므로, 재insmod 후 복원의 유일한 단서. */
#define ZONE_HEADER_MAGIC 0x5A4E5348U  /* "ZNSH" */

/* ★ 온디스크 포맷 — __le 고정. 접근 시 cpu_to_le/le_to_cpu로 변환. 
 * zone 섹터 0에 기록되어 재적재 후 zone 용도를 복원하는 단서. */
struct zone_header {
	__le32 magic;
	__le32 tag;  /* enum zone_tag */
	__le64 gen;  /* WAL zone 배정 순번 — 회수 후 zone_id 순서≠기록순서라 replay 순서 판정용. WAL 외 0. */
};

/* FREE zone 하나를 tag로 배정(없으면 ZONE_NONE). wp=1은 섹터 0(태그 헤더) 예약분.
 * gc_ctx=false면 마지막 gc_reserved_zones개는 거절 — GC 전용 예비분(gc_ctx=true).
 * 이게 없으면 free 고갈 시 GC가 회수를 못 하는 자기순환 데드락. */
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

/* 태그 tag로 nr섹터 쓸 물리 위치를 배정(필요하면 새 zone 확보).
 * 새로 잡은 zone id는 new_zone_out으로(없으면 -1) — 호출자가 락 밖에서
 * 그 zone에 태그 헤더를 써야 하므로. */
static int zone_pool_alloc(struct zone_pool *zp, enum zone_tag tag, sector_t nr,
			    sector_t *phys_out, int *new_zone_out, bool gc_ctx)
{
	unsigned int z = zp->active_zone[tag];

	if (new_zone_out)
		*new_zone_out = -1;

	/* nr이 zone 하나 용량(zone_sectors-1, 섹터 0은 헤더)보다 크면 즉시 실패 —
	 * 안 그러면 아래 while이 free zone을 하나씩 낭비하다 -ENOSPC로 끝난다. */
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

/* 실제 ZNS zone reset 명령을 하드웨어에 동기적으로 보낸다 — process
 * context 전용(compaction_work_fn에서만 호출). 소프트웨어 상태는 이
 * 호출이 성공한 뒤 zone_pool_mark_free가 담당. */
static int zone_reset_hw(struct zns_base_c *c, unsigned int zone_id)
{
	return blkdev_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET,
				  (sector_t)zone_id * c->zp->zone_sectors,
				  c->zp->zone_sectors, GFP_KERNEL);
}

/* zone 하나를 zone_pool에서 FREE로 되돌린다
 * (소프트웨어 상태만: wp·dispatch_wp·tag·카운터 리셋). 하드웨어는 안 건드린다.
 * 실제 zone_reset_hw가 성공한 "뒤에만" 호출
 * 호출자가 c->lock 보유. */
static void zone_pool_mark_free(struct zone_pool *zp, unsigned int zone_id)
{
	/* 대기열이 안 비었으면 = 아직 안 나간 쓰기가 남은 zone을 회수하려는 것
	 * (회수 판정이 어디선가 빠진 신호). 경고만 남긴다. */
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

/* 회수 안전 참조 카운트 — 진행 중인 읽기가 그 zone을 다 읽을 때까지 GC/compaction의
 * reset을 미루기 위한 per-zone 카운터. pin(inc)은 읽기가 phys를 확정하는 지점에서
 * 반드시 c->lock 안에서 해야 회수 측의 "매핑/색인 제거 → drain → reset" 순서와
 * 맞아 새 읽기가 회수 대상 zone을 못 잡는다(자세한 근거는 report/bugfix-log.md). */
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

/* zone_id의 dispatch_wp가 phys에 도달하면 fire(arg) 호출, 아니면 대기열에 넣어뒀다가 앞선 쓰기가 dispatch될 때 자동 방출
 * — "배정 순서"(wp[])와 실제 디바이스 발행 순서가 달라 zone 순차쓰기가 깨지는 걸 막음.
 * - 락은 이 함수가 잡음. fire는 락 안에서 불리니 sleep 금지.
 * - 반환 0(발행 또는 큐잉), -ENOMEM이면 호출자가 arg 실패 처리. */
static int zone_dispatch_gate(struct zns_base_c *c, sector_t phys, sector_t nr,
				void (*fire)(void *arg), void *arg)
{
	unsigned int zone_id = zone_of(c->zp, phys);
	struct dispatch_waiter *w, *tmp;
	bool my_turn;

	spin_lock_irq(&c->lock);
	my_turn = (phys == c->zp->dispatch_wp[zone_id]);
	if (!my_turn) {
		/* GFP_ATOMIC인 이유: atomic context(bio 완료 콜백)에서도 이 경로를 타기 때문 
		 * — 이 파일의 다른 모든 atomic-reachable 할당과 같은 원칙. */
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

	/* 내 차례 — dispatch_wp 전진과 fire 호출을 반드시 이 락 안에서 함께 한다(드레인되는 대기 항목들도 마찬가지)
	 * — 둘을 락 밖에서 분리하면 다른 CPU의 동시 호출이 먼저 fire()를 불러버리는 레이스가 생김 (fire 구현들은 전부 sleep 안 해서 스핀락 안에서 안전). */
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

/* zone_dispatch_gate의 가장 흔한 쓰임 — "차례가 되면 이 bio를 제출". */
static void zone_dispatch_write(struct zns_base_c *c, sector_t phys, sector_t nr, struct bio *bio)
{
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

/* zone_pool_alloc으로 배정만 받고 못 내보내게 된 phys를 게이트에서 건너뛴다(dispatch_wp를 넘겨줌).
 * 안 하면 그 자리에서 dispatch_wp가 멈춰 그 zone의 이후 모든 쓰기가 영구히 대기열에 갇힌다 — 배정 후 실패하는 경로는 반드시 호출. */
static void zone_dispatch_cancel(struct zns_base_c *c, sector_t phys, sector_t nr)
{
	if (zone_dispatch_gate(c, phys, nr, dispatch_fire_noop, NULL))
		DMERR("zone dispatch: out of memory cancelling reservation (zone %u, phys %llu) — this zone's dispatch queue will stall",
		      zone_of(c->zp, phys), (unsigned long long)phys);
}

/* Zone Append는 실제 기록 위치를 컨트롤러가 결정하므로 같은 zone에 여러
 * bio를 동시 제출해도 된다. 단, 오프셋 0의 zone header가 먼저 durable해야
 * 하므로 새 zone의 append는 header 완료 콜백까지 이 큐에서 대기한다. */
static int zone_append_write(struct zns_base_c *c, unsigned int zid, struct bio *bio)
{
	struct append_waiter *w;

	spin_lock_irq(&c->lock);
	if (c->zp->append_ready[zid]) {
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

/* zone_dispatch_gate의 블로킹 버전 — compaction처럼 동기(submit_bio_wait) 코드에서 쓴다.
 * 자기 차례가 될 때까지 재워뒀다가 깨어나면 리턴, 그 다음 호출자가 직접 실제 쓰기를 제출하면 순서가 보장된다.
 * process context 전용(compaction_work_fn에서만 호출) — 재우는 대기라 atomic context 금지. */
static void zone_dispatch_wait_turn(struct zns_base_c *c, sector_t phys, sector_t nr)
{
	struct completion done;

	init_completion(&done);
	if (zone_dispatch_gate(c, phys, nr, dispatch_fire_complete, &done)) {
		/* 큐잉 자체가 실패(OOM) — 영원히 블로킹할 수는 없으니 순서 보장을 포기하고 진행(극단적 메모리 부족 상황에서만 발생) */
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

/* buf+off의 페이지를 구한다 — kmalloc(선형)/kvmalloc(vmalloc 폴백) 양쪽 지원.
 * SSTable 레코드 배열은 수 MB까지 커질 수 있어(compaction 반복 병합) kzalloc이
 * 실패할 수 있으므로 kvmalloc으로 잡고, bio에는 페이지 단위로 붙인다. */
static struct page *buf_page(void *buf, size_t off)
{
	void *addr = (char *)buf + off;

	return is_vmalloc_addr(addr) ? vmalloc_to_page(addr) : virt_to_page(addr);
}

/* buf의 nr_sectors 섹터를 phys부터 op(REQ_OP_READ/WRITE)로 동기 전송.
 * 한 bio는 최대 BIO_MAX_VECS 페이지(=1MB)만 담을 수 있어, 그보다 크면 청크로 쪼개 순차 제출한다(안 그러면 nr_vecs 초과로 bio_alloc이 BUG).
 * buf는 kvmalloc 가능. process context 전용(submit_bio_wait). WRITE는 호출자가 미리 zone_dispatch_wait_turn으로 전체 범위의 차례를 잡아둬야 순서가 보장. */
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

/* memtable에 lba->phys 기록. 기존 lba를 덮어쓴 거면 그 옛 phys의 zone은
 * 죽은 공간이 늘어난 것이므로 invalid_count를 올려준다(GC victim 선정 근거).
 * 호출자가 c->lock을 쥐고 있다고 가정. */
static int mapping_put(struct zns_base_c *c, u64 lba, u64 phys)
{
	u64 old_phys;
	int ret = skiplist_upsert(c->memtable, lba, phys, &old_phys);

	if (ret < 0)
		return ret;
	if (ret == 1)
		/* 옛 값이 SSTable에 있는(=upsert가 삽입인) 경우는 여기서 invalid_count를
       	 * 못 올린다(찾으려면 SSTable I/O 필요, atomic context라 불가). 그 stale
         * entry는 compaction이 세대 병합 시 반영한다. */
		c->zp->invalid_count[zone_of(c->zp, old_phys)]++;
	return 0;
}

/* memtable에서만 조회. .map()의 READ 분기가 이게 miss일 때만 SSTable도 훑는다(아래 sstable_read_* 체인) - 여긴 순수 memtable 조회 그대로 둔다.
 * 호출자가 c->lock을 쥐고 있다고 가정. */
static int mapping_get(struct zns_base_c *c, u64 lba, u64 *phys_out)
{
	return skiplist_lookup(c->memtable, lba, phys_out);
}

/* GC 이주 전용 조건부 매핑 갱신 — 이주 사이 사용자가 같은 lba를 덮어써 memtable이
 * 이미 새 값을 갖게 됐으면(현재값 != expected_old) 갱신을 포기하고 이주본(new_phys)을
 * 무효로 표시한다(최신값을 옛 값으로 되돌리지 않기 위함). memtable에 lba가 없으면
 * 현재값이 SSTable의 expected_old이므로 정상 이주로 보고 삽입.
 * 호출자가 c->lock 보유. [반환값] 0 이주 반영, 1 경합으로 스킵(둘 다 GC엔 정상), <0 오류.
	 * [한계] memtable 갱신만 — 이미 durable한 GC batch WAL 레코드는 못 되돌리므로, 스킵
 * 직전 크래시 시 replay가 이주본을 되살릴 수 있다(완전 해결은 per-entry seq 필요). */
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
	return 0;
}

/* c->sstables[]에서 phys로 항목을 찾아 제거(compaction이 병합해서 못 쓰게 된 옛 SSTable 정리용)
 * 순서 유지 불필요라 마지막 원소와 바꿔치기하는 O(1) 제거. 호출자가 c->lock을 쥐고 있어야 한다. */
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

/* zone_id의 섹터 start부터 wp까지 WAL 레코드를 순서대로 읽어 fn(fn_ctx, rec, sector)로 하나씩 넘긴다 - checkpoint 탐색과 실제 replay가 공유하는 공용 순회자.
 * ctr() 전용 process context라 락 없이 접근, submit_bio_wait 안전. */
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
			/* v1 compatibility: one record in the first 512B sector. */
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

/* wal_zone_for_each_record 콜백 — CHECKPOINT만 골라 마지막(=가장 최근) 것의
 * seq_no/split_gen/split_off를 남긴다. PUT은 적용하지 않고 그냥 지나친다. */
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
			c->zp->invalid_count[zone_of(c->zp, old_phys)]++;
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

/* wal_zones[]에 모인 WAL zone들을 재생한다. 배열은 zone_id 순으로 들어오지만,
 * WAL zone 회수 후에는 zone_id 순서 ≠ 기록 순서라 여기서 generation
 * (wal_gen[zone_id]) 오름차순으로 정렬해 논리 순서를 복원한다.
 * 1단계: generation 큰 zone부터 거꾸로 훑어 마지막 CHECKPOINT를 찾으면 멈춤.
 * 2단계: 각 zone을 (그 zone의 gen, 오프셋)이 체크포인트의 (split_gen, split_off)
 * 이전이면 스킵, 같은 gen이면 split_off부터, 이후 gen이면 전부 — generation
 * 오름차순으로 재생해야 같은 lba의 최신 값이 memtable에 최종 반영된다. */
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
				continue;              /* 이 zone 전체가 체크포인트 이전 */
			if (gen == split_gen)
				start = split_off;     /* 걸쳐있는 zone — split 지점부터 */
			/* gen > split_gen이면 전부 재생(start=1) */
		}

		wal_zone_for_each_record(c, zone_id, start, wp, wal_replay_cb, c);
	}

	/* 복구된 마지막 체크포인트 이후부터 seq_no를 이어가야 다음 flush가
	 * 아직 회수 안 된 옛 SSTable과 seq_no가 겹치지 않는다. */
	c->next_seq_no = found ? ckpt_seq + 1 : 0;

	/* 복구된 체크포인트의 split_gen을 durable 지점으로 seed — 재부팅 전에
	 * 이미 온전히 flush됐던 옛 WAL zone들을 다음 회수 때 바로 정리할 수 있다. */
	if (found)
		c->wal_durable_split_gen = split_gen;
}

/* zone_id 안에 순서대로 쌓여있는 SSTable들을 전부 찾아 c->sstables[]에 등록한다.
 * 헤더를 읽을 때마다 record_count로 다음 SSTable 시작 위치를 계산해 이어감.
 * ctr() 전용 process context. */
static void scan_sstable_zone(struct zns_base_c *c, unsigned int zone_id, sector_t wp)
{
	sector_t cur = 1;  /* 섹터 0은 zone 태그 헤더 */

	while (cur < wp) {
		sector_t phys = (sector_t)zone_id * c->zp->zone_sectors + cur;
		struct sstable_header hdr;
		sector_t data_sectors;

		if (read_sstable_header(c, phys, &hdr) || le32_to_cpu(hdr.magic) != SSTABLE_MAGIC)
			break;  /* 손상되었거나 여기서 SSTable들이 끝남 */

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

/* blkdev_report_zones가 zone마다 호출 — 하드웨어 wp로 쓰인 적 있는 zone인지 판단하고, 태그 헤더가 있으면 zone_tag[]/wp[]/dispatch_wp[]를 복원한다.
 * WAL/SSTable zone은 id만 모아뒀다가 스캔이 끝난 뒤 한 번에 처리한다. */
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
		return 0;  /* 한 번도 안 쓰인 zone */

	ret = read_zone_header(c, idx, &hdr);
	if (ret)
		return 0;
	/* 온디스크는 __le — 여기서 한 번에 CPU 엔디안으로 디코드 */
	magic = le32_to_cpu(hdr.magic);
	tag = le32_to_cpu(hdr.tag);
	gen = le64_to_cpu(hdr.gen);
	if (magic != ZONE_HEADER_MAGIC)
		return 0;  /* 헤더 없음/손상 — 이 zone은 복원 대상 아님 */

	real_wp = zone->wp - zone->start;  /* zone 기준 상대 wp */

	c->zp->zone_tag[idx] = tag;
	c->zp->append_ready[idx] = true;
	c->zp->wp[idx] = real_wp;
	/* dispatch_wp도 실제 하드웨어 wp까지 따라잡혀 있어야 한다.
	 * 안 그러면 재부팅 후 이 zone의 첫 쓰기가 dispatch_wp의 초기값(zone 시작)을 영원히 기다리게 된다. */
	c->zp->dispatch_wp[idx] = (sector_t)idx * c->zp->zone_sectors + real_wp;

	/* 아직 안 꽉 찬 zone이면 그 태그의 활성 zone으로 채택 (태그별 non-full zone은 유일하므로 zone_id 순서와 무관하게 이 판정이 옳다). */
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
		zone_dispatch_cancel(c, ctx->reserved_phys, ctx->reserved_nr);
		ctx->orig_bio->bi_status = status;
		atomic_dec(&c->foreground_writes);
		bio_endio(ctx->orig_bio);
		kfree(ctx);
	} else if (ctx->on_headers_done == submit_wal_async) {
		zone_dispatch_cancel(c, ctx->wal_phys, 1);
		zone_dispatch_cancel(c, ctx->reserved_phys, ctx->reserved_nr);
		ctx->orig_bio->bi_status = status;
		atomic_dec(&c->foreground_writes);
		bio_endio(ctx->orig_bio);
		kfree(ctx);
	} else if (ctx->on_headers_done == submit_sstable_write_async) {
		zone_dispatch_cancel(c, ctx->sstable_phys, ctx->sstable_nr_sectors);
		sstable_flush_complete(ctx, status);
	} else if (ctx->on_headers_done == submit_checkpoint_async) {
		zone_dispatch_cancel(c, ctx->wal_phys, 1);
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
		kfree(ctx);
		flush_chain_end(c);
	} else {
		DMERR("zone header failure: unknown continuation, leaking context safely");
	}
}

static void flush_memtable_work_fn(struct work_struct *work)
{
	struct memtable_flush_work *fw =
		container_of(work, struct memtable_flush_work, work);

	flush_memtable_async(fw->c, fw->old_memtable, fw->seq_no,
				 fw->split_gen, fw->split_off);
	kfree(fw);
}

/* WAL PUT 레코드(512B, 앞 32B만 유효) 비동기 제출. process/atomic context
 * 양쪽에서 불리므로 GFP_ATOMIC 필수(GFP_NOIO도 sleep 가능해 안전하지 않음). */
static void submit_wal_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	bool full;

	INIT_LIST_HEAD(&ctx->wal_link);
	spin_lock_irq(&c->lock);
	list_add_tail(&ctx->wal_link, &c->wal_pending);
	full = ++c->wal_pending_count >= wal_batch_max_records;
	spin_unlock_irq(&c->lock);
	mod_delayed_work(zns_wq, &c->wal_batch_work,
			 full ? 0 : msecs_to_jiffies(wal_batch_delay_ms));
}

/* ctx->headers[ctx->header_idx]의 zone 태그 헤더를 비동기로 제출.
 * .map() 안에서 블로킹하면 bio 스태킹 때문에 자기 자신을 기다리는 데드락이 되므로 WAL과 같은 완전 비동기 콜백 체인
 *  GFP_ATOMIC 이유도 submit_wal_async와 동일. */
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
	hdr->gen = cpu_to_le64(h->gen);  /* WAL zone이면 generation, 아니면 0 */
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
	/* 헤더는 항상 그 zone의 오프셋 0 — 같은 zone의 다른 WAL/data write보다 먼저 dispatch돼야 하므로 zone_dispatch_write를 거친다. */
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

/* Zone Append 완료 시 bio sector에 반환된 실제 주소를 WAL에 남긴다.
 * data가 WAL보다 먼저 durable하므로 crash 시 매핑 없는 orphan은 생길 수
 * 있지만, durable하지 않은 data를 가리키는 매핑은 생기지 않는다. */
static void data_append_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	struct zns_base_c *c = ctx->c;

	if (bio->bi_status) {
		zone_dispatch_cancel(c, ctx->reserved_phys, ctx->reserved_nr);
		bio->bi_end_io = ctx->orig_end_io;
		bio->bi_private = ctx->orig_private;
		bio->bi_iter.bi_sector = ctx->orig_sector;
		bio->bi_opf = ctx->orig_opf;
		kfree(ctx);
		atomic_dec(&c->foreground_writes);
		bio_endio(bio);
		return;
	}
	ctx->phys = bio->bi_iter.bi_sector;
	bio->bi_end_io = ctx->orig_end_io;
	bio->bi_private = ctx->orig_private;
	bio->bi_iter.bi_sector = ctx->orig_sector;
	bio->bi_opf = ctx->orig_opf;

	submit_wal_async(ctx);
}

/* data Zone Append 완료 후 제출한 WAL의 완료 콜백. WAL이 durable해진
 * 시점에 매핑을 공개하고 원본 bio를 완료한다. */
static void wal_commit_ctx(struct zns_io_ctx *ctx, blk_status_t wal_status,
			   bool allow_flush)
{
	struct zns_base_c *c = ctx->c;
	struct bio *orig = ctx->orig_bio;
	u64 lba = ctx->lba;
	sector_t phys = ctx->phys;
	sector_t reserved_phys = ctx->reserved_phys;
	sector_t reserved_nr = ctx->reserved_nr;
	struct skiplist *flushed_memtable = NULL;
	struct memtable_flush_work *flush_work = NULL;
	u64 flushed_seq = 0;
	u64 flushed_split_gen = 0;
	sector_t flushed_split_off = 0;
	int ret;

	if (wal_status) {
		zone_dispatch_cancel(c, reserved_phys, reserved_nr);
		kfree(ctx);
		orig->bi_status = wal_status;
		atomic_dec(&c->foreground_writes);
		bio_endio(orig);
		return;
	}

	spin_lock_irq(&c->lock);
	ret = mapping_put(c, lba, phys);
	if (!ret && allow_flush && !c->gc_active &&
	    c->memtable->count >= flush_threshold) {
		/* memtable 교체는 이 락 안에서 — skiplist_init도 GFP_ATOMIC이라 atomic 컨텍스트에서 불러도 안전하다. */
		struct skiplist *new_memtable = kzalloc(sizeof(*new_memtable), GFP_ATOMIC);

		flush_work = kzalloc(sizeof(*flush_work), GFP_ATOMIC);
		if (new_memtable && flush_work && skiplist_init(new_memtable) == 0) {
			unsigned int wal_zone = c->zp->active_zone[ZONE_TAG_WAL];

			flushed_memtable = c->memtable;
			flushed_seq = c->next_seq_no++;
			/* 이 순간 이후 WAL에 쌓이는 레코드는 새 memtable 몫
			 * -> replay가 "스왑 시점 기준 이전/이후"로 정확히 나누도록 체크포인트에 이 위치를 (generation, 오프셋)로 실어둔다.
			 * 절대 섹터가 아니라 논리 순번을 쓰는 이유는 WAL zone 회수 후 zone_id 순서 ≠ 기록 순서가 되기 때문. */
			flushed_split_gen = c->zp->wal_gen[wal_zone];
			flushed_split_off = c->zp->wp[wal_zone];
			c->memtable = new_memtable;
			/* 이 flush의 체크포인트가 곧 발행된다 — durable될 때까지 in-flight로 센다. split_gen은 스왑마다 단조 증가. */
			c->wal_ckpt_inflight++;
			c->wal_highest_issued_split_gen = flushed_split_gen;
			INIT_WORK(&flush_work->work, flush_memtable_work_fn);
			flush_work->c = c;
			flush_work->old_memtable = flushed_memtable;
			flush_work->seq_no = flushed_seq;
			flush_work->split_gen = flushed_split_gen;
			flush_work->split_off = flushed_split_off;
		} else {
			/* 못 만들면 이번 flush는 건너뛴다 — 다음 put에서 다시 시도됨 */
			kfree(flush_work);
			flush_work = NULL;
			kfree(new_memtable);
		}
	}
	spin_unlock_irq(&c->lock);
	/* Zone Append는 일반 dispatch gate를 통과하지 않으므로 완료 시점에
	 * 예약 순서 회계를 직접 전진시킨다. data는 mapping_put 뒤에 완료
	 * 처리해야 dispatch_wp==wp가 곧 "모든 매핑 공개 완료"를 뜻한다. */
	zone_dispatch_cancel(c, reserved_phys, reserved_nr);
	kfree(ctx);

	if (ret) {
		orig->bi_status = BLK_STS_RESOURCE;
		atomic_dec(&c->foreground_writes);
		bio_endio(orig);
		return;
	}

	/* flush는 fire-and-forget — WAL에 이미 durable하게 기록됐으므로 데이터 bio가 flush 완료를 기다릴 필요 없다. */
	if (flush_work)
		queue_work(zns_flush_wq, &flush_work->work);

	atomic_dec(&c->foreground_writes);
	bio_endio(orig);
}

static int wal_page_append_sync(struct zns_base_c *c, sector_t reserved_phys,
				void *buf, sector_t nr_sectors)
{
	struct bio *bio = bio_alloc(GFP_KERNEL, 1);
	int ret;

	if (!bio)
		return -ENOMEM;
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = (sector_t)zone_of(c->zp, reserved_phys) *
				 c->zp->zone_sectors;
	bio->bi_opf = REQ_OP_ZONE_APPEND | REQ_SYNC | REQ_FUA;
	if (bio_add_page(bio, virt_to_page(buf), nr_sectors * 512, 0) !=
	    nr_sectors * 512) {
		bio_put(bio);
		zone_dispatch_cancel(c, reserved_phys, nr_sectors);
		return -EIO;
	}
	zone_dispatch_wait_turn(c, reserved_phys, nr_sectors);
	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}

static void wal_batch_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(to_delayed_work(work),
					     struct zns_base_c, wal_batch_work);
	LIST_HEAD(batch);
	struct zns_io_ctx *ctx, *tmp;
	struct wal_page *page;
	sector_t wal_phys = 0;
	int new_zone = -1;
	unsigned int count = 0;
	sector_t wal_sectors;
	int ret;
	bool more;

	page = (struct wal_page *)get_zeroed_page(GFP_KERNEL);
	if (!page)
		ret = -ENOMEM;
	else
		ret = 0;

	spin_lock_irq(&c->lock);
	c->wal_batch_busy = true;
	while (!list_empty(&c->wal_pending) && count < wal_batch_max_records) {
		ctx = list_first_entry(&c->wal_pending, struct zns_io_ctx, wal_link);
		list_move_tail(&ctx->wal_link, &batch);
		c->wal_pending_count--;
		count++;
	}
	more = !list_empty(&c->wal_pending);
	wal_sectors = count == 1 ? 1 : WAL_PAGE_SECTORS;
	if (!ret && count) {
		if (unlikely(wal_batch_alloc_failures)) {
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

		/* data append는 이미 끝났지만 WAL이 durable하지 않으므로 원 bio를
		 * 실패시키면 ext4가 손상 상태로 전환된다. pending 앞쪽에 원래
		 * 순서대로 되돌리고 GC가 공간을 만든 뒤 다시 시도한다. */
		spin_lock_irq(&c->lock);
		stopping = c->stopping;
		if (!stopping) {
			list_splice_init(&batch, &c->wal_pending);
			c->wal_pending_count += count;
			c->wal_batch_busy = false;
		}
		spin_unlock_irq(&c->lock);
		if (!stopping) {
			free_page((unsigned long)page);
			queue_work(zns_gc_wq, &c->gc_work);
			mod_delayed_work(zns_wq, &c->wal_batch_work,
					 msecs_to_jiffies(100));
			return;
		}
		/* target 종료 중에는 더 이상 재큐잉할 수 없으므로 아래 공통
		 * 완료 경로에서 오류로 정리한다. */
	}
	if (ret)
		goto complete;

	count = 0;
	list_for_each_entry(ctx, &batch, wal_link) {
		struct wal_record *rec = wal_sectors == 1 ?
			(struct wal_record *)page : &page->records[count];

		rec->type = cpu_to_le32(WAL_REC_PUT);
		rec->put.lba = cpu_to_le64(ctx->lba);
		rec->put.phys = cpu_to_le64(ctx->phys);
		count++;
	}
	if (wal_sectors > 1) {
		page->magic = cpu_to_le32(WAL_PAGE_MAGIC);
		page->version = cpu_to_le16(WAL_PAGE_VERSION);
		page->count = cpu_to_le16(count);
		page->crc32 = 0;
		page->crc32 = cpu_to_le32(crc32(~0U, page, PAGE_SIZE));
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
		ret = wal_page_append_sync(c, wal_phys, page, wal_sectors);
	}

complete:
	list_for_each_entry_safe(ctx, tmp, &batch, wal_link) {
		bool last = list_is_last(&ctx->wal_link, &batch);

		list_del_init(&ctx->wal_link);
		wal_commit_ctx(ctx, ret ? BLK_STS_IOERR : 0, last);
	}
out:
	if (page)
		free_page((unsigned long)page);
	spin_lock_irq(&c->lock);
	c->wal_batch_busy = false;
	more = !list_empty(&c->wal_pending);
	spin_unlock_irq(&c->lock);
	if (more)
		mod_delayed_work(zns_wq, &c->wal_batch_work, 0);
}

/* flush 체인(memtable → SSTable → checkpoint)의 모든 종료 지점에서 정확히 한 번 호출 — 성공/실패 무관.
 * wal_put_done의 스왑에서 늘린 in-flight 카운터를 여기서 되돌린다.
 * 카운터가 0이 되면(발행된 체크포인트가 전부 durable) 그제야 durable 지점을 highest_issued까지 올리고 WAL zone 회수를 큐잉한다.
 * 개별 체크포인트의 완료 순서에 기대지 않는 이유는 bio 완료가 out-of-order일 수 있어서(옛 체크포인트가 아직 안 끝났는데 그 세대 WAL을 지우면 안 됨). */
static void flush_chain_end(struct zns_base_c *c)
{
	bool advanced = false;
	bool need_gc;

	spin_lock_irq(&c->lock);
	if (--c->wal_ckpt_inflight == 0 &&
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

/* 모든 SSTable 청크가 durable하게 쓰인 뒤(또는 쓰기 실패 시) 호출.
 * 성공했으면 checkpoint 체인으로 넘겨(진짜 마지막 정리는 checkpoint_write_done) 다음 재부팅 때 이 세대의 WAL을 건너뛸 수 있게 한다.
 * 실패했으면 checkpoint 없이 바로 정리. bio는 호출자(sstable_write_chunk_done)가 이미 put했다. */
static void sstable_flush_complete(struct zns_io_ctx *ctx, blk_status_t status)
{
	struct zns_base_c *c = ctx->c;
	sector_t wal_phys;
	int new_wal_zone;
	int ret;

	if (status) {
		DMERR("SSTable flush write failed (seq=%llu): this generation's data is lost from the SSTable, but remains recoverable from WAL replay",
		      (unsigned long long)ctx->checkpoint_seq);
	} else {
		struct sstable_header *hdr = ctx->sstable_buf;
		int rret;
		bool should_compact;

		/* 이 시점부터 이 SSTable을 읽기 경로에서 찾을 수 있어야 한다 —
		 * checkpoint를 아직 안 썼어도 데이터 자체는 이미 durable. */
		spin_lock_irq(&c->lock);
		rret = sstable_register(c, ctx->sstable_phys, le64_to_cpu(hdr->seq_no),
					  le64_to_cpu(hdr->record_count),
					  le64_to_cpu(hdr->min_lba), le64_to_cpu(hdr->max_lba), GFP_ATOMIC);
		should_compact = !rret && c->nr_sstables >= compaction_k;
		spin_unlock_irq(&c->lock);
		if (rret)
			DMERR("SSTable flush: failed to register in-memory index (seq=%llu) — unreadable until next restart's recovery scan",
			      (unsigned long long)ctx->checkpoint_seq);

		/* atomic context라 queue_work만(비블로킹) — 실제 병합은
		 * compaction_wq 워커(process context)에서 처리 */
		if (should_compact)
			queue_work(zns_compaction_wq, &c->compaction_work);
	}

	kvfree(ctx->sstable_buf);

	if (status) {
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
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
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
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

	rec = kzalloc(512, GFP_ATOMIC);
	if (!rec) {
		DMERR("checkpoint write: out of memory (seq=%llu), skipping — replay will just do extra work next time",
		      (unsigned long long)ctx->checkpoint_seq);
		zone_dispatch_cancel(c, ctx->wal_phys, 1);
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
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
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
		kfree(ctx);
		flush_chain_end(c);
		return;
	}
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector =
		(zone_of(c->zp, ctx->wal_phys) * c->zp->zone_sectors);
	bio->bi_opf = REQ_OP_ZONE_APPEND;
	page = virt_to_page(rec);
	bio_add_page(bio, page, 512, offset_in_page(rec));
	bio->bi_end_io = checkpoint_write_done;
	bio->bi_private = ctx;
	if (zone_append_write(c, zone_of(c->zp, ctx->wal_phys), bio)) {
		bio->bi_status = BLK_STS_RESOURCE;
		bio_endio(bio);
	}
}

/* CHECKPOINT 기록이 끝난 뒤(성공하든 실패하든) flush 체인의 진짜 마지막 — 여기서 비로소 old_memtable을 완전히 버린다. */
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

	skiplist_destroy(ctx->old_memtable);
	kfree(ctx->old_memtable);
	kfree(ctx);
	flush_chain_end(c);
}

/* ctx->sstable_buf(헤더+레코드, 직렬화 완료)를 PAGE_SIZE 단위로 잘라 비동기 기록.
 * 마지막 page는 남은 바이트만큼만 붙여야 bio 총 크기가 정확히 일치한다(안 그러면 다음 flush가 그 틈/겹침을 밟는다). */
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

	/* SSTable이 1MB(BIO_MAX_VECS)를 넘으면 한 bio에 다 못 담아 nr_vecs 초과 BUG가 나므로,
	 * 청크 단위로 쪼개 gate를 거쳐 순차 제출하고 각 청크 완료(sstable_write_chunk_done)에서 다음 청크를 이어간다. */
	bio = bio_alloc(GFP_ATOMIC, nr_pages);
	if (!bio) {
		DMERR("SSTable flush: bio_alloc failed, dropping this generation (data remains in WAL)");
		/* 이미 배정된 SSTable phys 중 아직 안 나간 부분은 취소해 dispatch 정지를 막는다. */
		if (remaining_sectors > 0)
			zone_dispatch_cancel(c, chunk_phys, remaining_sectors);
		kvfree(ctx->sstable_buf);
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
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
	zone_dispatch_write(c, chunk_phys, chunk_sectors, bio);
}

/* SSTable 청크 하나가 durable해진 뒤 — 남은 청크가 있으면 이어서 제출, 아니면 flush 후처리(sstable_flush_complete).
 * 쓰기 실패는 곧바로 후처리로 넘겨 WAL 복구에 맡긴다. */
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
	sstable_flush_complete(ctx, 0);  /* 전 청크 durable */
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
	struct zns_io_ctx *ctx;
	u64 i = 0;
	int ret;

	if (old_memtable->count == 0) {
		skiplist_destroy(old_memtable);
		kfree(old_memtable);
		flush_chain_end(c);
		return;
	}

	/* 512(헤더 1섹터) + 레코드들(16B*count)을 섹터 단위로 올림 */
	data_bytes = 512 + round_up(old_memtable->count * sizeof(struct sstable_record), 512);
	nr_sectors = data_bytes / 512;
	alloc_bytes = round_up(data_bytes, PAGE_SIZE);

	/* kvzalloc은 큰 버퍼가 물리 연속 할당에 실패하면 vmalloc로 폴백한다.
	 * 아래 비동기 SSTable writer는 buf_page()로 두 종류 버퍼를 모두 지원한다. */
	buf = kvzalloc(alloc_bytes, GFP_KERNEL);
	if (!buf) {
		DMERR("SSTable flush: out of memory (seq=%llu), dropping this generation (data remains in WAL)",
		      (unsigned long long)seq_no);
		skiplist_destroy(old_memtable);
		kfree(old_memtable);
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
	ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors, &phys, &new_sstable_zone, false);
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("SSTable flush: zone_pool_alloc failed (%d, seq=%llu), dropping this generation (data remains in WAL)",
		      ret, (unsigned long long)seq_no);
		kvfree(buf);
		skiplist_destroy(old_memtable);
		kfree(old_memtable);
		flush_chain_end(c);
		return;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		DMERR("SSTable flush: out of memory building ctx (seq=%llu), dropping this generation (data remains in WAL)",
		      (unsigned long long)seq_no);
		kvfree(buf);
		skiplist_destroy(old_memtable);
		kfree(old_memtable);
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

/* .map()의 READ 분기가 memtable을 못 찾았을 때 이어받는 비동기 체인의 컨텍스트.
 * candidates는 .map()이 미리 걸러둔 SSTable 스냅샷 — 같은 lba가 여러 SSTable에 걸쳐 있을 수 있어
 * 첫 hit에서 안 멈추고 끝까지 훑어 seq_no가 가장 큰 것(best_seq/best_phys)으로 갱신해나간다.
 * 각 후보 안에서는 on-disk binary search(sec_buf에 512B 섹터씩 읽어가며 bs_lo/bs_hi로 좁힘)로 찾아,
 * SSTable 전체를 통짜로 읽던 예전 방식의 거대 GFP_ATOMIC 할당·read amplification·bio_vec 초과 BUG를 모두 없앤다. */
struct sstable_read_ctx {
	struct zns_base_c *c;
	struct bio *orig_bio;
	u64 lba;
	sector_t offset_in_block;
	struct sstable_info *candidates;
	unsigned int nr_candidates;
	unsigned int idx;
	void *sec_buf;      /* 512B — probe가 읽는 섹터(모든 probe/후보가 재사용) */
	long bs_lo, bs_hi;  /* 현재 후보의 이진 탐색 범위(레코드 인덱스) */
	long bs_mid;        /* 이번 probe가 읽은 mid — 콜백이 레코드 위치 계산에 씀 */
	int best_found;
	u64 best_seq;
	sector_t best_phys;
};

static void sstable_read_probe(struct sstable_read_ctx *rctx);
static void sstable_probe_done(struct bio *bio);

/* 후보를 다 훑었으면 결과를 원본 bio에 반영하고 체인을 끝낸다 */
static void sstable_read_finish(struct sstable_read_ctx *rctx)
{
	struct bio *orig = rctx->orig_bio;
	struct zns_base_c *c = rctx->c;
	unsigned int i;

	if (rctx->best_found) {
		struct zns_read_pin *pin = dm_per_bio_data(orig, sizeof(*pin));
		sector_t best_phys = rctx->best_phys;
		u64 cur;

		/* best_phys(SSTable가 준 위치)를 읽기 직전, 그 사이 GC가 이 lba를 옮겼는지
		 * 락 안에서 재확인 — 옮겼으면 memtable의 새 위치를 쓴다(TOCTOU). 그리고 실제
		 * 읽을 데이터 zone을 pin(end_io가 unpin)해 GC가 그 사이 reset 못하게 한다. */
		spin_lock_irq(&c->lock);
		if (mapping_get(c, rctx->lba, &cur))
			best_phys = cur;
		pin->zone = zone_of(c->zp, best_phys);
		zone_read_get(c->zp, pin->zone);
		spin_unlock_irq(&c->lock);

		orig->bi_iter.bi_sector = best_phys + rctx->offset_in_block;
		bio_set_dev(orig, c->dev->bdev);
		submit_bio_deferred(orig);
	} else {
		/* 후보 SSTable들의 min/max_lba 범위엔 들었지만 실제 레코드는
		 * 없었던 경우 — 한 번도 안 쓰인 블록과 같은 취급(zero-fill) */
		zero_fill_bio(orig);
		bio_endio(orig);
	}
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
		rctx->idx++;  /* 범위 밖 — 이 후보는 애초에 볼 필요 없음 */
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

/* 현재 후보에서 이진 탐색 한 스텝 — mid가 든 512B 섹터를 비동기로 읽는다.
 * process/atomic context 양쪽에서 불리므로 GFP_ATOMIC. */
static void sstable_read_probe(struct sstable_read_ctx *rctx)
{
	struct sstable_info *si = &rctx->candidates[rctx->idx];
	const unsigned int per_sec = 512 / sizeof(struct sstable_record);  /* =32 */
	sector_t sec_no;
	struct bio *bio;

	if (rctx->bs_lo > rctx->bs_hi) {  /* 이 후보엔 없음 → 다음 후보 */
		rctx->idx++;
		sstable_read_next_candidate(rctx);
		return;
	}
	rctx->bs_mid = rctx->bs_lo + (rctx->bs_hi - rctx->bs_lo) / 2;
	sec_no = si->phys + 1 + rctx->bs_mid / per_sec;

	bio = bio_alloc(GFP_ATOMIC, 1);
	if (!bio) {  /* 못 읽으면 이 후보 포기(열화) */
		rctx->idx++;
		sstable_read_next_candidate(rctx);
		return;
	}
	bio_set_dev(bio, rctx->c->dev->bdev);
	bio->bi_iter.bi_sector = sec_no;
	bio->bi_opf = REQ_OP_READ;
	bio_add_page(bio, virt_to_page(rctx->sec_buf), 512, offset_in_page(rctx->sec_buf));
	bio->bi_end_io = sstable_probe_done;
	bio->bi_private = rctx;
	submit_bio_deferred(bio);
}

/* probe 읽기가 끝난 뒤 — mid 레코드를 비교해 좁히거나, hit이면 best_* 갱신 후
 * 다음 후보로(lba는 SSTable당 유일하므로 더 볼 필요 없음). */
static void sstable_probe_done(struct bio *bio)
{
	struct sstable_read_ctx *rctx = bio->bi_private;
	struct sstable_info *si = &rctx->candidates[rctx->idx];
	blk_status_t status = bio->bi_status;
	const unsigned int per_sec = 512 / sizeof(struct sstable_record);
	struct sstable_record *r;

	bio_put(bio);

	if (status) {  /* 읽기 실패 → 이 후보 포기 */
		rctx->idx++;
		sstable_read_next_candidate(rctx);
		return;
	}
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

/* compaction 진행 중 victim SSTable 하나에서 읽어온 레코드 배열 + k-way merge 진행 상태(idx)
 * 각자 이미 lba 정렬돼 있어 merge 시 재정렬 불필요. */
struct compaction_source {
	struct sstable_record *recs;
	u64 count;
	u64 idx;                /* 다음으로 볼 레코드 인덱스 */
	u64 seq_no;              /* 이 소스의 세대 — 동률(같은 lba) 시 우선순위 판단 */
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

/* discarded[]에 쌓인 병합 중 밀려난 옛 데이터 phys들의 invalid_count를 한 번에 반영 — merge를 락 밖에서 다 끝낸 뒤 호출. */
static void apply_discarded_invalid_counts(struct zns_base_c *c, sector_t *discarded, unsigned int count)
{
	unsigned int i;

	if (count == 0)
		return;
	spin_lock_irq(&c->lock);
	for (i = 0; i < count; i++)
		c->zp->invalid_count[zone_of(c->zp, discarded[i])]++;
	spin_unlock_irq(&c->lock);
}

/* nr_srcs개의 정렬된 소스를 병합해 out에 쓴다. 같은 lba가 여러 소스에 걸쳐 있으면 seq_no가 가장 높은 것만 남기고,
 * 밀려난 나머지의 phys는 discarded_out[]에 쌓아 나중에 invalid_count에 반영한다. 
 * out/discarded_out은 호출자가 이미 넉넉히 할당해뒀다고 가정. */
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
			break;  /* 모든 소스 소진 */

		/* 그 lba를 지금 가리키는 모든 소스를 확인 — seq_no 최댓값만 채택, 나머지는 폐기(그 데이터 phys를 invalid_count 대상으로 기록) */
		for (s = 0; s < nr_srcs; s++) {
			if (srcs[s].idx >= srcs[s].count)
				continue;
			if (le64_to_cpu(srcs[s].recs[srcs[s].idx].lba) != min_lba)
				continue;

			if (best_src < 0 || srcs[s].seq_no > best_seq) {
				if (best_src >= 0)
					discarded_out[ndisc++] = best_phys;  /* 이전 최선이 밀려남 */
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

/* compaction_wq 워커(process context, submit_bio_wait/GFP_KERNEL 사용 가능).
 * 가장 오래된 compaction_k개를 읽어 k-way merge한 뒤 새 SSTable로 동기 기록하고,
 * 그게 durable해진 다음에야 옛 색인 제거 + 하드웨어 reset — 이 순서 덕분에 어느 지점에서 크래시가 나도 안전. */
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
	u64 out_seq_no = 0;
	u64 merged_min_lba = 0, merged_max_lba = 0;

	spin_lock_irq(&c->lock);
	if (c->nr_sstables < k) {
		spin_unlock_irq(&c->lock);
		return;  /* 그 사이 조건이 이미 해소됨(레이스) — 할 일 없음 */
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

	/* process context라 kvmalloc(vmalloc 폴백) 사용 — 병합 버퍼는 수십 MB까지
	 * 커질 수 있어 물리 연속 kzalloc은 실패한다(out_buf/srcs[].recs와 동일 방식). */
	merged = kvzalloc(round_up(total_input_records * sizeof(struct sstable_record), 512), GFP_KERNEL);
	discarded = kvmalloc_array(total_input_records ? total_input_records : 1, sizeof(*discarded), GFP_KERNEL);
	if (!merged || !discarded) {
		DMERR("compaction: out of memory allocating merge buffers, aborting this run");
		goto out_free_sources;
	}

	merged_count = merge_sstable_sources(srcs, k, merged, discarded, &discarded_count);
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

	/* 압축 결과는 새 논리 쓰기가 아니다. next_seq_no를 붙이면
	 * 병합 대상이 아닌 더 최신 SSTable보다 새로운 것처럼 보여
	 * 예전 phys가 최신 mapping을 덮어쓴다. 제거할 victim 중 최대
	 * seq를 유지하면 대상 밖의 더 최신 SSTable이 계속 우선한다. */
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
	ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors, &new_phys, &new_zone, false);
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("compaction: zone_pool_alloc failed (%d), aborting this run — old SSTables remain valid", ret);
		goto out_free_out_buf;
	}

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

	/* 병합된 SSTable 데이터를 실제로 durable하게 기록 — 전체 범위의 차례를 먼저 잡고(순서 게이트), 
	 * 큰 SSTable은 sstable_io_sync가 ≤1MB bio로 쪼개 순차 제출한다(한 bio에 다 담으면 nr_vecs 초과로 BUG). */
	zone_dispatch_wait_turn(c, new_phys, nr_sectors);
	ret = sstable_io_sync(c, REQ_OP_WRITE, new_phys, out_buf, nr_sectors);
	if (ret) {
		DMERR("compaction: merged SSTable write failed (err=%d), aborting this run — old SSTables remain valid", ret);
		goto out_free_out_buf;
	}

	/* 여기서부터 커밋 — 새 SSTable은 이제 완전히 durable함 */
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
			/* 마지막 조건: 아직 발행 안 된 배정분이 남은 zone은 회수
			 * 대상에서 제외 — gc_select_victim과 같은 이유(배정만 되고
			 * 아직 안 나간 쓰기가 있는 zone을 reset하면 그 쓰기가 유실되고
			 * dispatch도 영구히 멈춘다). */
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
}

/* ============================================================================
 * GC — 가득 찬 데이터 zone 중 무효 비율이 가장 높은 것을 회수
 * ============================================================================ */

/* [데이터 흐름] GC가 victim에서 살아있는 lba를 찾을 때 (lba, 지금 phys)만
 * 담아두는 스냅샷 원소 — memtable/SSTable 스캔 결과를 공용으로 담는다. */
struct gc_candidate {
	u64 lba;
	sector_t phys;
};

/* GC 라운드 전용 latest-map. victim을 한 번이라도 가리킨 LBA만
 * 보관한다. 오래된 SSTable부터 순차 스캔하며 phys를 덮어쓰면
 * 마지막에는 각 LBA의 최신 SSTable 위치만 남는다. */
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

/* [락] 호출자가 c->lock을 쥐고 있어야 한다. */
static unsigned int gc_count_free_zones(struct zone_pool *zp)
{
	unsigned int z, count = 0;

	for (z = 0; z < zp->nr_zones; z++)
		if (zp->zone_tag[z] == ZONE_TAG_FREE)
			count++;
	return count;
}

/* free zone이 gc_low_watermark 이하로 떨어지면 GC를 큐잉
 * zone_pool_alloc으로 free zone을 소비할 수 있는 지점(.map()의 WRITE 분기)에서 호출. */
static void maybe_trigger_gc(struct zns_base_c *c)
{
	bool should_gc;

	spin_lock_irq(&c->lock);
	should_gc = gc_count_free_zones(c->zp) <= gc_low_watermark;
	spin_unlock_irq(&c->lock);

	if (should_gc)
		queue_work(zns_gc_wq, &c->gc_work);
}

/* 닫힌 데이터 zone(USER_DATA/GC_DATA) 중 invalid_count가 가장 큰 것을 victim으로 선정.
 * GC_DATA도 대상 — 재배치한 데이터도 다시 덮어써지면 죽는다. SSTable/WAL zone은 compaction 전담이라 제외.
 * 각 continue 조건의 근거는 report/bugfix-log.md 참고. [락] 이 함수 자체가 잠금. */
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
		/* 아직 쓰는 중인 활성 zone은 대상 아님. "닫힘"을 wp==zone_sectors로
		 * 판정하면 안 됨 — rollover가 항상 몇 섹터 남기고 넘어가 절대 안 참. */
		if (z == c->zp->active_zone[tag])
			continue;
		/* 아직 발행 안 된 배정분이 남은 zone은 건드리면 안 됨 — .map()은 phys를 배정만 하고,
		 * 매핑은 wal_put_done에서야 등록되므로 아래 스캔이 진행 중인 쓰기를 못 본다.
		 * mapping_put이 dispatch보다 먼저 실행되므로 이 등식이 "배정분 전부 발행됨 = 전부 memtable에 보임"을 뜻한다. */
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
	bio->bi_iter.bi_sector = append ?
		(zone_of(c->zp, phys) * c->zp->zone_sectors) : phys;
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

/* 최대 한 WAL page에 들어가는 live block을 연속 GC_DATA 공간에 기록하고,
 * 같은 묶음의 조건부 mapping 갱신을 WAL page 하나로 durable하게 만든다. */
static int gc_relocate_batch(struct zns_base_c *c,
			     struct gc_live_entry **entries, unsigned int count)
{
	void *data = NULL;
	struct wal_page *wal = NULL;
	sector_t data_phys, wal_phys;
	sector_t data_sectors = (sector_t)count * BLOCK_SECTORS;
	int new_data_zone = -1, new_wal_zone = -1;
	unsigned int i;
	int ret = 0;

	data = kvzalloc((size_t)data_sectors * 512, GFP_KERNEL);
	wal = (struct wal_page *)get_zeroed_page(GFP_KERNEL);
	if (!data || !wal) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < count; i++) {
		ret = sstable_io_sync(c, REQ_OP_READ, entries[i]->phys,
				       (char *)data + (size_t)i * PAGE_SIZE,
				       BLOCK_SECTORS);
		if (ret) {
			DMERR("gc: batch source read failed at phys=%llu (%d)",
			      (unsigned long long)entries[i]->phys, ret);
			goto out;
		}
	}

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_GC_DATA, data_sectors,
			      &data_phys, &new_data_zone, true);
	spin_unlock_irq(&c->lock);
	if (ret)
		goto out;
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
	ret = zone_pool_alloc(c->zp, ZONE_TAG_WAL, WAL_PAGE_SECTORS,
			      &wal_phys, &new_wal_zone, true);
	spin_unlock_irq(&c->lock);
	if (ret)
		goto out;
	if (new_wal_zone >= 0) {
		struct zone_header *hdr = kzalloc(512, GFP_KERNEL);
		sector_t hdr_phys = (sector_t)new_wal_zone * c->zp->zone_sectors;

		if (!hdr) {
			zone_dispatch_cancel(c, hdr_phys, 1);
			zone_dispatch_cancel(c, wal_phys, WAL_PAGE_SECTORS);
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
			zone_dispatch_cancel(c, wal_phys, WAL_PAGE_SECTORS);
			zone_quarantine(c, new_wal_zone, ZONE_TAG_WAL, BLK_STS_IOERR);
			goto out;
		}
		zone_append_header_done(c, new_wal_zone, 0);
	}

	wal->magic = cpu_to_le32(WAL_PAGE_MAGIC);
	wal->version = cpu_to_le16(WAL_PAGE_VERSION);
	wal->count = cpu_to_le16(count);
	for (i = 0; i < count; i++) {
		wal->records[i].type = cpu_to_le32(WAL_REC_GC_PUT);
		wal->records[i].gc_put.lba = cpu_to_le64(entries[i]->lba);
		wal->records[i].gc_put.phys = cpu_to_le64(data_phys + (sector_t)i * BLOCK_SECTORS);
		wal->records[i].gc_put.expected_old = cpu_to_le64(entries[i]->phys);
	}
	wal->crc32 = cpu_to_le32(crc32(~0U, wal, PAGE_SIZE));
	ret = wal_page_append_sync(c, wal_phys, wal, WAL_PAGE_SECTORS);
	if (ret) {
		DMERR("gc: batch WAL append failed (%u entries, err=%d)", count, ret);
		zone_quarantine(c, zone_of(c->zp, wal_phys), ZONE_TAG_WAL,
				BLK_STS_IOERR);
		goto out;
	}

	spin_lock_irq(&c->lock);
	for (i = 0; i < count; i++) {
		sector_t old_phys = entries[i]->phys;
		sector_t new_phys = data_phys + (sector_t)i * BLOCK_SECTORS;
		int put = mapping_put_if_match(c, entries[i]->lba, old_phys, new_phys);

		if (put == 0)
			entries[i]->phys = new_phys;
		else if (put == 1 && !mapping_get(c, entries[i]->lba, &entries[i]->phys))
			ret = -EAGAIN;
		if (ret)
			break;
	}
	spin_unlock_irq(&c->lock);
out:
	if (wal)
		free_page((unsigned long)wal);
	kvfree(data);
	return ret;
}

/* GC 본체 — victim 하나 회수. worker의 첫 victim에서 전체 latest-map을
 * 한 번 구축하고 cycle에 보존, 이후 victim은 같은 map을 재사용한다.
 * → 전부 성공했을 때만 zone_reset_hw+mark_free(회수는 안전 확인 후 마지막에만).
 * [반환값] 회수 성공, 다음 후보 시도, 또는 대상 없음/실패(reset 안 함).
 * [락] 스냅샷/조회/커밋 각각 짧게, I/O는 락 밖. process context 전용.
 * foreground 경합은 mapping_put_if_match와 WAL_REC_GC_PUT(expected_old)로
 * 정상 실행·crash replay 모두에서 최신 mapping을 보존한다. */
enum gc_reclaim_result {
	GC_RECLAIM_STOP = 0,
	GC_RECLAIM_DONE,
	GC_RECLAIM_TRY_NEXT,
};

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
	bool ok = true;
	bool reclaimed = false;
	unsigned long victim_started = jiffies;
	unsigned long relocation_started;

	victim = gc_select_victim(c, cycle->excluded);
	if (victim == ZONE_NONE)
		return GC_RECLAIM_STOP;  /* 회수할 만한 zone이 없음 */
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
	cycle->sstables = snapshot;
	cycle->nr_sstables = snap_count;
	cycle->built = true;
	DMINFO("gc: latest-map ready for victim %u (entries=%u, memtable=%u, sstables=%u, elapsed=%ums)",
	       victim, live_map.count, nr_mt, snap_count,
	       jiffies_to_msecs(jiffies - victim_started));

refresh_current:
	/* snapshot 후 도착한 foreground overwrite를 반영. gc_active 덕분에
	 * 이 순회 중 memtable이 frozen 상태로 숨지 않는다. 새 엔트리
	 * 삽입은 필요 없고, 이미 victim 후보인 LBA의 최신 phys만 갱신. */
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
	/* 이주 data와 batch WAL page까지 감안해 물리 공간
	 * 순이익이 없는 fallback victim은 건드리지 않는다. 안 그러면
	 * 100% live zone을 GC_DATA로 복사하며 reserve만 소모할 수 있다. */
	if (live_sectors +
	    DIV_ROUND_UP_ULL(live_sectors / BLOCK_SECTORS,
			     WAL_PAGE_MAX_RECORDS) * WAL_PAGE_SECTORS >=
	    used_sectors) {
		DMINFO("gc: skipping victim %u: no positive reclaim gain (used=%llu, live=%llu)",
		       victim, (unsigned long long)used_sectors,
		       (unsigned long long)live_sectors);
		cycle->excluded[victim] = true;
		kvfree(mt_candidates);
		return GC_RECLAIM_TRY_NEXT;
	}
	DMINFO("gc: relocating victim %u (used=%llu, live=%llu, invalid=%llu, entries=%llu, free_zones=%u)",
	       victim, (unsigned long long)used_sectors,
	       (unsigned long long)live_sectors,
	       (unsigned long long)(used_sectors - live_sectors),
	       (unsigned long long)(live_sectors / BLOCK_SECTORS),
	       free_at_start);
	relocation_started = jiffies;
	reloc_batch = kcalloc(WAL_PAGE_MAX_RECORDS, sizeof(*reloc_batch), GFP_KERNEL);
	if (!reloc_batch) {
		ok = false;
		goto out_free_snapshot;
	}

	/* latest phys가 아직 victim 안인 LBA만 live. 이주 중 사용자
	 * overwrite는 batch 마지막의 mapping_put_if_match가 걸러낸다. */
	for (i = 0; i < live_map.capacity; i++) {
		struct gc_live_entry *entry = &live_map.entries[i];

		if (!entry->used || entry->phys < vstart || entry->phys >= vend)
			continue;
		reloc_batch[reloc_count++] = entry;
		if (reloc_count < WAL_PAGE_MAX_RECORDS)
			continue;
		if (gc_relocate_batch(c, reloc_batch, reloc_count)) {
			DMERR("gc: relocation failed, aborting this round without resetting victim zone %u", victim);
			ok = false;
			break;
		}
		nr_relocated += reloc_count;
		reloc_count = 0;
		if (nr_relocated / 32768 != (nr_relocated - WAL_PAGE_MAX_RECORDS) / 32768) {
			u64 elapsed_ms = jiffies_to_msecs(jiffies - relocation_started);

			DMINFO("gc: victim %u progress relocated=%u/%llu (%llums, %llu entries/s)",
			       victim, nr_relocated,
			       (unsigned long long)(live_sectors / BLOCK_SECTORS),
			       (unsigned long long)elapsed_ms,
			       (unsigned long long)(elapsed_ms ?
				div64_u64((u64)nr_relocated * 1000, elapsed_ms) : 0));
		}
	}
	if (ok && reloc_count) {
		if (gc_relocate_batch(c, reloc_batch, reloc_count)) {
			DMERR("gc: final relocation batch failed, preserving victim zone %u", victim);
			ok = false;
		} else {
			nr_relocated += reloc_count;
		}
	}
	kfree(reloc_batch);
	reloc_batch = NULL;
	if (!ok)
		goto out_free_snapshot;

	/* 전부 성공 — victim zone에 더 이상 살아있는 데이터가 없다고 확신할 수 있으므로 실제로 회수.
	 * reset 직전 진행 중인 읽기(read pin)가 전부 끝나길 기다린다(회수 안전). */
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
	}

out_free_snapshot:
	kfree(reloc_batch);
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

/* zone_pool_alloc 시도. 공간이 부족하면 GC를 worker에 트리거하고
 * -EAGAIN을 반환한다. .map() 호출자는 bio를 DM core에 requeue해 GC가
 * zone을 회수한 뒤 다시 map하게 한다. 여기서 flush_work()로 기다리면
 * ext4 writeback/JBD2가 GC I/O의 진행 자체를 막는 순환 대기가 생긴다.
 * [반환값] 0 성공, -EAGAIN GC 후 requeue 필요, -ENOSPC 반복 GC에도 진전 없음. */
static int zone_pool_alloc_with_gc_retry(struct zns_base_c *c, enum zone_tag tag, sector_t nr,
					   sector_t *phys_out, int *new_zone_out)
{
	int ret;

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, tag, nr, phys_out, new_zone_out, false);
	if (!ret)
		c->gc_no_progress = 0;
	/* 이전 무진전 횟수가 임계치를 넘었더라도 지금 GC가 victim을 이주
	 * 중이면 결과가 날 때까지 requeue해야 한다. 여기서 ENOSPC를 내면
	 * 느린 대형-zone GC가 잠시 뒤 성공해도 파일시스템은 먼저 I/O error를
	 * 받아 손상된다. */
	else if (c->gc_no_progress >= 3 && !c->gc_active)
		ret = -ENOSPC;
	spin_unlock_irq(&c->lock);

	if (ret) {
		if (ret == -ENOSPC)
			return ret;
		queue_work(zns_gc_wq, &c->gc_work);
		return -EAGAIN;
	}
	return 0;
}

/* zns_gc_wq 워커 — free zone이 watermark 이하인 동안 gc_reclaim_one_victim 반복 (한 번에 하나만 회수하면 쓰기를 못 따라잡음).
 * false(대상 없음/실패) 시 종료. per-trigger 상한(nr_zones)은 방어용
 * victim 로직 결함으로 무한 회수 순환에 빠져도 워커가 반드시 끝나게 한다. process context 전용. */
static void gc_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(work, struct zns_base_c, gc_work);
	bool still_low;
	unsigned int attempts;
	unsigned int reclaimed = 0;
	bool deferred = false;
	struct gc_cycle cycle = { 0 };
	enum gc_reclaim_result result;
	unsigned int free_at_start;
	unsigned int ckpt_inflight;

	/* frozen memtable은 active memtable에서는 빠졌지만 SSTable 색인에
	 * 아직 등록되지 않은 순간이 있다. 그 때 latest-map을 만들면
	 * 최신 overwrite를 놓칠 수 있으므로 진행 중 flush가 있으면 미루고,
	 * GC가 끝날 때까지 새 memtable swap을 막는다. */
	spin_lock_irq(&c->lock);
	free_at_start = gc_count_free_zones(c->zp);
	ckpt_inflight = c->wal_ckpt_inflight;
	if (c->wal_ckpt_inflight > 0) {
		deferred = true;
	} else {
		c->gc_active = true;
	}
	spin_unlock_irq(&c->lock);
	if (deferred) {
		DMINFO("gc: deferred while checkpoint/flush is in flight (inflight=%u, free_zones=%u)",
		       ckpt_inflight, free_at_start);
		return;
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
		spin_unlock_irq(&c->lock);
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
	spin_lock_irq(&c->lock);
	if (reclaimed)
		c->gc_no_progress = 0;
	else if (c->gc_no_progress < UINT_MAX)
		c->gc_no_progress++;
	c->gc_active = false;
	free_at_start = gc_count_free_zones(c->zp);
	spin_unlock_irq(&c->lock);
	DMINFO("gc: worker finished (reclaimed=%u, free_zones=%u, no_progress=%u)",
	       reclaimed, free_at_start, READ_ONCE(c->gc_no_progress));
	/* WAL 공간 부족으로 보류된 foreground bio가 있으면 100ms 타이머를
	 * 기다리지 않고 방금 회수한 공간을 즉시 사용하게 한다. */
	if (reclaimed && READ_ONCE(c->wal_pending_count))
		mod_delayed_work(zns_wq, &c->wal_batch_work, 0);
}

/* zns_wal_reclaim_wq 워커 — 온전히 flush된 WAL zone 회수. 대상 3조건(전부 락 하):
 *   1) wal_gen[z] < wal_durable_split_gen — 레코드가 전부 durable SSTable에 반영됨 (replay 불필요). 같은 gen(split 걸친 zone)은 제외.
 *   2) z != active_zone[WAL] — 쓰는 중인 zone 금지.
 *   3) dispatch_wp[z] == 시작+wp — 발행 안 된 배정분 없어야 함.
 * 정상 운영 중 WAL zone을 읽는 경로가 없어 회수 vs 읽기 race는 없다. reset 성공
 * 후에만 mark_free. process context 전용. */
static void wal_reclaim_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(work, struct zns_base_c, wal_reclaim_work);
	unsigned int rounds;

	for (rounds = 0; rounds < c->zp->nr_zones; rounds++) {
		unsigned int z, victim = ZONE_NONE;
		u64 durable;

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
			break;  /* 더 회수할 WAL zone 없음 */

		if (zone_reset_hw(c, victim)) {
			DMERR("wal reclaim: hardware zone reset failed for zone %u — leaked until reboot", victim);
			break;  /* 같은 zone을 무한 재선정하지 않도록 중단 */
		}

		spin_lock_irq(&c->lock);
		zone_pool_mark_free(c->zp, victim);
		spin_unlock_irq(&c->lock);
		DMINFO("wal reclaim: reclaimed zone %u (gen < %llu)",
		       victim, (unsigned long long)durable);
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

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		ti->error = "out of memory";
		return -ENOMEM;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &c->dev);
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
	/* ti->len is the host-visible logical address range.  The allocator must
	 * nevertheless manage every physical zone of the underlying ZNS device;
	 * otherwise reducing ti->len for over-provisioning also hides exactly the
	 * reserve zones that WAL, compaction and GC need. */
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
	/* dispatch_wp는 절대 섹터 좌표계(zone_pool_alloc의 phys와 동일) — 각
	 * zone의 절대 시작 섹터로 초기화해야 한다(0으로 일괄 두면 zone 0 외
	 * 모든 zone의 첫 쓰기가 영원히 대기열에 갇힘). */
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
	atomic_set(&c->foreground_writes, 0);

	/* zone 스캔으로 크래시/재로드 이전 상태 복원(zone_tag[]/wp[] + WAL
	 * checkpoint 위치 파악 후 replay + 살아있는 SSTable들 색인 재구성).
	 * 완전히 새 디바이스면 모든 zone의 wp가 0이라 사실상 no-op. */
	{
		struct recovery_scan_ctx rctx = { .c = c, .nr_wal_zones = 0, .nr_sstable_zones = 0 };
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
	ti->num_discard_bios = 0;
	/* 읽기 bio가 pin한 zone을 완료 시 unpin하려면 per-bio에 그 zone을 실어둔다(zns_read_pin). */
	ti->per_io_data_size = sizeof(struct zns_read_pin);
	/* 매핑 단위(4KB)보다 큰 bio는 DM core가 애초에 쪼개서 .map()에 보내게 함 */
	ti->max_io_len = BLOCK_SECTORS;

	/* 복구로 durable 지점을 seed했으면(replay_wal_zones), 재부팅 전에 이미 온전히 flush됐던 옛 WAL zone들을 지금 한 번 회수해준다 —
	 * 이후 쓰기가 없어도 새어나가지 않도록. */
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
	flush_delayed_work(&c->wal_batch_work);

	/* worker가 SSTable/checkpoint 비동기 체인을 시작하게 한 뒤, 마지막 callback의
	 * flush_chain_end까지 기다려 c와 frozen memtable의 수명을 보장한다. */
	flush_workqueue(zns_flush_wq);
	wait_event(c->flush_waitq, READ_ONCE(c->wal_ckpt_inflight) == 0);

	/* 아직 큐잉/실행 중인 compaction이 있으면 완전히 끝날 때까지 기다린다(안 그러면 아래에서 c를 해제한 뒤 use-after-free) */
	cancel_work_sync(&c->compaction_work);
	cancel_work_sync(&c->gc_work);
	cancel_work_sync(&c->wal_reclaim_work);

	dm_put_device(ti, c->dev);
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

	pin->zone = -1;  /* 읽기 pin 없음이 기본 — READ 경로에서 필요 시에만 설정 */

	/* 순수 flush 요청(nr=0)은 특정 LBA와 무관하므로 매핑 로직을 타면 안 됨.
	 * bi_sector에 남은 임의값을 lba로 오인해 엉뚱한 매핑을 덮어쓰게 된다. */
	if (nr == 0) {
		bool wal_busy;

		spin_lock_irq(&c->lock);
		wal_busy = c->wal_batch_busy || !list_empty(&c->wal_pending) ||
			atomic_read(&c->foreground_writes) > 0;
		spin_unlock_irq(&c->lock);
		if (wal_busy) {
			mod_delayed_work(zns_wq, &c->wal_batch_work, 0);
			return DM_MAPIO_REQUEUE;
		}
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}

	lba = bio->bi_iter.bi_sector;

	/* 매핑 키는 항상 블록(BLOCK_SECTORS) 정렬된 lba — 커널이 블록 정렬 안 된 위치(예: ext4 슈퍼블록 프로브, sector 2부터 2섹터)로 읽을 수 있어서. */
	block_lba = (lba / BLOCK_SECTORS) * BLOCK_SECTORS;
	offset_in_block = lba - block_lba;

	/* 블록 경계를 넘으면 그 블록 끝까지만 처리, 나머지는 DM core가 재분배 */
	if (nr > BLOCK_SECTORS - offset_in_block) {
		dm_accept_partial_bio(bio, BLOCK_SECTORS - offset_in_block);
		nr = BLOCK_SECTORS - offset_in_block;
	}

	switch (bio_op(bio)) {
	case REQ_OP_WRITE: {
		sector_t phys;     // append capacity 예약 위치(실제 phys는 완료 시 반환)
		int ret;
		int new_data_zone;
		struct zns_io_ctx *ctx;

		/* 매핑/WAL/GC의 최소 단위는 4KB다. 부분 write를 그대로
		 * append하면 read-modify-write 없이 나머지 섹터를 잃게 되므로
		 * 조용히 잘못된 mapping을 만드는 대신 명시적으로 거절한다. */
		if (offset_in_block || nr != BLOCK_SECTORS) {
			bio->bi_status = BLK_STS_NOTSUPP;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		ret = zone_pool_alloc_with_gc_retry(c, ZONE_TAG_USER_DATA, nr, &phys, &new_data_zone);
		if (ret == -EAGAIN)
			return DM_MAPIO_REQUEUE;
		if (ret) {
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		ctx = kmalloc(sizeof(*ctx), GFP_NOIO);
		if (!ctx) {
			zone_dispatch_cancel(c, phys, nr);
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		ctx->c = c;
		ctx->orig_bio = bio;
		ctx->lba = block_lba;
		ctx->reserved_phys = phys;
		ctx->reserved_nr = nr;
		atomic_inc(&c->foreground_writes);

		/* 새로 배정받은 zone이 있으면(태그별 최대 1개) 헤더부터 비동기로 써야 재부팅 후 recovery_zone_cb가 zone_tag[]/wp[]를 되찾을 수 있다. */
		ctx->nr_headers = 0;
		if (new_data_zone >= 0) {
			ctx->headers[ctx->nr_headers].zone_id = new_data_zone;
			ctx->headers[ctx->nr_headers].tag = ZONE_TAG_USER_DATA;
			ctx->headers[ctx->nr_headers].gen = 0;  /* WAL 아님 */
			ctx->nr_headers++;
		}
		ctx->header_idx = 0;
		ctx->on_headers_done = submit_data_append_async;

		if (ctx->nr_headers > 0)
			submit_header_async(ctx);
		else
			submit_data_append_async(ctx);

		/* data append의 실제 phys를 알아야 WAL을 만들 수 있어 data가 먼저다. */
		maybe_trigger_gc(c);
		return DM_MAPIO_SUBMITTED;
	}
	case REQ_OP_READ: {
		sector_t phys;
		int found;
		unsigned int nr_sst, actual_nr, nmatch, i;
		struct sstable_info *candidates;
		struct sstable_read_ctx *rctx;

		spin_lock_irq(&c->lock);
		found = mapping_get(c, block_lba, &phys);
		if (found) {
			/* 이 데이터 zone을 pin — REMAPPED로 나간 읽기가 끝날 때까지(end_io)
			 * GC가 이 zone을 reset하지 못하게. 반드시 조회와 같은 락 안에서 pin해야
			 * GC의 "매핑 이동 → drain → reset" 순서와 어긋나지 않는다. */
			pin->zone = zone_of(c->zp, phys);
			zone_read_get(c->zp, pin->zone);
		}
		nr_sst = c->nr_sstables;
		spin_unlock_irq(&c->lock);

		if (found) {
			/* memtable에 있으면 8단계 전과 동일하게 즉시 처리.
			 * SSTable이 하나도 없던 지금까지의 모든 테스트는 항상 이 경로만 타서 동작·성능이 그대로 유지된다. */
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
		memcpy(candidates, c->sstables, actual_nr * sizeof(*candidates));
		/* in-range 후보만 남기고, 각 후보의 SSTable zone을 pin — probe가 그 zone을
		 * 읽는 동안 compaction이 reset하지 못하게. pin은 반드시 스냅샷과 같은 락
		 * 안에서(compaction의 색인 제거와 순서가 맞아야). unpin은 sstable_read_finish. */
		nmatch = 0;
		for (i = 0; i < actual_nr; i++) {
			if (block_lba >= candidates[i].min_lba && block_lba <= candidates[i].max_lba) {
				candidates[nmatch] = candidates[i];
				zone_read_get(c->zp, zone_of(c->zp, candidates[nmatch].phys));
				nmatch++;
			}
		}
		spin_unlock_irq(&c->lock);

		if (nmatch == 0) {
			kfree(candidates);
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

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
		rctx->idx = 0;
		sstable_read_next_candidate(rctx);

		/* SSTable 조회는 비동기 — sstable_read_finish가 원본 bio를 마무리 */
		return DM_MAPIO_SUBMITTED;
	}
	}

	/* Student work goes here: translate random writes into sequential ones. */
	bio_set_dev(bio, c->dev->bdev);
	return DM_MAPIO_REMAPPED;
}

static void zns_base_status(struct dm_target *ti, status_type_t type, unsigned int status_flas, char *result, unsigned int maxlen) {
	struct zns_base_c *c = ti->private;
	unsigned int i;
	unsigned int sz =0;

	if (type == STATUSTYPE_INFO) {
		DMEMIT("zones=%u zone_sectors=%llu active[user_data]=%u wp:",
		       c->zp->nr_zones, (unsigned long long)c->zp->zone_sectors,
		       c->zp->active_zone[ZONE_TAG_USER_DATA]);
		for (i = 0; i < c->zp->nr_zones; i++)
			DMEMIT(" %llu/%llu", (unsigned long long)c->zp->wp[i], (unsigned long long)c->zp->zone_sectors);
	} else {
		DMEMIT("%s", c->dev->name);
	}
}

/* 1:1 mapping, so ti->begin is passed straight through. A non-identity
 * mapping would need to translate args->next_sector. */
// static int zns_base_report_zones(struct dm_target *ti,
// 				 struct dm_report_zones_args *args,
// 				 unsigned int nr_zones)
// {
// 	struct zns_base_c *c = ti->private;

// 	return dm_report_zones(c->dev->bdev, ti->begin,
// 			       args->next_sector, args, nr_zones);
// }

/* DM_TARGET_ZONED_HM is just a capability flag. Without this callback the
 * underlying device's chunk_sectors and zoned attributes never propagate up
 * to the DM queue, and blkzone fails with "unable to determine zone size". */
// static int zns_base_iterate_devices(struct dm_target *ti,
// 				    iterate_devices_callout_fn fn, void *data)
// {
// 	struct zns_base_c *c = ti->private;

// 	return fn(ti, c->dev, 0, ti->len, data);
// }

/* 읽기 bio 완료 훅 — .map()이 그 읽기가 걸친 zone을 pin(zone_read_get)했으면 여기서
 * put한다. 이 완료 추적 덕에 GC/compaction이 zone reset 전 진행 중인 읽기가 끝나길
 * 기다릴 수 있다(zone_wait_reads_drained). pin 안 한 bio(쓰기/flush/미스)는
 * per_io.zone == -1이라 no-op. REMAPPED·SUBMITTED 둘 다 이 훅이 불린다. */
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

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 1, 0},
	.features        = DM_TARGET_ZONED_HM,
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	.status			 = zns_base_status,
	.end_io          = zns_base_end_io,
	// .report_zones    = zns_base_report_zones, //위쪽엔 zone이 없으므로
	// .iterate_devices = zns_base_iterate_devices, // 위쪽으로 zone 속성 전파를 막음
};

static int __init zns_base_init(void)
{
	int ret;

	/* WQ_MEM_RECLAIM: 이 워크큐가 막히면 그 위 I/O가 영원히 안 풀리므로
	 * 메모리 회수 경로에서도 최소 진행 보장 필요. WQ_UNBOUND + max_active=1:
	 * bound 워크큐의 CPU별 풀이 순서를 뒤바꿀 가능성을 없애려던 최초
	 * 대응(실제 수정은 zone_dispatch_gate) — 무해해서 유지. */
	zns_wq = alloc_workqueue("dm_zns_base", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!zns_wq) {
		DMERR("failed to allocate workqueue");
		return -ENOMEM;
	}

	/* compaction은 bio 제출 경로가 아니라 WQ_MEM_RECLAIM 불필요 —
	 * max_active=1로 device당 compaction이 겹쳐 돌지 않게 제한 */
	zns_compaction_wq = alloc_workqueue("dm_zns_base_compaction", 0, 1);
	if (!zns_compaction_wq) {
		DMERR("failed to allocate compaction workqueue");
		destroy_workqueue(zns_wq);
		return -ENOMEM;
	}

	/* GC도 compaction과 같은 이유로 WQ_MEM_RECLAIM 불필요, max_active=1 */
	zns_gc_wq = alloc_workqueue("dm_zns_base_gc", 0, 1);
	if (!zns_gc_wq) {
		DMERR("failed to allocate gc workqueue");
		destroy_workqueue(zns_compaction_wq);
		destroy_workqueue(zns_wq);
		return -ENOMEM;
	}

	/* WAL 회수도 같은 이유로 WQ_MEM_RECLAIM 불필요, max_active=1 */
	zns_wal_reclaim_wq = alloc_workqueue("dm_zns_base_wal_reclaim", 0, 1);
	if (!zns_wal_reclaim_wq) {
		DMERR("failed to allocate wal reclaim workqueue");
		destroy_workqueue(zns_gc_wq);
		destroy_workqueue(zns_compaction_wq);
		destroy_workqueue(zns_wq);
		return -ENOMEM;
	}

	zns_flush_wq = alloc_workqueue("dm_zns_base_flush",
				       WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!zns_flush_wq) {
		DMERR("failed to allocate flush workqueue");
		destroy_workqueue(zns_wal_reclaim_wq);
		destroy_workqueue(zns_gc_wq);
		destroy_workqueue(zns_compaction_wq);
		destroy_workqueue(zns_wq);
		return -ENOMEM;
	}

	ret = dm_register_target(&zns_base_target);
	if (ret < 0) {
		DMERR("target registration failed: %d", ret);
		destroy_workqueue(zns_flush_wq);
		destroy_workqueue(zns_wal_reclaim_wq);
		destroy_workqueue(zns_gc_wq);
		destroy_workqueue(zns_compaction_wq);
		destroy_workqueue(zns_wq);
	} else {
		DMINFO("target registered");
	}
	return ret;
}

static void __exit zns_base_exit(void)
{
	dm_unregister_target(&zns_base_target);
	destroy_workqueue(zns_flush_wq);
	destroy_workqueue(zns_wal_reclaim_wq);
	destroy_workqueue(zns_gc_wq);
	destroy_workqueue(zns_compaction_wq);
	destroy_workqueue(zns_wq);
	DMINFO("target unregistered");
}

module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS base dm target (zoned-aware pass-through scaffold)");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
