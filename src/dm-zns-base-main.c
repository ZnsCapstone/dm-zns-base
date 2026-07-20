// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: LSM-tree 기반 매핑 테이블로 랜덤 쓰기를 순차 쓰기로 바꿔주는
 * ZNS(zoned) DM 타깃. 아키텍처 배경은 docs/10-lsm-tree-architecture.md,
 * 구현 단계는 docs/09-lsm-implementation-plan.md, 세부 주석은
 * report/annotated/dm-zns-base-main.annotated.c 참고.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/blkdev.h>
#include <linux/mm.h>

#include "skiplist.h"

#define DM_MSG_PREFIX "zns-base"

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

/* memtable이 이 개수를 넘으면 SSTable로 flush. 기본값이 큰 이유: SSTable
 * 읽기 경로(8단계)가 아직 없어 flush된 세대는 못 읽으므로, 테스트 중
 * 우연히 flush가 발생하면 안 된다. 검증 시 insmod flush_threshold=1000 등으로
 * 낮춰서 확인. */
static unsigned int flush_threshold = 1000000;
module_param(flush_threshold, uint, 0444);
MODULE_PARM_DESC(flush_threshold, "memtable entry count that triggers an SSTable flush");

// zone pool
struct zone_pool {
	sector_t zone_sectors;
	unsigned int nr_zones;
	enum zone_tag *zone_tag; 					// zone_tag[zone_id] — 이 zone이 지금 뭘로 쓰이는지
	sector_t *wp; 								// zone_id별 쓴 섹터 수
	unsigned int *invalid_count; 				// zone_id별 무효(죽은) 섹터 수 — GC(M3) victim 선정 근거
	unsigned int active_zone[ZONE_TAG_COUNT]; 	// 태그별 현재 활성 zone
};

struct zns_base_c {
	struct dm_dev *dev;

	sector_t 		nr_sectors;
	struct zone_pool *zp;
	struct skiplist *memtable;  // LBA -> phys 매핑 (M1의 map[] flat array를 대체)
	u64              next_seq_no;  // 다음 flush에 붙일 SSTable 세대 번호

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

// WAL - 복구용
#define WAL_REC_PUT        1
#define WAL_REC_CHECKPOINT 2

/* 32B 고정(512B/4096B에 나머지 없이 나눠떨어짐). split_phys는 이 체크포인트가
 * 찍힌 "스왑 시점"의 다음 WAL 쓰기 위치(전역 물리 섹터) — replay가 이 값보다
 * 앞선 PUT 레코드는 이미 SSTable에 반영됐다고 보고 건너뛴다. */
struct wal_record {
	u32 type;
	u32 reserved;
	union {
        struct { u64 lba; u64 phys; } put;
        struct { u64 seq_no; u64 split_phys; } checkpoint;
    };
};

/* zone_pool_alloc이 새로 배정한 zone 하나 — 헤더를 비동기로 써줘야 할 대상 */
struct pending_header {
	unsigned int zone_id;
	unsigned int tag;  /* enum zone_tag */
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
	sector_t checkpoint_split_phys;
};

/* zone이 새로 태그를 배정받을 때 섹터 0에 기록하는 헤더 — zone_tag[]/wp[]는
 * 메모리에만 있어서 크래시 시 사라지므로, 재insmod 후 복원의 유일한 단서. */
#define ZONE_HEADER_MAGIC 0x5A4E5348U  /* "ZNSH" */

struct zone_header {
	u32 magic;
	u32 tag;  /* enum zone_tag */
};

/* FREE 태그 zone 하나를 찾아 tag로 배정. wp를 1로 시작하는 이유: 섹터 0은
 * 태그 헤더용으로 예약(실제 헤더는 submit_header_async가 비동기로 씀). */
static unsigned int zone_pool_acquire_free(struct zone_pool *zp, enum zone_tag tag)
{
	unsigned int z;
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
		if (new_zone_out)
			*new_zone_out = z;
	}

	while (zp->wp[z] + nr > zp->zone_sectors) {
		z = zone_pool_acquire_free(zp, tag);
		if (z == ZONE_NONE)
			return -ENOSPC;
		zp->active_zone[tag] = z;
		if (new_zone_out)
			*new_zone_out = z;
	}

	*phys_out = z * zp->zone_sectors + zp->wp[z];
	zp->wp[z] += nr;
	return 0;
}

