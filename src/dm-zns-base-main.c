// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: M0 scaffold for the capstone project.
 *
 * Registers a zoned-aware pass-through DM target on top of a host-managed
 * zoned device. The random-to-sequential translation that the project is
 * actually about is left out on purpose — that's the student's work.
 * See docs/07-milestones.md.
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

	spinlock_t 		lock;
};

// SSTable - 오래된 걸 압축해서 Zone에 색인포함으로 보관
struct SSTable {
	u64 lba;
	u64 phys;
};

// WAL - 복구용
#define WAL_REC_PUT        1
#define WAL_REC_CHECKPOINT 2

struct wal_record {
	u32 type;
	u32 reserved;
	union {
        struct { u64 lba; u64 phys; } put;
        struct { u64 seq_no; u64 unused; } checkpoint;
    };
};

/* .map()의 WRITE 경로가 비동기로 넘어가면서, WAL append 완료 콜백(wal_put_done)까지
 * 들고 가야 하는 상태를 담는다. */
struct zns_io_ctx {
	struct zns_base_c *c;
	struct bio *orig_bio;
	u64 lba;
	sector_t phys;
	struct wal_record *wal_buf;
};

/* FREE 태그 zone 하나를 찾아서 tag로 바꿔 배정. 없으면 ZONE_NONE */
static unsigned int zone_pool_acquire_free(struct zone_pool *zp, enum zone_tag tag)
{
	unsigned int z;
	for (z = 0; z < zp->nr_zones; z++) {
		if (zp->zone_tag[z] == ZONE_TAG_FREE) {
			zp->zone_tag[z] = tag;
			zp->wp[z] = 0;
			return z;
		}
	}
	return ZONE_NONE;
}

