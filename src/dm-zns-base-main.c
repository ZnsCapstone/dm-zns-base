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
static unsigned int gc_low_watermark = 2;
module_param(gc_low_watermark, uint, 0444);
MODULE_PARM_DESC(gc_low_watermark, "free zone count that triggers GC");

/* free zone 중 마지막 이만큼은 GC_DATA 태그에만 내준다 — GC 자신도 재배치할
 * zone이 필요한데 USER_DATA/WAL이 여유 zone을 전부 먹으면 GC가 회수를 못 해
 * 자기순환 데드락에 빠진다. 이 예비분으로 GC는 항상 재배치할 곳을 확보. */
static unsigned int gc_reserved_zones = 2;
module_param(gc_reserved_zones, uint, 0444);
MODULE_PARM_DESC(gc_reserved_zones, "free zones reserved exclusively for GC relocation");

// zone pool
struct zone_pool {
	sector_t zone_sectors;
	unsigned int nr_zones;
	enum zone_tag *zone_tag; 					// zone_tag[zone_id] — 이 zone이 지금 뭘로 쓰이는지
	sector_t *wp; 								// zone_id별 "할당된"(아직 실제로 안 나갔을 수도 있는) 섹터 수
	sector_t *dispatch_wp; 						// zone_id별 "실제로 디바이스에 나간" 섹터 수 — wp와 분리한 이유는
									// 아래 dispatch_waiters 설명 참고
	struct list_head *dispatch_waiters; 			// zone_id별: 아직 자기 차례가 안 된 쓰기 대기열
	unsigned int *invalid_count; 				// zone_id별 무효(죽은) 섹터 수 — GC(M3) victim 선정 근거
	unsigned int *sstable_live_count; 			// zone_id별 그 zone에 저장된 살아있는 SSTable 개수 — 0이 되면 compaction이 zone을 회수 가능
	unsigned int active_zone[ZONE_TAG_COUNT]; 	// 태그별 현재 활성 zone
	u64 *wal_gen; 							// WAL zone에 한해서만 의미 있는 배정 순번(generation) — replay 순서 판정용
	u64 wal_next_gen; 						// 다음 WAL zone에 부여할 generation (단조 증가, c->lock 하에 증가)
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

struct zns_base_c {
	struct dm_dev *dev;

	sector_t 		nr_sectors;
	struct zone_pool *zp;
	struct skiplist *memtable;  // LBA -> phys 매핑 (M1의 map[] flat array를 대체)
	u64              next_seq_no;  // 다음 flush에 붙일 SSTable 세대 번호

	struct sstable_info *sstables;  // 살아있는 SSTable 색인 (append-only, krealloc으로 증가)
	unsigned int nr_sstables;
	unsigned int sstables_cap;      // sstables 배열의 현재 용량

	struct work_struct compaction_work;  // compaction_wq에 큐잉되는 백그라운드 작업
	struct work_struct gc_work;          // gc_wq에 큐잉되는 백그라운드 작업

	spinlock_t 		lock;
};

// SSTable - 오래된 걸 압축해서 Zone에 색인포함으로 보관
#define SSTABLE_MAGIC 0x53535442U  /* "SSTB" */

/* flush 시 정렬 순서로 이 형태 그대로 기록 — 고정 16B라 offset=i*16으로
 * binary search 가능(8단계에서 사용 예정). */
struct sstable_record {
	u64 lba;
	u64 phys;
};

/* SSTable 한 세대의 첫 섹터에 오는 헤더. min/max_lba는 8단계 조회 필터용. */
struct sstable_header {
	u32 magic;
	u32 reserved;
	u64 seq_no;
	u64 record_count;
	u64 min_lba;
	u64 max_lba;
};

/* 살아있는 SSTable 하나를 메모리에서 빠르게 찾기 위한 색인 — 헤더 내용을
 * 그대로 캐싱한 것. phys는 그 SSTable 자신의 헤더가 시작하는 절대 섹터(zone
 * 하나에 여러 SSTable이 순서대로 쌓일 수 있어 zone_id만으론 부족). */
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

/* 고정 크기(512/4096에 나머지 없이 나눠떨어짐). 체크포인트는 스왑 시점의
 * WAL 스트림 위치를 (split_gen, split_off) = (활성 WAL zone의 generation,
 * 그 zone 내 다음 쓰기 오프셋)로 남긴다 — replay가 이 지점보다 앞선 PUT은
 * 이미 SSTable에 반영됐다고 보고 건너뛴다. 절대 섹터가 아니라 논리 순번을
 * 쓰는 이유는 WAL zone 회수 후 zone_id 순서 ≠ 기록 순서가 되기 때문. */
struct wal_record {
	u32 type;
	u32 reserved;
	union {
        struct { u64 lba; u64 phys; } put;
        struct { u64 seq_no; u64 split_gen; u64 split_off; } checkpoint;
    };
};

/* zone_pool_alloc이 새로 배정한 zone 하나 — 헤더를 비동기로 써줘야 할 대상 */
struct pending_header {
	unsigned int zone_id;
	unsigned int tag;  /* enum zone_tag */
	u64 gen;           /* WAL zone이면 그 generation, 아니면 0 */
};

/* .map() WRITE 경로와 SSTable flush 경로가 "(필요하면) 헤더 쓰기 → 본 작업"
 * 패턴을 공유하므로 헤더 체인 상태 + 각자 전용 필드를 한 구조체에 담는다.
 * on_headers_done이 헤더 완료 후 이어갈 다음 단계(submit_wal_async 또는
 * submit_sstable_write_async)를 가리킨다. */
struct zns_io_ctx {
	struct zns_base_c *c;

	/* 헤더 체인 공용 */
	struct pending_header headers[2];  /* 최대 2개(데이터/WAL/SSTable 태그 zone 중 이번에 새로 배정된 것들) */
	int nr_headers;
	int header_idx;
	void *hdr_buf;              /* submit_header_async가 할당, header_write_done이 해제 */
	void (*on_headers_done)(struct zns_io_ctx *ctx);

	/* WAL 레코드 쓰기 공용 — WRITE 경로에선 PUT, flush 경로 뒷단에선
	 * CHECKPOINT를 쓸 때 재사용 */
	sector_t wal_phys;
	struct wal_record *wal_buf;

	/* WRITE 경로 전용 */
	struct bio *orig_bio;
	u64 lba;
	sector_t phys;