/* compaction/GC가 다 쓴 zone을 돌려줄 때 호출 */
static void zone_pool_reset(struct zone_pool *zp, unsigned int zone_id)
{
	/* TODO: 실제 ZNS zone reset 명령(REQ_OP_ZONE_RESET)을 underlying에 내려야 함 —
	   지금은 compaction/GC를 아직 안 짰으니 이 부분은 9~10단계에서 채우면 됨 */
	zp->wp[zone_id] = 0;
	zp->zone_tag[zone_id] = ZONE_TAG_FREE;
	zp->invalid_count[zone_id] = 0;
}

static inline unsigned int zone_of(struct zone_pool *zp, sector_t phys)
{
	return phys / zp->zone_sectors;
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

/* memtable에 lba->phys 기록. 기존 lba를 덮어쓴 거라면 그 옛 phys의 zone은
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

/* memtable에서만 조회(8단계 전까지는 매핑 조회의 전부). 호출자가 c->lock을
 * 쥐고 있다고 가정. */
static int mapping_get(struct zns_base_c *c, u64 lba, u64 *phys_out)
{
	return skiplist_lookup(c->memtable, lba, phys_out);
}

/* zone_id의 섹터 start부터 wp까지 WAL 레코드를 순서대로 읽어 fn(fn_ctx, rec,
 * zone_id, sector)로 하나씩 넘긴다. checkpoint 탐색과 실제 replay가 똑같은
 * 청크 read 루프를 공유하기 위한 공용 순회자. ctr() 전용 process context라
 * 락 없이 접근, submit_bio_wait도 안전. */
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
	sector_t split_phys;
};

/* wal_zone_for_each_record 콜백 — CHECKPOINT만 골라 마지막(=가장 최근) 것의
 * seq_no/split_phys를 남긴다. PUT은 적용하지 않고 그냥 지나친다. */