/* 태그 tag로 nr 섹터 쓸 물리 위치를 반환. 필요하면 zone 새로 배정 */
static int zone_pool_alloc(struct zone_pool *zp, enum zone_tag tag, sector_t nr, sector_t *phys_out)
{
	unsigned int z = zp->active_zone[tag];

	if (z == ZONE_NONE) {
		z = zone_pool_acquire_free(zp, tag);
		if (z == ZONE_NONE)
			return -ENOSPC;
		zp->active_zone[tag] = z;
	}

	while (zp->wp[z] + nr > zp->zone_sectors) {
		z = zone_pool_acquire_free(zp, tag);
		if (z == ZONE_NONE)
			return -ENOSPC;
		zp->active_zone[tag] = z;
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

/* phys 섹터가 몇 번 zone에 속하는지 */
static inline unsigned int zone_of(struct zone_pool *zp, sector_t phys)
{
	return phys / zp->zone_sectors;
}

/* memtable(skip list)에 lba->phys 기록. 이미 있던 lba를 덮어쓴 거라면,
 * 그 옛 phys가 있던 zone은 이 순간부터 그만큼 죽은 공간이 생긴 것이므로
 * invalid_count를 올려준다 — GC(M3)의 victim 선정 근거가 됨.
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

/* memtable에서 lba의 현재 물리 위치를 조회. 찾으면 1, 없으면 0.
 * 호출자가 c->lock을 쥐고 있다고 가정. */
static int mapping_get(struct zns_base_c *c, u64 lba, u64 *phys_out)
{
	return skiplist_lookup(c->memtable, lba, phys_out);
}

/* WAL PUT record가 durable하게 쓰인 뒤 호출되는 완료 콜백.
 * 여기서 비로소 memtable에 매핑을 반영하고, 원본 데이터 bio를 실제
 * 물리 위치로 보내 제출한다 — WAL이 데이터보다 먼저 durable해야 한다는
 * 순서를 지키기 위해 이 콜백 전에는 데이터 쓰기를 절대 내보내지 않는다. */
static void wal_put_done(struct bio *wal_bio)
{
	struct zns_io_ctx *ctx = wal_bio->bi_private;
	struct zns_base_c *c = ctx->c;
	struct bio *orig = ctx->orig_bio;
	blk_status_t wal_status = wal_bio->bi_status;
	u64 lba = ctx->lba;
	sector_t phys = ctx->phys;
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
	spin_unlock_irq(&c->lock);

	if (ret) {
		orig->bi_status = BLK_STS_RESOURCE;
		bio_endio(orig);
		return;
	}

	/* 원본 bio를 실제 phys 위치로 보내 그대로 제출. bi_end_io는 원래
	 * 상위 계층(ext4 등)이 걸어둔 그대로라서, 이 데이터 쓰기가 실제로
	 * 끝나면 별도 콜백 없이도 정상적으로 그쪽에 완료가 통보된다. */
	orig->bi_iter.bi_sector = phys;
	bio_set_dev(orig, c->dev->bdev);
	submit_bio(orig);
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

	/* underlying 디바이스에서 zone 하나의 크기(섹터 단위)를 읽어옴 */
	c->zp->zone_sectors = bdev_zone_sectors(c->dev->bdev);
	/* ti->len: DM이 이 타깃에 할당한 총 논리 섹터 수 */
	c->nr_sectors = ti->len;
	/* 전체 섹터 / zone 크기 = zone 개수 */
	c->zp->nr_zones = c->nr_sectors / c->zp->zone_sectors;

	/* wp[zone_id] = 해당 zone에 쓴 섹터 수. zone 개수만큼만 필요 */
	c->zp->wp = kcalloc(c->zp->nr_zones, sizeof(sector_t), GFP_KERNEL);
	if (!c->zp->wp) {
		ti->error = "out of memory (wp)";
		return -ENOMEM;
	}

	/* zone_tag[zone_id] — kcalloc이 0으로 채워주므로 전부 ZONE_TAG_FREE(=0)로 시작 */
	c->zp->zone_tag = kcalloc(c->zp->nr_zones, sizeof(enum zone_tag), GFP_KERNEL);
	if (!c->zp->zone_tag) {
		ti->error = "out of memory (zone_tag)";
		return -ENOMEM;
	}

	/* invalid_count[zone_id] — kcalloc이라 처음엔 전부 0(죽은 공간 없음) */
	c->zp->invalid_count = kcalloc(c->zp->nr_zones, sizeof(unsigned int), GFP_KERNEL);
	if (!c->zp->invalid_count) {
		ti->error = "out of memory (invalid_count)";
		return -ENOMEM;
	}

	/* active_zone[]은 kzalloc이 0으로 채우지만, 0은 "zone 0번"이라는 유효한 값이라
	 * "아직 배정된 zone 없음"을 뜻하는 ZONE_NONE으로 명시적으로 초기화해야 함 */
	for (i = 0; i < ZONE_TAG_COUNT; i++)
		c->zp->active_zone[i] = ZONE_NONE;

	/* memtable 설정 */
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

	ti->private = c;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 0;
	/* 매핑 단위(4KB=BLOCK_SECTORS)보다 큰 bio는 DM core가 애초에 쪼개서
	 * .map()에 보내게 함 — .map() 안에서 수동으로 dm_accept_partial_bio를
	 * 부르는 것보다 이게 표준적이고 확실한 방법 */
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

	/* 데이터 없는 순수 flush 요청(nr=0) — 특정 LBA와 무관한 "지금까지
	 * 쓴 걸 확실히 반영해" 요청이라 매핑 로직을 타면 안 된다. 안 그러면
	 * bio->bi_iter.bi_sector에 남아있는 임의의 값(보통 0)을 실제 lba처럼
	 * 취급해서 엉뚱한 매핑 엔트리를 덮어쓰게 된다. 그냥 underlying으로
	 * 그대로 전달한다. */
	if (nr == 0) {
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}

	lba = bio->bi_iter.bi_sector;

	/* 매핑 키는 항상 블록(BLOCK_SECTORS) 정렬된 lba를 쓴다. 커널이 슈퍼블록을
	 * 읽을 때처럼 블록 정렬 안 된 위치(예: sector 2부터 2섹터)로 요청이 올 수
	 * 있는데, 이런 요청도 결국 어떤 블록 "안"의 일부이므로, 그 블록의 정렬된
	 * lba로 조회하고 블록 내 옵셋만큼 phys를 보정해서 응답해야 한다. */
	block_lba = (lba / BLOCK_SECTORS) * BLOCK_SECTORS;
	offset_in_block = lba - block_lba;

	/* bio가 지금 블록의 남은 부분을 넘어서면(블록 경계를 넘으면) 딱 그 블록
	 * 끝까지만 처리하고 나머지는 DM core가 다음 map() 호출로 재분배하게 한다. */
	if (nr > BLOCK_SECTORS - offset_in_block) {
		dm_accept_partial_bio(bio, BLOCK_SECTORS - offset_in_block);
		nr = BLOCK_SECTORS - offset_in_block;
	}

	switch (bio_op(bio)) {
	case REQ_OP_WRITE: {
		sector_t phys;     // 실제 데이터가 놓일 물리 섹터
		sector_t wal_phys; // WAL 레코드가 놓일 물리 섹터
		int ret;
		struct zns_io_ctx *ctx;
		struct wal_record *rec;
		struct bio *wal_bio;
		struct page *page;

		spin_lock_irq(&c->lock);
		ret = zone_pool_alloc(c->zp, ZONE_TAG_USER_DATA, nr, &phys);
		if (ret) {
			spin_unlock_irq(&c->lock);
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		/* WAL은 데이터와 별개 zone(태그)에서, 항상 1섹터(512B)짜리
		 * 고정 레코드 하나만 append. 버퍼링 없이 매 쓰기마다 즉시. */
		ret = zone_pool_alloc(c->zp, ZONE_TAG_WAL, 1, &wal_phys);
		spin_unlock_irq(&c->lock);
		if (ret) {
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		/* 섹터 하나(512B) 전체를 kzalloc — 레코드는 앞 32바이트뿐이지만
		 * 디바이스에 512B보다 작은 단위로는 쓸 수 없어 나머지는 패딩. */
		rec = kzalloc(512, GFP_NOIO);
		ctx = kmalloc(sizeof(*ctx), GFP_NOIO);
		if (!rec || !ctx) {
			kfree(rec);
			kfree(ctx);
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		rec->type = WAL_REC_PUT;
		rec->put.lba = lba;
		rec->put.phys = phys;

		ctx->c = c;
		ctx->orig_bio = bio;
		ctx->lba = lba;
		ctx->phys = phys;
		ctx->wal_buf = rec;

		wal_bio = bio_alloc(GFP_NOIO, 1);
		bio_set_dev(wal_bio, c->dev->bdev);
		wal_bio->bi_iter.bi_sector = wal_phys;
		wal_bio->bi_opf = REQ_OP_WRITE;
		page = virt_to_page(rec);
		bio_add_page(wal_bio, page, 512, offset_in_page(rec));
		wal_bio->bi_end_io = wal_put_done;
		wal_bio->bi_private = ctx;
		submit_bio(wal_bio);

		/* 데이터 bio는 아직 안 내보냄 — wal_put_done이 WAL 완료 확인 후
		 * 이어서 처리한다 (WAL이 데이터보다 먼저 durable해야 하므로). */
		return DM_MAPIO_SUBMITTED;
	}
	case REQ_OP_READ: {
		sector_t phys;
		int found;

		/* block_lba(정렬된 키)로 조회 — 요청이 블록 중간에서 시작해도
		 * 그 블록 전체가 어디 있는지는 찾을 수 있다. */
		spin_lock_irq(&c->lock);
		found = mapping_get(c, block_lba, &phys);
		spin_unlock_irq(&c->lock);
		if (!found) {
			/* 한 번도 안 쓴 블록 — 에러가 아니라 0으로 채워진 데이터로 읽히는 게
			 * 표준 블록 디바이스 동작(thin-provisioning과 동일한 관례).
			 * mkfs.ext4가 디바이스 전체를 스캔하면서 이런 읽기를 함. */
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		/* phys는 block_lba의 물리 위치이므로, 실제 요청이 블록 중간에서
		 * 시작했다면(offset_in_block) 그만큼 더해줘야 정확한 위치가 된다. */
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