	/* SSTable flush 경로 전용 */
	struct skiplist *old_memtable;
	void *sstable_buf;
	sector_t sstable_phys;
	sector_t sstable_nr_sectors;
	u64 checkpoint_seq;
	u64 checkpoint_split_gen;   /* 스왑 시점 활성 WAL zone의 generation */
	sector_t checkpoint_split_off;  /* 그 zone 내 다음 WAL 쓰기 오프셋 */
};

/* zone이 새로 태그를 배정받을 때 섹터 0에 기록하는 헤더 — zone_tag[]/wp[]는
 * 메모리에만 있어서 크래시 시 사라지므로, 재insmod 후 복원의 유일한 단서. */
#define ZONE_HEADER_MAGIC 0x5A4E5348U  /* "ZNSH" */

struct zone_header {
	u32 magic;
	u32 tag;  /* enum zone_tag */
	u64 gen;  /* WAL zone의 배정 순번(generation) — replay 순서 판정용.
		   * WAL 외 태그에선 무의미(0). WAL zone을 회수하기 시작하면
		   * zone_id 순서가 곧 기록 순서라는 보장이 깨지므로, 물리 위치
		   * 대신 이 논리 순번으로 "누가 더 최근 WAL인지"를 판단한다. */
};

/* FREE 태그 zone 하나를 찾아 tag로 배정. wp를 1로 시작하는 이유: 섹터 0은
 * 태그 헤더용으로 예약(실제 헤더는 submit_header_async가 비동기로 씀).
 *
 * GC_DATA가 아닌 태그는 free zone이 gc_reserved_zones개 이하로 남으면
 * 더 이상 못 가져간다 — 이 마지막 예비분은 GC 자신의 재배치용으로만
 * 남겨둔다(위 gc_reserved_zones 설명 참고, 자기순환 데드락 방지). */
static unsigned int zone_pool_acquire_free(struct zone_pool *zp, enum zone_tag tag)
{
	unsigned int z;

	if (tag != ZONE_TAG_GC_DATA) {
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

/* 태그 tag로 nr 섹터 쓸 물리 위치를 반환, 필요하면 zone 새로 배정.
 * new_zone_out에 이번에 새로 배정된 zone id를 담아준다(없으면 -1) —
 * 호출자가 락 밖에서 그 zone에 태그 헤더를 써야 하기 때문. */
static int zone_pool_alloc(struct zone_pool *zp, enum zone_tag tag, sector_t nr,
			    sector_t *phys_out, int *new_zone_out)
{
	unsigned int z = zp->active_zone[tag];

	if (new_zone_out)
		*new_zone_out = -1;

	if (z == ZONE_NONE) {
		z = zone_pool_acquire_free(zp, tag);
		if (z == ZONE_NONE)
			return -ENOSPC;
		zp->active_zone[tag] = z;
		if (tag == ZONE_TAG_WAL)
			zp->wal_gen[z] = zp->wal_next_gen++;
		if (new_zone_out)
			*new_zone_out = z;
	}

	while (zp->wp[z] + nr > zp->zone_sectors) {
		z = zone_pool_acquire_free(zp, tag);
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

/* zone_reset_hw가 성공한 뒤에만 호출해야 한다 — 순서를 뒤집으면 아직 실제로는
 * 안 비워진 zone에 새로 써도 된다고 착각해 write pointer 위반이 난다.
 * 호출자가 c->lock을 쥐고 있어야 한다. */
static void zone_pool_mark_free(struct zone_pool *zp, unsigned int zone_id)
{
	/* 대기열이 안 비었다는 건 "아직 안 나간 쓰기가 남은 zone을 회수하려 한다"는
	 * 뜻 — 회수 대상 판정(dispatch_wp == zone 시작 + wp)이 어딘가에서 빠졌다는
	 * 신호이고, 그대로 두면 그 waiter들이 옛 좌표계의 phys를 든 채 다음
	 * 세대의 dispatch_wp와 비교되어 영원히 안 풀리거나 엉뚱한 자리에 발사된다. */
	if (!list_empty(&zp->dispatch_waiters[zone_id]))
		DMERR("zone %u reclaimed while dispatch waiters remain — those writes will never complete (reclaim guard missed)",
		      zone_id);

	zp->wp[zone_id] = 0;
	/* dispatch_wp는 절대 섹터 좌표계 — 그 zone 자신의 절대 시작 섹터로
	 * 되돌려야 한다(ctr()의 초기화와 같은 이유, zone_dispatch_gate 참고). */
	zp->dispatch_wp[zone_id] = (sector_t)zone_id * zp->zone_sectors;
	zp->zone_tag[zone_id] = ZONE_TAG_FREE;
	zp->invalid_count[zone_id] = 0;
	zp->sstable_live_count[zone_id] = 0;
}

static inline unsigned int zone_of(struct zone_pool *zp, sector_t phys)
{
	return phys / zp->zone_sectors;
}

/* zone_id의 dispatch_wp가 phys에 도달하면 fire(arg) 호출, 아니면 대기열에
 * 넣어뒀다가 앞선 쓰기가 dispatch될 때 자동 방출 — "배정 순서"(wp[])와
 * 실제 디바이스 발행 순서가 달라 zone 순차쓰기가 깨지는 걸 막는다.
 * 반환 -ENOMEM이면 호출자가 arg를 직접 실패 처리. 락은 이 함수가 직접 잠근다. */
static int zone_dispatch_gate(struct zns_base_c *c, sector_t phys, sector_t nr,
				void (*fire)(void *arg), void *arg)
{
	unsigned int zone_id = zone_of(c->zp, phys);
	struct dispatch_waiter *w, *tmp;
	bool my_turn;

	spin_lock_irq(&c->lock);
	my_turn = (phys == c->zp->dispatch_wp[zone_id]);
	if (!my_turn) {
		/* GFP_ATOMIC인 이유: atomic context(bio 완료 콜백)에서도
		 * 이 경로를 타기 때문 — 이 파일의 다른 모든 atomic-reachable
		 * 할당과 같은 원칙. */
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

	/* 내 차례 — dispatch_wp 전진과 fire 호출을 반드시 이 락 안에서 함께
	 * 한다(드레인되는 대기 항목들도 마찬가지) — 둘을 락 밖에서 분리하면
	 * 다른 CPU의 동시 호출이 먼저 fire()를 불러버리는 레이스가 생김
	 * (fire 구현들은 전부 sleep 안 해서 스핀락 안에서 안전). */
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

/* zone_pool_alloc으로 배정은 받았지만 결국 못 내보내게 된 phys를 게이트에서
 * 건너뛰게 한다. 이걸 안 하면 dispatch_wp가 그 자리에서 영원히 멈춰, 그 zone에
 * 오는 이후 모든 쓰기가 대기열에 갇혀 절대 완료되지 않는다(= 그 zone을 쓰는
 * 모든 I/O가 영구 행). 배정 후 실패하는 모든 경로에서 반드시 호출할 것.
 * 자기 차례면 즉시 dispatch_wp를 넘기고, 아직 앞선 쓰기가 안 나갔으면
 * "발사돼도 아무것도 안 하는" 대기자로 줄에 서서 순서를 이어준다. */
static void zone_dispatch_cancel(struct zns_base_c *c, sector_t phys, sector_t nr)
{
	if (zone_dispatch_gate(c, phys, nr, dispatch_fire_noop, NULL))
		DMERR("zone dispatch: out of memory cancelling reservation (zone %u, phys %llu) — this zone's dispatch queue will stall",
		      zone_of(c->zp, phys), (unsigned long long)phys);
}

/* zone_dispatch_gate의 블로킹 버전 — compaction처럼 동기(submit_bio_wait)
 * 코드에서 쓴다. 자기 차례가 될 때까지 재워뒀다가 깨어나면 리턴, 그 다음
 * 호출자가 직접 실제 쓰기를 제출하면 순서가 보장된다. process context
 * 전용(compaction_work_fn에서만 호출) — 재우는 대기라 atomic context 금지. */
static void zone_dispatch_wait_turn(struct zns_base_c *c, sector_t phys, sector_t nr)
{
	struct completion done;

	init_completion(&done);
	if (zone_dispatch_gate(c, phys, nr, dispatch_fire_complete, &done)) {
		/* 큐잉 자체가 실패(OOM) — 영원히 블로킹할 수는 없으니 순서
		 * 보장을 포기하고 진행(극단적 메모리 부족 상황에서만 발생) */
		DMERR("zone dispatch: out of memory queuing compaction write (zone %u, phys %llu) — proceeding without ordering guarantee",
		      zone_of(c->zp, phys), (unsigned long long)phys);
		return;
	}
	wait_for_completion(&done);
}

/* zone_id 섹터 0을 동기적으로 읽어 헤더를 얻는다. ctr()(process context)
 * 전용 — c->lock도 없는 초기화 단계에서만 호출되므로 submit_bio_wait이 안전. */
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
		c->zp->invalid_count[zone_of(c->zp, old_phys)]++;
	return 0;
}

/* memtable에서만 조회. .map()의 READ 분기가 이게 miss일 때만 SSTable도
 * 훑는다(아래 sstable_read_* 체인) — 여긴 순수 memtable 조회 그대로 둔다.
 * 호출자가 c->lock을 쥐고 있다고 가정. */
static int mapping_get(struct zns_base_c *c, u64 lba, u64 *phys_out)
{
	return skiplist_lookup(c->memtable, lba, phys_out);
}

/* c->sstables[]에 SSTable 하나를 등록, 필요하면 배열을 2배로 키운다.
 * 그 zone의 sstable_live_count도 같이 올린다(compaction의 zone 회수 판단
 * 근거). 호출자가 c->lock을 쥐고 있어야 한다. gfp는 호출 컨텍스트에 맞게. */
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

/* c->sstables[]에서 phys로 항목을 찾아 제거(compaction이 병합해서 못 쓰게
 * 된 옛 SSTable 정리용) — 순서 유지 불필요라 마지막 원소와 바꿔치기하는
 * O(1) 제거. 호출자가 c->lock을 쥐고 있어야 한다. */
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

/* zone_id의 섹터 start부터 wp까지 WAL 레코드를 순서대로 읽어 fn(fn_ctx, rec,
 * sector)로 하나씩 넘긴다 — checkpoint 탐색과 실제 replay가 공유하는 공용
 * 순회자. ctr() 전용 process context라 락 없이 접근, submit_bio_wait 안전. */
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
		DMERR("WAL scan: out of memory, skipping zone %u from sector %llu",
		      zone_id, (unsigned long long)start);
		return;
	}
	page = virt_to_page(buf);

	while (cur < wp) {
		sector_t chunk = wp - cur;
		sector_t max_chunk = PAGE_SIZE / 512;
		struct bio *bio;
		sector_t i;
		int ret;

		if (chunk > max_chunk)
			chunk = max_chunk;

		bio = bio_alloc(GFP_KERNEL, 1);
		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = (sector_t)zone_id * c->zp->zone_sectors + cur;
		bio->bi_opf = REQ_OP_READ;
		bio_add_page(bio, page, chunk * 512, 0);

		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret) {
			DMERR("WAL scan: read failed at zone %u sector %llu",
			      zone_id, (unsigned long long)cur);
			break;
		}

		for (i = 0; i < chunk; i++)
			fn(fn_ctx, (struct wal_record *)(buf + i * 512), cur + i);

		cur += chunk;
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

	if (rec->type == WAL_REC_CHECKPOINT) {
		st->found = 1;
		st->seq_no = rec->checkpoint.seq_no;
		st->split_gen = rec->checkpoint.split_gen;
		st->split_off = rec->checkpoint.split_off;
	}
}

/* wal_zone_for_each_record 콜백 — PUT을 memtable에 적용(replay 본체) */
static void wal_replay_cb(void *fn_ctx, struct wal_record *rec, sector_t sector)
{
	struct zns_base_c *c = fn_ctx;

	if (rec->type == WAL_REC_PUT) {
		u64 old_phys;

		if (skiplist_upsert(c->memtable, rec->put.lba, rec->put.phys, &old_phys) == 1)
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

	/* generation 오름차순 정렬 — count는 zone 수라 작아서 삽입정렬로 충분. */
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
}

/* zone_id 안에 순서대로 쌓여있는 SSTable들을 전부 찾아 c->sstables[]에
 * 등록한다 — 헤더를 읽을 때마다 record_count로 다음 SSTable 시작 위치를
 * 계산해 이어감. ctr() 전용 process context. */
static void scan_sstable_zone(struct zns_base_c *c, unsigned int zone_id, sector_t wp)
{
	sector_t cur = 1;  /* 섹터 0은 zone 태그 헤더 */

	while (cur < wp) {
		sector_t phys = (sector_t)zone_id * c->zp->zone_sectors + cur;
		struct sstable_header hdr;
		sector_t data_sectors;

		if (read_sstable_header(c, phys, &hdr) || hdr.magic != SSTABLE_MAGIC)
			break;  /* 손상되었거나 여기서 SSTable들이 끝남 */

		if (sstable_register(c, phys, hdr.seq_no, hdr.record_count,
				       hdr.min_lba, hdr.max_lba, GFP_KERNEL))
			DMERR("SSTable scan: out of memory registering zone %u sector %llu",
			      zone_id, (unsigned long long)cur);

		data_sectors = round_up(hdr.record_count * sizeof(struct sstable_record), 512) / 512;
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

/* blkdev_report_zones가 zone마다 호출 — 하드웨어 wp로 쓰인 적 있는 zone인지
 * 판단하고, 태그 헤더가 있으면 zone_tag[]/wp[]/dispatch_wp[]를 복원한다.
 * WAL/SSTable zone은 id만 모아뒀다가 스캔이 끝난 뒤 한 번에 처리한다. */
static int recovery_zone_cb(struct blk_zone *zone, unsigned int idx, void *data)
{
	struct recovery_scan_ctx *rctx = data;
	struct zns_base_c *c = rctx->c;
	struct zone_header hdr;
	sector_t real_wp;
	int ret;

	if (zone->wp <= zone->start)
		return 0;  /* 한 번도 안 쓰인 zone */

	ret = read_zone_header(c, idx, &hdr);
	if (ret || hdr.magic != ZONE_HEADER_MAGIC)
		return 0;  /* 헤더 없음/손상 — 이 zone은 복원 대상 아님 */

	real_wp = zone->wp - zone->start;  /* zone 기준 상대 wp */

	c->zp->zone_tag[idx] = hdr.tag;
	c->zp->wp[idx] = real_wp;
	/* dispatch_wp도 실제 하드웨어 wp까지 따라잡혀 있어야 한다 — 안 그러면
	 * 재부팅 후 이 zone의 첫 쓰기가 dispatch_wp의 초기값(zone 시작)을
	 * 영원히 기다리게 된다. */
	c->zp->dispatch_wp[idx] = (sector_t)idx * c->zp->zone_sectors + real_wp;

	/* 아직 안 꽉 찬 zone이면 그 태그의 활성 zone으로 채택 (태그별 non-full
	 * zone은 유일하므로 zone_id 순서와 무관하게 이 판정이 옳다). */
	if (real_wp < c->zp->zone_sectors)
		c->zp->active_zone[hdr.tag] = idx;

	if (hdr.tag == ZONE_TAG_WAL) {
		/* generation 복원 — 새 WAL zone이 이어서 더 큰 gen을 받도록
		 * wal_next_gen도 max(gen)+1로 끌어올린다. */
		c->zp->wal_gen[idx] = hdr.gen;
		if (hdr.gen >= c->zp->wal_next_gen)
			c->zp->wal_next_gen = hdr.gen + 1;
		rctx->wal_zones[rctx->nr_wal_zones++] = idx;
	} else if (hdr.tag == ZONE_TAG_SSTABLE) {
		rctx->sstable_zones[rctx->nr_sstable_zones++] = idx;
	}

	return 0;
}

static void header_write_done(struct bio *bio);
static void wal_put_done(struct bio *wal_bio);
static void sstable_flush_done(struct bio *bio);
static void submit_checkpoint_async(struct zns_io_ctx *ctx);
static void checkpoint_write_done(struct bio *bio);
static void flush_memtable_async(struct zns_base_c *c, struct skiplist *old_memtable,
				   u64 seq_no, u64 split_gen, sector_t split_off);

/* WAL PUT 레코드(512B, 앞 32B만 유효) 비동기 제출. process/atomic context
 * 양쪽에서 불리므로 GFP_ATOMIC 필수(GFP_NOIO도 sleep 가능해 안전하지 않음). */
static void submit_wal_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	struct wal_record *rec;
	struct bio *wal_bio;
	struct page *page;
	/* bio_endio 이후엔 못 읽으므로 미리 확보 — 실패 경로에서 이 데이터
	 * 예약분을 취소할 때 필요하다. */
	sector_t data_nr = bio_sectors(ctx->orig_bio);

	/* 섹터 하나(512B) 전체를 kzalloc — 레코드는 앞 32바이트뿐이지만
	 * 디바이스에 512B보다 작은 단위로는 쓸 수 없어 나머지는 패딩. */
	rec = kzalloc(512, GFP_ATOMIC);
	if (!rec)
		goto fail;
	rec->type = WAL_REC_PUT;
	rec->put.lba = ctx->lba;
	rec->put.phys = ctx->phys;
	ctx->wal_buf = rec;

	wal_bio = bio_alloc(GFP_ATOMIC, 1);
	if (!wal_bio) {
		kfree(rec);
		goto fail;
	}
	bio_set_dev(wal_bio, c->dev->bdev);
	wal_bio->bi_iter.bi_sector = ctx->wal_phys;
	wal_bio->bi_opf = REQ_OP_WRITE;
	page = virt_to_page(rec);
	bio_add_page(wal_bio, page, 512, offset_in_page(rec));
	wal_bio->bi_end_io = wal_put_done;
	wal_bio->bi_private = ctx;
	zone_dispatch_write(c, ctx->wal_phys, 1, wal_bio);
	return;

fail:
	/* 이미 배정받은 WAL/데이터 phys를 게이트에서 건너뛰게 해줘야 해당
	 * zone들의 dispatch가 여기서 영구히 멈추지 않는다. */
	zone_dispatch_cancel(c, ctx->wal_phys, 1);
	zone_dispatch_cancel(c, ctx->phys, data_nr);
	ctx->orig_bio->bi_status = BLK_STS_RESOURCE;
	bio_endio(ctx->orig_bio);
	kfree(ctx);
}

/* ctx->headers[ctx->header_idx]의 zone 태그 헤더를 비동기로 제출. .map()
 * 안에서 블로킹하면 bio 스태킹 때문에 자기 자신을 기다리는 데드락이 되므로
 * WAL과 같은 완전 비동기 콜백 체인 — GFP_ATOMIC 이유도 submit_wal_async와 동일. */
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
	hdr->magic = ZONE_HEADER_MAGIC;
	hdr->tag = h->tag;
	hdr->gen = h->gen;  /* WAL zone이면 generation, 아니면 0 */
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
	/* 헤더는 항상 그 zone의 오프셋 0 — 같은 zone의 다른 WAL/data write보다
	 * 먼저 dispatch돼야 하므로 zone_dispatch_write를 거친다. */
	zone_dispatch_write(c, bio->bi_iter.bi_sector, 1, bio);
	return;

skip_header:
	/* 헤더 하나를 못 써도 이 zone의 복원(replay)만 못 하게 될 뿐이라 다음
	 * 단계로 그냥 진행한다 — 단, 섹터 0은 zone_pool_acquire_free가 이미
	 * 예약(wp=1)해뒀으므로 반드시 취소해줘야 한다. 안 그러면 dispatch_wp가
	 * zone 시작에서 못 움직여 그 zone 전체가 처음부터 영구히 막힌다. */
	zone_dispatch_cancel(c, (sector_t)h->zone_id * c->zp->zone_sectors, 1);
	ctx->header_idx++;
	if (ctx->header_idx < ctx->nr_headers)
		submit_header_async(ctx);
	else
		ctx->on_headers_done(ctx);
}

/* 헤더 하나가 durable하게 쓰인 뒤 호출. 남은 헤더가 있으면 이어서,
 * 없으면 ctx->on_headers_done으로 넘어간다. */
static void header_write_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	blk_status_t status = bio->bi_status;

	kfree(ctx->hdr_buf);
	bio_put(bio);

	if (status)
		DMERR("zone header write failed (zone %u, tag %u): recovery for this zone will be broken",
		      ctx->headers[ctx->header_idx].zone_id, ctx->headers[ctx->header_idx].tag);

	ctx->header_idx++;
	if (ctx->header_idx < ctx->nr_headers)
		submit_header_async(ctx);
	else
		ctx->on_headers_done(ctx);
}

/* WAL PUT record가 durable하게 쓰인 뒤 호출. 여기서 비로소 memtable에
 * 반영하고 원본 데이터 bio를 제출한다(WAL이 데이터보다 먼저 durable해야 함). */
static void wal_put_done(struct bio *wal_bio)
{
	struct zns_io_ctx *ctx = wal_bio->bi_private;
	struct zns_base_c *c = ctx->c;
	struct bio *orig = ctx->orig_bio;
	blk_status_t wal_status = wal_bio->bi_status;
	u64 lba = ctx->lba;
	sector_t phys = ctx->phys;
	struct skiplist *flushed_memtable = NULL;
	u64 flushed_seq = 0;
	u64 flushed_split_gen = 0;
	sector_t flushed_split_off = 0;
	int ret;

	kfree(ctx->wal_buf);
	bio_put(wal_bio);
	kfree(ctx);

	if (wal_status) {
		orig->bi_status = wal_status;
		bio_endio(orig);
		return;
	}

	spin_lock_irq(&c->lock);
	ret = mapping_put(c, lba, phys);
	if (!ret && c->memtable->count >= flush_threshold) {
		/* memtable 교체는 이 락 안에서 — skiplist_init도 GFP_ATOMIC이라
		 * atomic 컨텍스트에서 불러도 안전하다. */
		struct skiplist *new_memtable = kzalloc(sizeof(*new_memtable), GFP_ATOMIC);

		if (new_memtable && skiplist_init(new_memtable) == 0) {
			unsigned int wal_zone = c->zp->active_zone[ZONE_TAG_WAL];

			flushed_memtable = c->memtable;
			flushed_seq = c->next_seq_no++;
			/* 이 순간 이후 WAL에 쌓이는 레코드는 새 memtable 몫 —
			 * replay가 "스왑 시점 기준 이전/이후"로 정확히 나누도록
			 * 체크포인트에 이 위치를 (generation, 오프셋)로 실어둔다.
			 * 절대 섹터가 아니라 논리 순번을 쓰는 이유는 WAL zone 회수
			 * 후 zone_id 순서 ≠ 기록 순서가 되기 때문. */
			flushed_split_gen = c->zp->wal_gen[wal_zone];
			flushed_split_off = c->zp->wp[wal_zone];
			c->memtable = new_memtable;
		} else {
			/* 못 만들면 이번 flush는 건너뛴다 — 다음 put에서 다시 시도됨 */
			kfree(new_memtable);
		}
	}
	spin_unlock_irq(&c->lock);

	if (ret) {
		orig->bi_status = BLK_STS_RESOURCE;
		bio_endio(orig);
		return;
	}

	/* flush는 fire-and-forget — WAL에 이미 durable하게 기록됐으므로 데이터
	 * bio가 flush 완료를 기다릴 필요 없다. */
	if (flushed_memtable)
		flush_memtable_async(c, flushed_memtable, flushed_seq,
				     flushed_split_gen, flushed_split_off);

	orig->bi_iter.bi_sector = phys;
	bio_set_dev(orig, c->dev->bdev);
	zone_dispatch_write(c, phys, bio_sectors(orig), orig);
}

/* SSTable 데이터가 durable하게 쓰인 뒤 호출. 성공했으면 checkpoint 체인으로
 * 넘겨(진짜 마지막 정리는 checkpoint_write_done) 다음 재부팅 때 이 세대의
 * WAL을 건너뛸 수 있게 한다. 실패했으면 checkpoint 없이 바로 정리. */
static void sstable_flush_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	struct zns_base_c *c = ctx->c;
	blk_status_t status = bio->bi_status;
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
		rret = sstable_register(c, ctx->sstable_phys, hdr->seq_no, hdr->record_count,
					  hdr->min_lba, hdr->max_lba, GFP_ATOMIC);
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

	kfree(ctx->sstable_buf);
	bio_put(bio);

	if (status) {
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
		kfree(ctx);
		return;
	}

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_WAL, 1, &wal_phys, &new_wal_zone);
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("checkpoint alloc failed (%d, seq=%llu): replay will just do extra work next time, no data lost",
		      ret, (unsigned long long)ctx->checkpoint_seq);
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
		kfree(ctx);
		return;
	}

	ctx->wal_phys = wal_phys;
	ctx->nr_headers = 0;
	if (new_wal_zone >= 0) {
		ctx->headers[ctx->nr_headers].zone_id = new_wal_zone;
		ctx->headers[ctx->nr_headers].tag = ZONE_TAG_WAL;
		ctx->nr_headers++;
	}
	ctx->header_idx = 0;
	ctx->on_headers_done = submit_checkpoint_async;

	if (ctx->nr_headers > 0)
		submit_header_async(ctx);
	else
		submit_checkpoint_async(ctx);
}

/* CHECKPOINT(seq_no, split_gen, split_off) 레코드를 비동기로 append — flush
 * 체인의 마지막 단계, GFP_ATOMIC 원칙은 submit_wal_async와 동일. */
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
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
		kfree(ctx);
		return;
	}
	rec->type = WAL_REC_CHECKPOINT;
	rec->checkpoint.seq_no = ctx->checkpoint_seq;
	rec->checkpoint.split_gen = ctx->checkpoint_split_gen;
	rec->checkpoint.split_off = ctx->checkpoint_split_off;
	ctx->wal_buf = rec;

	bio = bio_alloc(GFP_ATOMIC, 1);
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = ctx->wal_phys;
	bio->bi_opf = REQ_OP_WRITE;
	page = virt_to_page(rec);
	bio_add_page(bio, page, 512, offset_in_page(rec));
	bio->bi_end_io = checkpoint_write_done;
	bio->bi_private = ctx;
	zone_dispatch_write(c, ctx->wal_phys, 1, bio);
}