static void checkpoint_scan_cb(void *fn_ctx, struct wal_record *rec, sector_t sector)
{
	struct checkpoint_scan_state *st = fn_ctx;

	if (rec->type == WAL_REC_CHECKPOINT) {
		st->found = 1;
		st->seq_no = rec->checkpoint.seq_no;
		st->split_phys = rec->checkpoint.split_phys;
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

/* wal_zones[]에 모인 WAL zone들을(zone_id 오름차순 = 할당 순서 — GC/compaction이
 * 아직 zone을 회수하지 않아 성립하는 전제, 회수가 생기면 재검토 필요) 재생한다.
 *
 * 1단계: 최신 zone부터 거꾸로 훑어 마지막 CHECKPOINT를 찾으면 즉시 멈춘다 —
 * 그보다 오래된 zone은 읽지도 않는다.
 * 2단계: split_phys(체크포인트가 남긴, 스왑 시점의 전역 물리 섹터) 이전
 * 데이터만 가진 zone은 통째로 스킵, 걸쳐있는 zone은 그 지점부터, 이후 zone은
 * 전부 재생 — "물리적으로 언제 쓰였는지"가 아니라 "스왑 시점 기준 이전/이후"로
 * 정확히 나뉜다(체크포인트 자체는 flush가 끝난 한참 뒤에야 쓰이므로, 스왑 이후
 * 도착한 새 쓰기들의 WAL 레코드가 체크포인트보다 물리적으로 먼저 있을 수 있음
 * — split_phys는 이런 레코드를 건너뛰지 않도록 보장한다). */
static void replay_wal_zones(struct zns_base_c *c, unsigned int *wal_zones, unsigned int count)
{
	sector_t split_phys = 0;
	u64 ckpt_seq = 0;
	int found = 0;
	int i;

	for (i = (int)count - 1; i >= 0; i--) {
		unsigned int zone_id = wal_zones[i];
		struct checkpoint_scan_state st = { 0 };

		wal_zone_for_each_record(c, zone_id, 1, c->zp->wp[zone_id], checkpoint_scan_cb, &st);
		if (st.found) {
			found = 1;
			ckpt_seq = st.seq_no;
			split_phys = st.split_phys;
			break;
		}
	}

	if (found)
		DMINFO("WAL replay: last checkpoint seq=%llu (split_phys=%llu) — skipping WAL entries before it",
		       (unsigned long long)ckpt_seq, (unsigned long long)split_phys);
	else
		DMINFO("WAL replay: no checkpoint found — replaying all %u WAL zone(s) in full", count);

	for (i = 0; i < (int)count; i++) {
		unsigned int zone_id = wal_zones[i];
		sector_t zone_start_phys = (sector_t)zone_id * c->zp->zone_sectors;
		sector_t wp = c->zp->wp[zone_id];
		sector_t start = 1;

		if (zone_start_phys + wp <= split_phys)
			continue;  /* 이 zone 전체가 split 이전 — 통째로 스킵 */

		if (zone_start_phys < split_phys)
			start = split_phys - zone_start_phys;  /* split이 걸쳐있는 zone */

		wal_zone_for_each_record(c, zone_id, start, wp, wal_replay_cb, c);
	}

	/* 복구된 마지막 체크포인트 이후부터 seq_no를 이어가야 다음 flush가
	 * 아직 회수 안 된 옛 SSTable과 seq_no가 겹치지 않는다. */
	c->next_seq_no = found ? ckpt_seq + 1 : 0;
}

struct recovery_scan_ctx {
	struct zns_base_c *c;
	unsigned int *wal_zones;
	unsigned int nr_wal_zones;
};

/* blkdev_report_zones가 zone마다 호출 — 하드웨어 wp로 쓰인 적 있는 zone인지
 * 판단하고, 태그 헤더가 있으면 zone_tag[]/wp[]를 복원한다. WAL zone은 바로
 * replay하지 않고 id만 모아둔다 — replay_wal_zones가 전체 스캔이 끝난 뒤
 * 체크포인트 위치를 먼저 찾아야 하기 때문. */
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

	/* 아직 안 꽉 찬 zone이면 그 태그의 활성 zone으로 채택 */
	if (real_wp < c->zp->zone_sectors)
		c->zp->active_zone[hdr.tag] = idx;

	if (hdr.tag == ZONE_TAG_WAL)
		rctx->wal_zones[rctx->nr_wal_zones++] = idx;

	return 0;
}

static void header_write_done(struct bio *bio);
static void wal_put_done(struct bio *wal_bio);
static void sstable_flush_done(struct bio *bio);
static void submit_checkpoint_async(struct zns_io_ctx *ctx);
static void checkpoint_write_done(struct bio *bio);
static void flush_memtable_async(struct zns_base_c *c, struct skiplist *old_memtable,
				   u64 seq_no, sector_t wal_split_phys);

/* WAL PUT 레코드(512B, 앞 32B만 유효) 비동기 제출. .map()(process context)과
 * header_write_done(atomic context) 양쪽에서 불리므로 GFP_ATOMIC 필수 —
 * GFP_NOIO도 sleep 가능해 atomic 컨텍스트에서 안전하지 않다. */
static void submit_wal_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	struct wal_record *rec;
	struct bio *wal_bio;
	struct page *page;

	/* 섹터 하나(512B) 전체를 kzalloc — 레코드는 앞 32바이트뿐이지만
	 * 디바이스에 512B보다 작은 단위로는 쓸 수 없어 나머지는 패딩. */
	rec = kzalloc(512, GFP_ATOMIC);
	if (!rec) {
		ctx->orig_bio->bi_status = BLK_STS_RESOURCE;
		bio_endio(ctx->orig_bio);
		kfree(ctx);
		return;
	}
	rec->type = WAL_REC_PUT;
	rec->put.lba = ctx->lba;
	rec->put.phys = ctx->phys;
	ctx->wal_buf = rec;

	wal_bio = bio_alloc(GFP_ATOMIC, 1);
	bio_set_dev(wal_bio, c->dev->bdev);
	wal_bio->bi_iter.bi_sector = ctx->wal_phys;
	wal_bio->bi_opf = REQ_OP_WRITE;
	page = virt_to_page(rec);
	bio_add_page(wal_bio, page, 512, offset_in_page(rec));
	wal_bio->bi_end_io = wal_put_done;
	wal_bio->bi_private = ctx;
	submit_bio(wal_bio);
}

/* ctx->headers[ctx->header_idx]의 zone 태그 헤더를 비동기로 제출. .map()
 * 안에서 submit_bio_wait로 블로킹하면 bio 스태킹(current->bio_list) 재귀
 * 제출 방지 때문에 자기 자신을 영원히 기다리는 데드락이 되므로, WAL과 같이
 * 완전 비동기 콜백 체인으로 처리. GFP_ATOMIC인 이유도 submit_wal_async와
 * 동일(process context와 atomic context 양쪽에서 호출). */