/* CHECKPOINT 기록이 끝난 뒤(성공하든 실패하든) flush 체인의 진짜 마지막 —
 * 여기서 비로소 old_memtable을 완전히 버린다. */
static void checkpoint_write_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	blk_status_t status = bio->bi_status;

	kfree(ctx->wal_buf);
	bio_put(bio);

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
}

/* ctx->sstable_buf(헤더+레코드, 직렬화 완료)를 PAGE_SIZE 단위로 잘라 비동기
 * 기록. 마지막 page는 남은 바이트만큼만 붙여야 bio 총 크기가 정확히
 * 일치한다(안 그러면 다음 flush가 그 틈/겹침을 밟는다). */
static void submit_sstable_write_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	struct bio *bio;
	size_t total_bytes = ctx->sstable_nr_sectors * 512;
	unsigned int nr_pages = DIV_ROUND_UP(total_bytes, PAGE_SIZE);
	size_t remaining = total_bytes;
	unsigned int i;

	bio = bio_alloc(GFP_ATOMIC, nr_pages);
	if (!bio) {
		DMERR("SSTable flush: bio_alloc failed, dropping this generation (data remains in WAL)");
		kfree(ctx->sstable_buf);
		skiplist_destroy(ctx->old_memtable);
		kfree(ctx->old_memtable);
		kfree(ctx);
		return;
	}
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = ctx->sstable_phys;
	bio->bi_opf = REQ_OP_WRITE;

	for (i = 0; i < nr_pages; i++) {
		struct page *page = virt_to_page((char *)ctx->sstable_buf + i * PAGE_SIZE);
		size_t len = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;

		bio_add_page(bio, page, len, 0);
		remaining -= len;
	}

	bio->bi_end_io = sstable_flush_done;
	bio->bi_private = ctx;
	zone_dispatch_write(c, ctx->sstable_phys, ctx->sstable_nr_sectors, bio);
}

/* memtable 하나를 SSTable 한 세대로 직렬화해서 zone에 기록. wal_put_done
 * (atomic context)에서 호출되므로 전부 GFP_ATOMIC. old_memtable은 이미
 * c->memtable에서 떼어져 나온 상태라 락 없이 순회해도 안전. split_gen/off는
 * ctx에 실어 뒷단 체크포인트 레코드에 쓴다. */
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
		return;
	}

	/* 512(헤더 1섹터) + 레코드들(16B*count)을 섹터 단위로 올림 */
	data_bytes = 512 + round_up(old_memtable->count * sizeof(struct sstable_record), 512);
	nr_sectors = data_bytes / 512;
	alloc_bytes = round_up(data_bytes, PAGE_SIZE);

	/* kzalloc은 물리적 연속 메모리 필요 — flush_threshold가 커지면 vmalloc +
	 * 분할 bio로 바꿔야 할 수 있음(지금 검증 규모에선 문제 없음). */
	buf = kzalloc(alloc_bytes, GFP_ATOMIC);
	if (!buf) {
		DMERR("SSTable flush: out of memory (seq=%llu), dropping this generation (data remains in WAL)",
		      (unsigned long long)seq_no);
		skiplist_destroy(old_memtable);
		kfree(old_memtable);
		return;
	}

	hdr = buf;
	hdr->magic = SSTABLE_MAGIC;
	hdr->seq_no = seq_no;
	hdr->record_count = old_memtable->count;

	rec = (struct sstable_record *)((char *)buf + 512);
	node = old_memtable->head->forward[0];
	hdr->min_lba = node ? node->lba : 0;
	hdr->max_lba = 0;
	while (node) {
		rec[i].lba = node->lba;
		rec[i].phys = node->phys;
		hdr->max_lba = node->lba;
		node = node->forward[0];
		i++;
	}

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors, &phys, &new_sstable_zone);
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("SSTable flush: zone_pool_alloc failed (%d, seq=%llu), dropping this generation (data remains in WAL)",
		      ret, (unsigned long long)seq_no);
		kfree(buf);
		skiplist_destroy(old_memtable);
		kfree(old_memtable);
		return;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx) {
		DMERR("SSTable flush: out of memory building ctx (seq=%llu), dropping this generation (data remains in WAL)",
		      (unsigned long long)seq_no);
		kfree(buf);
		skiplist_destroy(old_memtable);
		kfree(old_memtable);
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

/* buf(record_count개의 정렬된 sstable_record)에서 lba를 이진 탐색.
 * flush_memtable_async가 정렬 순서로 기록해뒀으므로 성립. */
static int sstable_binary_search(void *buf, u64 record_count, u64 lba, u64 *phys_out)
{
	struct sstable_record *recs = buf;
	u64 lo = 0, hi = record_count;

	while (lo < hi) {
		u64 mid = lo + (hi - lo) / 2;

		if (recs[mid].lba == lba) {
			*phys_out = recs[mid].phys;
			return 1;
		} else if (recs[mid].lba < lba) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return 0;
}

/* .map()의 READ 분기가 memtable을 못 찾았을 때 이어받는 비동기 체인의
 * 컨텍스트. candidates는 .map()이 미리 걸러둔 SSTable 스냅샷 — 같은 lba가
 * 여러 SSTable에 걸쳐 있을 수 있어 첫 hit에서 안 멈추고 끝까지 훑어
 * seq_no가 가장 큰 것(best_seq/best_phys)으로 갱신해나간다. */
struct sstable_read_ctx {
	struct zns_base_c *c;
	struct bio *orig_bio;
	u64 lba;
	sector_t offset_in_block;
	struct sstable_info *candidates;
	unsigned int nr_candidates;
	unsigned int idx;
	void *buf;
	int best_found;
	u64 best_seq;
	sector_t best_phys;
};

static void sstable_read_candidate_done(struct bio *bio);

/* 후보를 다 훑었으면 결과를 원본 bio에 반영하고 체인을 끝낸다 */
static void sstable_read_finish(struct sstable_read_ctx *rctx)
{
	struct bio *orig = rctx->orig_bio;

	if (rctx->best_found) {
		orig->bi_iter.bi_sector = rctx->best_phys + rctx->offset_in_block;
		bio_set_dev(orig, rctx->c->dev->bdev);
		submit_bio_deferred(orig);
	} else {
		/* 후보 SSTable들의 min/max_lba 범위엔 들었지만 실제 레코드는
		 * 없었던 경우 — 한 번도 안 쓰인 블록과 같은 취급(zero-fill) */
		zero_fill_bio(orig);
		bio_endio(orig);
	}
	kfree(rctx->candidates);
	kfree(rctx);
}

/* candidates[idx]의 레코드 전체를 비동기로 읽어온다. process/atomic
 * context 양쪽에서 불리므로 GFP_ATOMIC 원칙은 이 파일의 다른 비동기
 * 함수들과 동일. */
static void sstable_read_next_candidate(struct sstable_read_ctx *rctx)
{
	struct sstable_info *si;
	struct bio *bio;
	sector_t nr_sectors;
	size_t total_bytes;
	size_t alloc_bytes;
	unsigned int nr_pages;
	size_t remaining;
	unsigned int i;

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

	nr_sectors = round_up(si->record_count * sizeof(struct sstable_record), 512) / 512;
	total_bytes = (size_t)nr_sectors * 512;
	alloc_bytes = round_up(total_bytes, PAGE_SIZE);

	rctx->buf = kzalloc(alloc_bytes, GFP_ATOMIC);
	if (!rctx->buf) {
		/* 이 후보를 못 읽으면 그냥 다음 후보로 — 정확도가 조금 떨어질 수
		 * 있지만(이 후보에 더 최신 값이 있었을 가능성) 메모리 부족 상황의
		 * 열화로 수용 */
		rctx->idx++;
		sstable_read_next_candidate(rctx);
		return;
	}

	nr_pages = DIV_ROUND_UP(alloc_bytes, PAGE_SIZE);
	bio = bio_alloc(GFP_ATOMIC, nr_pages);
	if (!bio) {
		kfree(rctx->buf);
		rctx->buf = NULL;
		rctx->idx++;
		sstable_read_next_candidate(rctx);
		return;
	}
	bio_set_dev(bio, rctx->c->dev->bdev);
	bio->bi_iter.bi_sector = si->phys + 1;  /* SSTable 자신의 헤더(1섹터) 다음이 레코드 */
	bio->bi_opf = REQ_OP_READ;

	remaining = total_bytes;
	for (i = 0; i < nr_pages; i++) {
		struct page *page = virt_to_page((char *)rctx->buf + i * PAGE_SIZE);
		size_t len = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;

		bio_add_page(bio, page, len, 0);
		remaining -= len;
	}

	bio->bi_end_io = sstable_read_candidate_done;
	bio->bi_private = rctx;
	submit_bio_deferred(bio);
}

/* 후보 하나의 레코드 읽기가 끝난 뒤 호출 — 이진 탐색해서 hit이면(그리고
 * 지금까지 중 seq_no가 가장 크면) best_*를 갱신하고, 다음 후보로 넘어간다. */
static void sstable_read_candidate_done(struct bio *bio)
{
	struct sstable_read_ctx *rctx = bio->bi_private;
	struct sstable_info *si = &rctx->candidates[rctx->idx];
	blk_status_t status = bio->bi_status;
	u64 phys;

	if (!status && sstable_binary_search(rctx->buf, si->record_count, rctx->lba, &phys)) {
		if (!rctx->best_found || si->seq_no > rctx->best_seq) {
			rctx->best_found = 1;
			rctx->best_seq = si->seq_no;
			rctx->best_phys = phys;
		}
	}

	kfree(rctx->buf);
	rctx->buf = NULL;
	bio_put(bio);

	rctx->idx++;
	sstable_read_next_candidate(rctx);
}

/* compaction 진행 중 victim SSTable 하나에서 읽어온 레코드 배열 + k-way
 * merge 진행 상태(idx) — 각자 이미 lba 정렬돼 있어 merge 시 재정렬 불필요. */
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
 * compaction_work_fn 전용 process context — submit_bio_wait이 안전한
 * 이유가 ctr()과 동일. */
static int compaction_read_source(struct zns_base_c *c, struct sstable_info *victim,
				    struct compaction_source *src)
{
	size_t total_bytes = victim->record_count * sizeof(struct sstable_record);
	size_t alloc_bytes = round_up(total_bytes, PAGE_SIZE);
	unsigned int nr_pages = DIV_ROUND_UP(alloc_bytes, PAGE_SIZE);
	struct bio *bio;
	size_t remaining;
	unsigned int i;
	int ret;

	src->recs = kzalloc(alloc_bytes, GFP_KERNEL);
	if (!src->recs)
		return -ENOMEM;

	bio = bio_alloc(GFP_KERNEL, nr_pages);
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = victim->phys + 1;  /* 헤더(1섹터) 다음이 레코드 */
	bio->bi_opf = REQ_OP_READ;

	remaining = total_bytes;
	for (i = 0; i < nr_pages; i++) {
		struct page *page = virt_to_page((char *)src->recs + i * PAGE_SIZE);
		size_t len = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;

		bio_add_page(bio, page, len, 0);
		remaining -= len;
	}

	ret = submit_bio_wait(bio);
	bio_put(bio);
	if (ret) {
		kfree(src->recs);
		src->recs = NULL;
		return ret;
	}

	src->count = victim->record_count;
	src->idx = 0;
	src->seq_no = victim->seq_no;
	return 0;
}

/* discarded[]에 쌓인 병합 중 밀려난 옛 데이터 phys들의 invalid_count를
 * 한 번에 반영 — merge를 락 밖에서 다 끝낸 뒤 호출. */
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

/* nr_srcs개의 정렬된 소스를 병합해 out에 쓴다. 같은 lba가 여러 소스에 걸쳐
 * 있으면 seq_no가 가장 높은 것만 남기고, 밀려난 나머지의 phys는
 * discarded_out[]에 쌓아 나중에 invalid_count에 반영한다. out/discarded_out은
 * 호출자가 이미 넉넉히 할당해뒀다고 가정. */
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
			if (min_i < 0 || srcs[s].recs[srcs[s].idx].lba < min_lba) {
				min_i = (int)s;
				min_lba = srcs[s].recs[srcs[s].idx].lba;
			}
		}
		if (min_i < 0)
			break;  /* 모든 소스 소진 */

		/* 그 lba를 지금 가리키는 모든 소스를 확인 — seq_no 최댓값만 채택,
		 * 나머지는 폐기(그 데이터 phys를 invalid_count 대상으로 기록) */
		for (s = 0; s < nr_srcs; s++) {
			if (srcs[s].idx >= srcs[s].count)
				continue;
			if (srcs[s].recs[srcs[s].idx].lba != min_lba)
				continue;

			if (best_src < 0 || srcs[s].seq_no > best_seq) {
				if (best_src >= 0)
					discarded_out[ndisc++] = best_phys;  /* 이전 최선이 밀려남 */
				best_src = (int)s;
				best_seq = srcs[s].seq_no;
				best_phys = srcs[s].recs[srcs[s].idx].phys;
			} else {
				discarded_out[ndisc++] = srcs[s].recs[srcs[s].idx].phys;
			}
			srcs[s].idx++;
		}

		out[out_count].lba = min_lba;
		out[out_count].phys = best_phys;
		out_count++;
	}

	*discarded_count = ndisc;
	return out_count;
}