static void submit_header_async(struct zns_io_ctx *ctx)
{
	struct zns_base_c *c = ctx->c;
	struct pending_header *h = &ctx->headers[ctx->header_idx];
	struct zone_header *hdr;
	struct bio *bio;
	struct page *page;

	hdr = kzalloc(512, GFP_ATOMIC);
	if (!hdr) {
		/* 헤더 하나를 못 써도 이후 복원(replay)만 못 하게 될 뿐, 지금
		 * 쓰기 자체를 막을 정도는 아니라고 보고 다음 단계로 진행 */
		ctx->header_idx++;
		if (ctx->header_idx < ctx->nr_headers)
			submit_header_async(ctx);
		else
			ctx->on_headers_done(ctx);
		return;
	}
	hdr->magic = ZONE_HEADER_MAGIC;
	hdr->tag = h->tag;
	ctx->hdr_buf = hdr;

	bio = bio_alloc(GFP_ATOMIC, 1);
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = (sector_t)h->zone_id * c->zp->zone_sectors;
	bio->bi_opf = REQ_OP_WRITE;
	page = virt_to_page(hdr);
	bio_add_page(bio, page, 512, offset_in_page(hdr));
	bio->bi_end_io = header_write_done;
	bio->bi_private = ctx;
	submit_bio(bio);
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

/* WAL PUT record가 durable하게 쓰인 뒤 호출. 여기서 비로소 memtable에 반영하고
 * 원본 데이터 bio를 제출한다 — WAL이 데이터보다 먼저 durable해야 하므로. */
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
	sector_t flushed_wal_split = 0;
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
			/* 지금 이 순간부터 WAL에 쌓이는 레코드는 새 memtable
			 * 몫이다 — 이 위치를 체크포인트에 실어야, replay가
			 * "물리적으로 체크포인트보다 먼저 쓰였는지"가 아니라
			 * "이 스왑 시점 기준 이전/이후"로 정확히 나눌 수 있다. */
			flushed_wal_split = (sector_t)wal_zone * c->zp->zone_sectors
					    + c->zp->wp[wal_zone];
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
		flush_memtable_async(c, flushed_memtable, flushed_seq, flushed_wal_split);

	orig->bi_iter.bi_sector = phys;
	bio_set_dev(orig, c->dev->bdev);
	submit_bio(orig);
}

/* SSTable 데이터가 durable하게 쓰인 뒤 호출. 성공했으면 WAL에 CHECKPOINT를
 * 남겨야 다음 재부팅 때 이 세대의 WAL 레코드를 건너뛸 수 있으므로, 여기서
 * old_memtable을 바로 버리지 않고 checkpoint 체인으로 넘긴다 — 진짜 마지막
 * 정리는 checkpoint_write_done에서 한다. SSTable write 자체가 실패했다면
 * (그 세대가 durable하지 않으므로) checkpoint 없이 바로 정리한다. */
static void sstable_flush_done(struct bio *bio)
{
	struct zns_io_ctx *ctx = bio->bi_private;
	struct zns_base_c *c = ctx->c;
	blk_status_t status = bio->bi_status;
	sector_t wal_phys;
	int new_wal_zone;
	int ret;

	if (status)
		DMERR("SSTable flush write failed (seq=%llu): this generation's data is lost from the SSTable, but remains recoverable from WAL replay",
		      (unsigned long long)ctx->checkpoint_seq);

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

/* CHECKPOINT(seq_no, split_phys) 레코드를 비동기로 append — flush 체인의
 * 마지막 단계. GFP_ATOMIC/submit_bio 원칙은 submit_wal_async와 동일. */
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
	rec->checkpoint.split_phys = ctx->checkpoint_split_phys;
	ctx->wal_buf = rec;

	bio = bio_alloc(GFP_ATOMIC, 1);
	bio_set_dev(bio, c->dev->bdev);
	bio->bi_iter.bi_sector = ctx->wal_phys;
	bio->bi_opf = REQ_OP_WRITE;
	page = virt_to_page(rec);
	bio_add_page(bio, page, 512, offset_in_page(rec));
	bio->bi_end_io = checkpoint_write_done;
	bio->bi_private = ctx;
	submit_bio(bio);
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
		DMINFO("checkpoint written (seq=%llu, split_phys=%llu)",
		       (unsigned long long)ctx->checkpoint_seq,
		       (unsigned long long)ctx->checkpoint_split_phys);

	skiplist_destroy(ctx->old_memtable);
	kfree(ctx->old_memtable);
	kfree(ctx);
}