/* compaction_wq 워커(process context, submit_bio_wait/GFP_KERNEL 사용 가능).
 * 가장 오래된 compaction_k개를 읽어 k-way merge한 뒤 새 SSTable로 동기
 * 기록하고, 그게 durable해진 다음에야 옛 색인 제거 + 하드웨어 reset —
 * 이 순서 덕분에 어느 지점에서 크래시가 나도 안전. */
static void compaction_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(work, struct zns_base_c, compaction_work);
	struct sstable_info *snapshot = NULL;
	unsigned int snap_count = 0;
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
	snap_count = c->nr_sstables;
	snapshot = kmalloc_array(snap_count, sizeof(*snapshot), GFP_ATOMIC);
	if (!snapshot) {
		spin_unlock_irq(&c->lock);
		DMERR("compaction: out of memory snapshotting SSTable index, will retry on next trigger");
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

	merged = kzalloc(round_up(total_input_records * sizeof(struct sstable_record), 512), GFP_KERNEL);
	discarded = kmalloc_array(total_input_records ? total_input_records : 1, sizeof(*discarded), GFP_KERNEL);
	if (!merged || !discarded) {
		DMERR("compaction: out of memory allocating merge buffers, aborting this run");
		goto out_free_sources;
	}

	merged_count = merge_sstable_sources(srcs, k, merged, discarded, &discarded_count);
	apply_discarded_invalid_counts(c, discarded, discarded_count);

	if (merged_count == 0) {
		/* 이론상 안 생김(입력 SSTable들은 항상 count>0으로만 만들어짐) —
		 * 방어적으로만 처리 */
		DMERR("compaction: merge produced zero records, aborting this run");
		goto out_free_sources;
	}

	merged_min_lba = merged[0].lba;
	merged_max_lba = merged[merged_count - 1].lba;

	data_bytes = 512 + round_up(merged_count * sizeof(struct sstable_record), 512);
	nr_sectors = data_bytes / 512;
	alloc_bytes = round_up(data_bytes, PAGE_SIZE);

	out_buf = kzalloc(alloc_bytes, GFP_KERNEL);
	if (!out_buf) {
		DMERR("compaction: out of memory serializing merged SSTable, aborting this run");
		goto out_free_sources;
	}

	spin_lock_irq(&c->lock);
	out_seq_no = c->next_seq_no++;
	spin_unlock_irq(&c->lock);

	out_hdr = out_buf;
	out_hdr->magic = SSTABLE_MAGIC;
	out_hdr->seq_no = out_seq_no;
	out_hdr->record_count = merged_count;
	out_hdr->min_lba = merged_min_lba;
	out_hdr->max_lba = merged_max_lba;

	out_rec = (struct sstable_record *)((char *)out_buf + 512);
	memcpy(out_rec, merged, merged_count * sizeof(struct sstable_record));

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_SSTABLE, nr_sectors, &new_phys, &new_zone);
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("compaction: zone_pool_alloc failed (%d), aborting this run — old SSTables remain valid", ret);
		goto out_free_out_buf;
	}

	if (new_zone >= 0) {
		struct zone_header *zhdr = kzalloc(512, GFP_KERNEL);

		if (!zhdr) {
			DMERR("compaction: out of memory writing zone header (zone %d): recovery for this zone will be broken", new_zone);
		} else {
			struct bio *hbio = bio_alloc(GFP_KERNEL, 1);

			zhdr->magic = ZONE_HEADER_MAGIC;
			zhdr->tag = ZONE_TAG_SSTABLE;
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
				DMERR("compaction: zone header write failed (zone %d): recovery for this zone will be broken", new_zone);
		}
	}

	/* 병합된 SSTable 데이터를 실제로 durable하게 기록 */
	{
		unsigned int nr_pages = DIV_ROUND_UP(alloc_bytes, PAGE_SIZE);
		size_t remaining = data_bytes;
		struct bio *dbio = bio_alloc(GFP_KERNEL, nr_pages);
		unsigned int p;

		bio_set_dev(dbio, c->dev->bdev);
		dbio->bi_iter.bi_sector = new_phys;
		dbio->bi_opf = REQ_OP_WRITE;
		for (p = 0; p < nr_pages; p++) {
			struct page *page = virt_to_page((char *)out_buf + p * PAGE_SIZE);
			size_t len = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;

			bio_add_page(dbio, page, len, 0);
			remaining -= len;
		}
		zone_dispatch_wait_turn(c, new_phys, nr_sectors);
		ret = submit_bio_wait(dbio);
		bio_put(dbio);
	}
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
	kfree(out_buf);
out_free_sources:
	for (i = 0; i < k; i++)
		kfree(srcs[i].recs);
	kfree(srcs);
	kfree(merged);
	kfree(discarded);
out_free_snapshot:
	kfree(snapshot);
}

/* ============================================================================
 * GC (10단계) — 가득 찬 데이터 zone 중 무효 비율이 가장 높은 것을 회수
 * ============================================================================ */

/* [데이터 흐름] GC가 victim에서 살아있는 lba를 찾을 때 (lba, 지금 phys)만
 * 담아두는 스냅샷 원소 — memtable/SSTable 스캔 결과를 공용으로 담는다. */
struct gc_candidate {
	u64 lba;
	sector_t phys;
};

/* [락] 호출자가 c->lock을 쥐고 있어야 한다. */
static unsigned int gc_count_free_zones(struct zone_pool *zp)
{
	unsigned int z, count = 0;

	for (z = 0; z < zp->nr_zones; z++)
		if (zp->zone_tag[z] == ZONE_TAG_FREE)
			count++;
	return count;
}

/* free zone이 gc_low_watermark 이하로 떨어지면 GC를 큐잉 — zone_pool_alloc
 * 으로 free zone을 소비할 수 있는 지점(.map()의 WRITE 분기)에서 호출. */
static void maybe_trigger_gc(struct zns_base_c *c)
{
	bool should_gc;

	spin_lock_irq(&c->lock);
	should_gc = gc_count_free_zones(c->zp) <= gc_low_watermark;
	spin_unlock_irq(&c->lock);

	if (should_gc)
		queue_work(zns_gc_wq, &c->gc_work);
}

/* 닫힌 데이터 zone(USER_DATA/GC_DATA) 중 invalid_count가 가장 큰 것을
 * victim으로 선정. GC_DATA도 대상 — 재배치한 데이터도 다시 덮어써지면 죽는다.
 * SSTable/WAL zone은 compaction 전담이라 제외.
 * 각 continue 조건의 근거는 report/bugfix-log.md 참고.
 * [락] 이 함수 자체가 잠금. */
static unsigned int gc_select_victim(struct zns_base_c *c)
{
	unsigned int z, victim = ZONE_NONE;
	unsigned int best_invalid = 0;

	spin_lock_irq(&c->lock);
	for (z = 0; z < c->zp->nr_zones; z++) {
		enum zone_tag tag = c->zp->zone_tag[z];

		if (tag != ZONE_TAG_USER_DATA && tag != ZONE_TAG_GC_DATA)
			continue;
		/* 아직 쓰는 중인 활성 zone은 대상 아님. "닫힘"을 wp==zone_sectors로
		 * 판정하면 안 됨 — rollover가 항상 몇 섹터 남기고 넘어가 절대 안 참. */
		if (z == c->zp->active_zone[tag])
			continue;
		if (c->zp->invalid_count[z] == 0)
			continue;  /* 100% live zone은 회수해도 순증가 0이라 제외 */
		/* 아직 발행 안 된 배정분이 남은 zone은 건드리면 안 됨 — .map()은 phys를
		 * 배정만 하고 매핑은 wal_put_done에서야 등록되므로 아래 스캔이 진행 중인
		 * 쓰기를 못 본다. mapping_put이 dispatch보다 먼저 실행되므로 이 등식이
		 * "배정분 전부 발행됨 = 전부 memtable에 보임"을 뜻한다(bugfix-log #9). */
		if (c->zp->dispatch_wp[z] != (sector_t)z * c->zp->zone_sectors + c->zp->wp[z])
			continue;
		if (victim == ZONE_NONE || c->zp->invalid_count[z] > best_invalid) {
			victim = z;
			best_invalid = c->zp->invalid_count[z];
		}
	}
	spin_unlock_irq(&c->lock);
	return victim;
}

/* lba의 현재 진짜 물리 위치를 조회 — memtable 우선, 없으면 SSTable 전체를
 * 훑어 seq_no 최대. GC가 SSTable에서 찾은 후보가 아직 살아있는지(아니면 이미
 * 덮어써진 stale entry인지) 검증할 때 쓴다. 못 찾으면 0, 찾으면 1 반환.
 * [호출 컨텍스트] gc_work_fn 전용 process context(submit_bio_wait 안전). */
static int gc_lookup_current_phys(struct zns_base_c *c, u64 lba, sector_t *phys_out)
{
	struct sstable_info *candidates;
	unsigned int nr_sst, actual_nr, i;
	int best_found = 0;
	u64 best_seq = 0;
	sector_t best_phys = 0;

	spin_lock_irq(&c->lock);
	if (mapping_get(c, lba, phys_out)) {
		spin_unlock_irq(&c->lock);
		return 1;
	}
	nr_sst = c->nr_sstables;
	spin_unlock_irq(&c->lock);

	if (nr_sst == 0)
		return 0;

	candidates = kmalloc_array(nr_sst, sizeof(*candidates), GFP_KERNEL);
	if (!candidates)
		return 0;

	spin_lock_irq(&c->lock);
	actual_nr = min(nr_sst, c->nr_sstables);
	memcpy(candidates, c->sstables, actual_nr * sizeof(*candidates));
	spin_unlock_irq(&c->lock);

	for (i = 0; i < actual_nr; i++) {
		struct sstable_info *si = &candidates[i];
		size_t total_bytes, alloc_bytes;
		unsigned int nr_pages, p;
		struct bio *bio;
		void *buf;
		size_t remaining;
		u64 phys;
		int ret;

		if (lba < si->min_lba || lba > si->max_lba)
			continue;
		if (best_found && si->seq_no < best_seq)
			continue;  /* 이미 더 최신 걸 찾았으면 더 오래된 건 안 읽어도 됨 */

		total_bytes = si->record_count * sizeof(struct sstable_record);
		alloc_bytes = round_up(total_bytes, PAGE_SIZE);
		nr_pages = DIV_ROUND_UP(alloc_bytes, PAGE_SIZE);

		buf = kzalloc(alloc_bytes, GFP_KERNEL);
		if (!buf)
			continue;

		bio = bio_alloc(GFP_KERNEL, nr_pages);
		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = si->phys + 1;
		bio->bi_opf = REQ_OP_READ;
		remaining = total_bytes;
		for (p = 0; p < nr_pages; p++) {
			struct page *page = virt_to_page((char *)buf + p * PAGE_SIZE);
			size_t len = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;

			bio_add_page(bio, page, len, 0);
			remaining -= len;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);

		if (!ret && sstable_binary_search(buf, si->record_count, lba, &phys)) {
			if (!best_found || si->seq_no > best_seq) {
				best_found = 1;
				best_seq = si->seq_no;
				best_phys = phys;
			}
		}
		kfree(buf);
	}

	kfree(candidates);
	if (best_found)
		*phys_out = best_phys;
	return best_found;
}

/* lba의 실제 4KB 데이터를 old_phys에서 읽어 GC 전용 active zone(GC_DATA)에
 * 새로 쓰고 mapping_put으로 갱신. compaction과 달리 매핑 레코드가 아니라 진짜
 * 사용자 데이터를 옮긴다. zone 쓰기는 zone_dispatch_wait_turn으로 순서 게이트를
 * 거친다.
 * [반환값] 0 성공, 음수면 이번 GC 라운드 전체 중단(호출자가 victim zone을
 * reset하지 않음 — 아직 못 옮긴 데이터가 있을 수 있으므로 안전 후퇴).
 * [호출 컨텍스트] gc_work_fn 전용 process context(submit_bio_wait 안전). */