/* ctx->sstable_buf(헤더+레코드, 직렬화 완료)를 PAGE_SIZE 단위로 잘라 비동기
 * 기록. 마지막 page는 남은 바이트만큼만 붙여야 bio 총 크기가
 * sstable_nr_sectors*512와 정확히 일치한다(안 그러면 zone_pool_alloc의 wp
 * 계산과 어긋나 다음 flush가 그 틈을 밟는다). */
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
	submit_bio(bio);
}

/* memtable 하나를 SSTable 한 세대로 직렬화해서 zone에 기록. wal_put_done
 * (atomic context)에서 호출되므로 전부 GFP_ATOMIC. old_memtable은 이미
 * c->memtable에서 떼어져 나온 상태라 락 없이 순회해도 안전. wal_split_phys는
 * 그대로 ctx에 실어 sstable_flush_done 이후의 체크포인트 레코드에 쓴다. */
static void flush_memtable_async(struct zns_base_c *c, struct skiplist *old_memtable,
				   u64 seq_no, sector_t wal_split_phys)
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

	/* kzalloc은 물리적으로 연속된 메모리만 주므로, 큰 flush_threshold에서는
	 * 나중에 vmalloc + 분할 bio로 바꿔야 할 수 있다 — 지금 검증 규모(수천)에선
	 * 문제 없음. */
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
	ctx->checkpoint_split_phys = wal_split_phys;

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
	 * checkpoint 위치 파악 후 replay). 완전히 새 디바이스면 모든 zone의
	 * wp가 0이라 사실상 no-op. */
	{
		struct recovery_scan_ctx rctx = { .c = c, .nr_wal_zones = 0 };

		rctx.wal_zones = kcalloc(c->zp->nr_zones, sizeof(unsigned int), GFP_KERNEL);
		if (!rctx.wal_zones) {
			ti->error = "out of memory (wal_zones scratch)";
			return -ENOMEM;
		}

		ret = blkdev_report_zones(c->dev->bdev, 0, c->zp->nr_zones, recovery_zone_cb, &rctx);
		if (ret < 0)
			DMERR("zone report failed during recovery scan: %d", ret);

		replay_wal_zones(c, rctx.wal_zones, rctx.nr_wal_zones);
		kfree(rctx.wal_zones);
	}

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

	dm_put_device(ti, c->dev);
	skiplist_destroy(c->memtable);
	kfree(c->memtable);
	kfree(c->zp->wp);
	kfree(c->zp->zone_tag);
	kfree(c->zp->invalid_count);
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

		spin_lock_irq(&c->lock);
		ret = zone_pool_alloc(c->zp, ZONE_TAG_USER_DATA, nr, &phys, &new_data_zone);
		if (ret) {
			spin_unlock_irq(&c->lock);
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		/* WAL은 별개 zone(태그)에서 항상 1섹터짜리 고정 레코드 하나만 append */
		ret = zone_pool_alloc(c->zp, ZONE_TAG_WAL, 1, &wal_phys, &new_wal_zone);
		spin_unlock_irq(&c->lock);
		if (ret) {
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		ctx = kmalloc(sizeof(*ctx), GFP_NOIO);
		if (!ctx) {
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
			ctx->nr_headers++;
		}
		if (new_wal_zone >= 0) {
			ctx->headers[ctx->nr_headers].zone_id = new_wal_zone;
			ctx->headers[ctx->nr_headers].tag = ZONE_TAG_WAL;
			ctx->nr_headers++;
		}
		ctx->header_idx = 0;
		ctx->on_headers_done = submit_wal_async;

		if (ctx->nr_headers > 0)
			submit_header_async(ctx);
		else
			submit_wal_async(ctx);

		/* 데이터 bio는 아직 안 내보냄 — wal_put_done이 WAL 완료 후 이어서 처리 */
		return DM_MAPIO_SUBMITTED;
	}
	case REQ_OP_READ: {
		sector_t phys;
		int found;

		spin_lock_irq(&c->lock);
		found = mapping_get(c, block_lba, &phys);
		spin_unlock_irq(&c->lock);
		if (!found) {
			/* 한 번도 안 쓴 블록 — 표준 thin-provisioning 관례대로 zero-fill */
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		bio->bi_iter.bi_sector = phys + offset_in_block;
		break;
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
	int ret = dm_register_target(&zns_base_target);

	if (ret < 0)
		DMERR("target registration failed: %d", ret);
	else
		DMINFO("target registered");
	return ret;
}

static void __exit zns_base_exit(void)
{
	dm_unregister_target(&zns_base_target);
	DMINFO("target unregistered");
}

module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS base dm target (zoned-aware pass-through scaffold)");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