static int gc_relocate_one(struct zns_base_c *c, u64 lba, sector_t old_phys)
{
	void *buf;
	struct bio *bio;
	sector_t new_phys;
	int new_zone = -1;
	int ret;

	buf = kzalloc(BLOCK_SECTORS * 512, GFP_KERNEL);
	if (!buf) {
		DMERR("gc: out of memory relocating lba=%llu, aborting this round",
		      (unsigned long long)lba);
		return -ENOMEM;
	}

	bio = bio_alloc(GFP_KERNEL, 1);
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = old_phys;
	bio->bi_opf = REQ_OP_READ;
	bio_add_page(bio, virt_to_page(buf), BLOCK_SECTORS * 512, 0);
	ret = submit_bio_wait(bio);
	bio_put(bio);
	if (ret) {
		DMERR("gc: failed to read lba=%llu at phys=%llu (%d), aborting this round",
		      (unsigned long long)lba, (unsigned long long)old_phys, ret);
		kfree(buf);
		return ret;
	}

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, ZONE_TAG_GC_DATA, BLOCK_SECTORS, &new_phys, &new_zone);
	spin_unlock_irq(&c->lock);
	if (ret) {
		DMERR("gc: zone_pool_alloc failed (%d) relocating lba=%llu, aborting this round",
		      ret, (unsigned long long)lba);
		kfree(buf);
		return ret;
	}

	if (new_zone >= 0) {
		struct zone_header *zhdr = kzalloc(512, GFP_KERNEL);

		if (!zhdr) {
			DMERR("gc: out of memory writing zone header (zone %d): recovery for this zone will be broken", new_zone);
			/* 예약된 섹터 0을 취소하지 않으면 바로 아래
			 * zone_dispatch_wait_turn(new_phys)이 영원히 블로킹되어
			 * GC 워커 자체가 멈춘다(→ flush_work/cancel_work_sync 연쇄 행). */
			zone_dispatch_cancel(c, (sector_t)new_zone * c->zp->zone_sectors, 1);
		} else {
			struct bio *hbio = bio_alloc(GFP_KERNEL, 1);

			zhdr->magic = ZONE_HEADER_MAGIC;
			zhdr->tag = ZONE_TAG_GC_DATA;
			bio_set_dev(hbio, c->dev->bdev);
			hbio->bi_iter.bi_sector = (sector_t)new_zone * c->zp->zone_sectors;
			hbio->bi_opf = REQ_OP_WRITE;
			bio_add_page(hbio, virt_to_page(zhdr), 512, offset_in_page(zhdr));
			zone_dispatch_wait_turn(c, hbio->bi_iter.bi_sector, 1);
			if (submit_bio_wait(hbio))
				DMERR("gc: zone header write failed (zone %d): recovery for this zone will be broken", new_zone);
			bio_put(hbio);
			kfree(zhdr);
		}
	}

	bio = bio_alloc(GFP_KERNEL, 1);
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = new_phys;
	bio->bi_opf = REQ_OP_WRITE;
	bio_add_page(bio, virt_to_page(buf), BLOCK_SECTORS * 512, 0);
	zone_dispatch_wait_turn(c, new_phys, BLOCK_SECTORS);
	ret = submit_bio_wait(bio);
	bio_put(bio);
	kfree(buf);
	if (ret) {
		DMERR("gc: failed to write relocated data for lba=%llu (%d), aborting this round",
		      (unsigned long long)lba, ret);
		return ret;
	}

	spin_lock_irq(&c->lock);
	ret = mapping_put(c, lba, new_phys);
	spin_unlock_irq(&c->lock);
	if (ret)
		DMERR("gc: failed to update mapping for lba=%llu after relocation (%d), aborting this round",
		      (unsigned long long)lba, ret);
	return ret;
}

/* 10단계 GC 본체 — victim 하나를 회수.
 * victim 선정 → victim zone에 걸친 살아있는 LBA를 memtable(검증 없이 재배치)과
 * SSTable(gc_lookup_current_phys로 stale 아닌 것만)에서 찾아 gc_relocate_one으로
 * 옮김 → 전부 성공했을 때만 zone_reset_hw + zone_pool_mark_free로 회수(compaction과
 * 같은 이유 — 회수는 안전 확인 후 마지막에만).
 * [반환값] true 회수 성공(zone 하나 free), false 회수할 victim 없음 또는 재배치
 * 실패(zone reset 안 함, 안전 후퇴) — gc_work_fn이 이걸로 루프 종료를 판단.
 * [호출 컨텍스트] gc_work_fn(zns_gc_wq 워커) 전용, process context.
 * [락] 스냅샷/조회/커밋 각각 짧게 잠금, 실제 I/O는 락 밖.
 * [미해결 race] 재배치 중 사용자가 같은 lba를 덮어쓰면 GC가 옛 값으로
 * mapping_put을 해 새 값을 되돌릴 수 있다 — 통합 회수-안전 메커니즘 필요
 * (compaction의 동시 읽기 race와 같은 성격, report/bugfix-log.md 참고). */
static bool gc_reclaim_one_victim(struct zns_base_c *c)
{
	unsigned int victim;
	sector_t vstart, vend;
	struct gc_candidate *mt_candidates = NULL;
	unsigned int nr_mt = 0, mt_cap = 0;
	struct sstable_info *snapshot = NULL;
	unsigned int snap_count = 0;
	struct skiplist_node *node;
	unsigned int i;
	bool ok = true;
	bool reclaimed = false;

	victim = gc_select_victim(c);
	if (victim == ZONE_NONE)
		return false;  /* 회수할 만한 zone이 없음 */

	vstart = (sector_t)victim * c->zp->zone_sectors;
	vend = vstart + c->zp->zone_sectors;

	/* 1) memtable 스냅샷 — 순회 중 mapping_put으로 리스트가 바뀌는 걸
	 * 피하려고 (lba, phys)만 배열로 복사해둔 뒤 락을 놓는다. */
	spin_lock_irq(&c->lock);
	for (node = c->memtable->head->forward[0]; node; node = node->forward[0]) {
		if (node->phys < vstart || node->phys >= vend)
			continue;
		if (nr_mt == mt_cap) {
			unsigned int new_cap = mt_cap ? mt_cap * 2 : 16;
			struct gc_candidate *grown = krealloc(mt_candidates,
							new_cap * sizeof(*grown), GFP_ATOMIC);
			if (!grown) {
				ok = false;
				break;
			}
			mt_candidates = grown;
			mt_cap = new_cap;
		}
		mt_candidates[nr_mt].lba = node->lba;
		mt_candidates[nr_mt].phys = node->phys;
		nr_mt++;
	}
	spin_unlock_irq(&c->lock);

	if (!ok) {
		DMERR("gc: out of memory snapshotting memtable, aborting this round");
		goto out_free_mt;
	}

	for (i = 0; i < nr_mt && ok; i++)
		ok = (gc_relocate_one(c, mt_candidates[i].lba, mt_candidates[i].phys) == 0);

	if (!ok) {
		DMERR("gc: relocation failed, aborting this round without resetting victim zone %u", victim);
		goto out_free_mt;
	}

	/* 2) SSTable 스냅샷 — 각자 레코드를 읽어 victim zone에 걸친 것만 후보로,
	 * gc_lookup_current_phys로 "아직도 살아있는 진짜 현재 위치"인지 검증한
	 * 것만 재배치(그렇지 않으면 이미 다른 곳에서 덮어써진 stale entry). */
	spin_lock_irq(&c->lock);
	snap_count = c->nr_sstables;
	if (snap_count) {
		snapshot = kmalloc_array(snap_count, sizeof(*snapshot), GFP_ATOMIC);
		if (!snapshot) {
			spin_unlock_irq(&c->lock);
			DMERR("gc: out of memory snapshotting SSTable index, aborting this round");
			goto out_free_mt;
		}
		memcpy(snapshot, c->sstables, snap_count * sizeof(*snapshot));
	}
	spin_unlock_irq(&c->lock);

	for (i = 0; i < snap_count && ok; i++) {
		struct sstable_info *si = &snapshot[i];
		size_t total_bytes = si->record_count * sizeof(struct sstable_record);
		size_t alloc_bytes = round_up(total_bytes, PAGE_SIZE);
		unsigned int nr_pages = DIV_ROUND_UP(alloc_bytes, PAGE_SIZE);
		struct sstable_record *recs;
		struct bio *bio;
		void *buf;
		size_t remaining;
		unsigned int p, r;
		int ret;

		buf = kzalloc(alloc_bytes, GFP_KERNEL);
		if (!buf) {
			DMERR("gc: out of memory reading SSTable (seq=%llu), skipping its entries this round",
			      (unsigned long long)si->seq_no);
			continue;
		}
		bio = bio_alloc(GFP_KERNEL, nr_pages);
		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = si->phys + 1;
		bio->bi_opf = REQ_OP_READ;
		remaining = total_bytes;
		for (p = 0; p < nr_pages; p++) {
			struct page *page = virt_to_page((char *)buf + p * PAGE_SIZE);
			size_t len = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;

			bio_add_page(bio, page, len, 0);
			remaining -= len;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret) {
			DMERR("gc: failed to read SSTable (seq=%llu, err=%d), skipping its entries this round",
			      (unsigned long long)si->seq_no, ret);
			kfree(buf);
			continue;
		}

		recs = buf;
		for (r = 0; r < si->record_count; r++) {
			sector_t phys = recs[r].phys;
			sector_t cur_phys;

			if (phys < vstart || phys >= vend)
				continue;
			if (!gc_lookup_current_phys(c, recs[r].lba, &cur_phys) || cur_phys != phys)
				continue;  /* 이미 다른 곳에서 덮어써진 stale entry — 재배치 불필요 */
			if (gc_relocate_one(c, recs[r].lba, phys)) {
				ok = false;
				break;
			}
		}
		kfree(buf);
	}

	if (!ok) {
		DMERR("gc: relocation failed, aborting this round without resetting victim zone %u", victim);
		goto out_free_snapshot;
	}

	/* 전부 성공 — victim zone에 더 이상 살아있는 데이터가 없다고 확신할
	 * 수 있으므로 실제로 회수 */
	if (zone_reset_hw(c, victim)) {
		DMERR("gc: hardware zone reset failed for zone %u — zone leaked until manually recovered", victim);
	} else {
		spin_lock_irq(&c->lock);
		zone_pool_mark_free(c->zp, victim);
		spin_unlock_irq(&c->lock);
		DMINFO("gc: reclaimed zone %u (%u memtable entries relocated)", victim, nr_mt);
		reclaimed = true;
	}

out_free_snapshot:
	kfree(snapshot);
out_free_mt:
	kfree(mt_candidates);
	return reclaimed;
}

/* zone_pool_alloc을 시도하고, 여유 zone이 없어 실패하면 GC를 한 라운드 돌려
 * zone을 확보한 뒤 재시도. GC는 백그라운드 비동기라 zone 소진 시점과 회수 시점
 * 사이의 시차 동안 도착한 쓰기가 억울하게 ENOSPC를 맞는 걸 막는다. free zone이
 * 전혀 안 늘면 더 해줄 게 없다는 뜻이라 그 시점의 실패를 반환.
 *
 * GC 실행은 gc_reclaim_one_victim 직접 호출이 아니라 queue_work+flush_work로
 * 우회한다 — .map()의 WRITE 분기가 iodepth만큼 동시에 이 경로를 타므로, 직접
 * 부르면 zns_gc_wq의 max_active=1("GC는 한 번에 하나")이 깨져 여러 스레드가
 * 같은 victim을 동시에 건드린다(report/bugfix-log.md).
 * [호출 컨텍스트] .map() WRITE 분기, process context 전용(flush_work가 블로킹).
 * [반환값] 0 성공, 그 외 zone_pool_alloc 에러코드(보통 -ENOSPC). */
static int zone_pool_alloc_with_gc_retry(struct zns_base_c *c, enum zone_tag tag, sector_t nr,
					   sector_t *phys_out, int *new_zone_out)
{
	int ret;
	unsigned int attempts;
	unsigned int prev_free = UINT_MAX;

	spin_lock_irq(&c->lock);
	ret = zone_pool_alloc(c->zp, tag, nr, phys_out, new_zone_out);
	spin_unlock_irq(&c->lock);

	for (attempts = 0; ret && attempts < c->zp->nr_zones; attempts++) {
		unsigned int free_now;

		queue_work(zns_gc_wq, &c->gc_work);
		flush_work(&c->gc_work);

		spin_lock_irq(&c->lock);
		free_now = gc_count_free_zones(c->zp);
		ret = zone_pool_alloc(c->zp, tag, nr, phys_out, new_zone_out);
		spin_unlock_irq(&c->lock);

		if (ret && free_now == prev_free)
			break;  /* GC를 돌렸는데도 free zone이 안 늘었음 — 더 시도해봤자 소용없음 */
		prev_free = free_now;
	}
	return ret;
}

/* zns_gc_wq 워커 진입점 — free zone이 watermark 이하인 동안 gc_reclaim_one_victim
 * 을 반복(한 트리거당 zone 하나만 회수하면 쓰기 속도를 못 따라잡음). false
 * 반환(victim 없음/재배치 실패) 시 종료.
 * per-trigger 상한(nr_zones)은 방어용 — victim 선정 로직에 결함이 생겨 무한
 * 회수 순환에 빠져도(과거 재부팅으로만 복구된 사건) 워커가 반드시 끝나게 한다.
 * 정상 동작이면 zone당 최대 한 번 회수라 이 상한에 안 걸린다.
 * [호출 컨텍스트] zns_gc_wq 워커, process context. */
static void gc_work_fn(struct work_struct *work)
{
	struct zns_base_c *c = container_of(work, struct zns_base_c, gc_work);
	bool still_low;
	unsigned int rounds;

	for (rounds = 0; rounds < c->zp->nr_zones; rounds++) {
		if (!gc_reclaim_one_victim(c))
			break;
		spin_lock_irq(&c->lock);
		still_low = gc_count_free_zones(c->zp) <= gc_low_watermark;
		spin_unlock_irq(&c->lock);
		if (!still_low)
			break;
	}
	if (rounds == c->zp->nr_zones)
		DMERR("gc: hit the per-trigger round cap (%u) — free zones may still be low, will retry on next trigger",
		      c->zp->nr_zones);
}

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
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
	c->nr_sectors = ti->len;
	c->zp->nr_zones = c->nr_sectors / c->zp->zone_sectors;

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

	spin_lock_init(&c->lock);

	ti->private = c;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 0;
	/* 매핑 단위(4KB)보다 큰 bio는 DM core가 애초에 쪼개서 .map()에 보내게 함 */
	ti->max_io_len = BLOCK_SECTORS;

	DMINFO("ctr: target attached on top of '%s'", argv[0]);
	return 0;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;

	/* 아직 큐잉/실행 중인 compaction이 있으면 완전히 끝날 때까지 기다린다
	 * (안 그러면 아래에서 c를 해제한 뒤 use-after-free) */
	cancel_work_sync(&c->compaction_work);
	cancel_work_sync(&c->gc_work);

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
		/* 정상적이면 dtr() 시점엔 대기열이 비어있어야 하지만, 혹시
		 * 남아있어도 메모리 누수는 막는다 */
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
	kfree(c->zp);
	kfree(c);
	DMINFO("dtr: target detached");
}

static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;

	sector_t lba;
	sector_t nr = bio_sectors(bio);
	sector_t block_lba;
	sector_t offset_in_block;

	/* 순수 flush 요청(nr=0)은 특정 LBA와 무관하므로 매핑 로직을 타면 안 됨 —
	 * bi_sector에 남은 임의값을 lba로 오인해 엉뚱한 매핑을 덮어쓰게 된다. */
	if (nr == 0) {
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}

	lba = bio->bi_iter.bi_sector;

	/* 매핑 키는 항상 블록(BLOCK_SECTORS) 정렬된 lba — 커널이 블록 정렬 안 된
	 * 위치(예: ext4 슈퍼블록 프로브, sector 2부터 2섹터)로 읽을 수 있어서. */
	block_lba = (lba / BLOCK_SECTORS) * BLOCK_SECTORS;
	offset_in_block = lba - block_lba;

	/* 블록 경계를 넘으면 그 블록 끝까지만 처리, 나머지는 DM core가 재분배 */
	if (nr > BLOCK_SECTORS - offset_in_block) {
		dm_accept_partial_bio(bio, BLOCK_SECTORS - offset_in_block);
		nr = BLOCK_SECTORS - offset_in_block;
	}

	switch (bio_op(bio)) {
	case REQ_OP_WRITE: {
		sector_t phys;     // 실제 데이터가 놓일 물리 섹터
		sector_t wal_phys; // WAL 레코드가 놓일 물리 섹터
		int ret;
		int new_data_zone, new_wal_zone;
		struct zns_io_ctx *ctx;

		ret = zone_pool_alloc_with_gc_retry(c, ZONE_TAG_USER_DATA, nr, &phys, &new_data_zone);
		if (ret) {
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		/* WAL은 별개 zone(태그)에서 항상 1섹터짜리 고정 레코드 하나만 append */
		ret = zone_pool_alloc_with_gc_retry(c, ZONE_TAG_WAL, 1, &wal_phys, &new_wal_zone);
		if (ret) {
			/* 데이터 쪽은 이미 배정됐다 — 취소 안 하면 그 zone의
			 * dispatch가 여기서 영구히 멈춘다(zone pool이 빠듯한
			 * GC 테스트에서 실제로 가장 걸리기 쉬운 경로). */
			if (new_data_zone >= 0)
				zone_dispatch_cancel(c, (sector_t)new_data_zone * c->zp->zone_sectors, 1);
			zone_dispatch_cancel(c, phys, nr);
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		ctx = kmalloc(sizeof(*ctx), GFP_NOIO);
		if (!ctx) {
			if (new_data_zone >= 0)
				zone_dispatch_cancel(c, (sector_t)new_data_zone * c->zp->zone_sectors, 1);
			if (new_wal_zone >= 0)
				zone_dispatch_cancel(c, (sector_t)new_wal_zone * c->zp->zone_sectors, 1);
			zone_dispatch_cancel(c, phys, nr);
			zone_dispatch_cancel(c, wal_phys, 1);
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		ctx->c = c;
		ctx->orig_bio = bio;
		ctx->lba = lba;
		ctx->phys = phys;
		ctx->wal_phys = wal_phys;

		/* 새로 배정받은 zone이 있으면(태그별 최대 1개) 헤더부터 비동기로 써야
		 * 재부팅 후 recovery_zone_cb가 zone_tag[]/wp[]를 되찾을 수 있다. */
		ctx->nr_headers = 0;
		if (new_data_zone >= 0) {
			ctx->headers[ctx->nr_headers].zone_id = new_data_zone;
			ctx->headers[ctx->nr_headers].tag = ZONE_TAG_USER_DATA;
			ctx->headers[ctx->nr_headers].gen = 0;  /* WAL 아님 */
			ctx->nr_headers++;
		}
		if (new_wal_zone >= 0) {
			ctx->headers[ctx->nr_headers].zone_id = new_wal_zone;
			ctx->headers[ctx->nr_headers].tag = ZONE_TAG_WAL;
			/* 방금 배정된 zone이라 wal_gen[new_wal_zone]은 안정된 값 */
			ctx->headers[ctx->nr_headers].gen = c->zp->wal_gen[new_wal_zone];
			ctx->nr_headers++;
		}
		ctx->header_idx = 0;
		ctx->on_headers_done = submit_wal_async;

		if (ctx->nr_headers > 0)
			submit_header_async(ctx);
		else
			submit_wal_async(ctx);

		/* 데이터 bio는 아직 안 내보냄 — wal_put_done이 WAL 완료 후 이어서 처리 */
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
		nr_sst = c->nr_sstables;
		spin_unlock_irq(&c->lock);

		if (found) {
			/* memtable에 있으면 8단계 전과 동일하게 즉시 처리 —
			 * SSTable이 하나도 없던 지금까지의 모든 테스트는 항상
			 * 이 경로만 타서 동작·성능이 그대로 유지된다. */
			bio->bi_iter.bi_sector = phys + offset_in_block;
			break;
		}

		if (nr_sst == 0) {
			/* 한 번도 안 쓴 블록 — 표준 thin-provisioning 관례대로 zero-fill */
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		/* SSTable 후보 스냅샷 — c->sstables는 krealloc으로 커질 수
		 * 있어 주소가 바뀔 수 있으므로, 복사는 반드시 락 안에서. */
		candidates = kmalloc_array(nr_sst, sizeof(*candidates), GFP_NOIO);
		if (!candidates) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		spin_lock_irq(&c->lock);
		actual_nr = min(nr_sst, c->nr_sstables);
		memcpy(candidates, c->sstables, actual_nr * sizeof(*candidates));
		spin_unlock_irq(&c->lock);

		nmatch = 0;
		for (i = 0; i < actual_nr; i++) {
			if (block_lba >= candidates[i].min_lba && block_lba <= candidates[i].max_lba)
				candidates[nmatch++] = candidates[i];
		}

		if (nmatch == 0) {
			kfree(candidates);
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		rctx = kzalloc(sizeof(*rctx), GFP_NOIO);
		if (!rctx) {
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

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 1, 0},
	.features        = DM_TARGET_ZONED_HM,
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	.status			 = zns_base_status,
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

	ret = dm_register_target(&zns_base_target);
	if (ret < 0) {
		DMERR("target registration failed: %d", ret);
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
