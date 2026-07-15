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
#include <linux/vmalloc.h>
#include <linux/blkdev.h>

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

// zone pool
struct zone_pool {
	sector_t zone_sectors;
	unsigned int nr_zones;
	enum zone_tag *zone_tag; 					// zone_tag[zone_id] — 이 zone이 지금 뭘로 쓰이는지
	sector_t *wp; 								// zone_id별 쓴 섹터 수
	unsigned int active_zone[ZONE_TAG_COUNT]; 	// 태그별 현재 활성 zone
};

struct zns_base_c {
	struct dm_dev *dev;

	sector_t 		nr_sectors;
	struct zone_pool *zp;
	struct skiplist *memtable;
	sector_t 		*map; // 물리 섹터

	spinlock_t 		lock;
};

// SSTable - 오래된 걸 압축해서 Zone에 색인포함으로 보관
struct SSTable {
	u64 lba;
	u64 phys;
};

// WAL - 복구용
struct wal_record {
	u32 type;
	u32 reserved;
	union {
        struct { u64 lba; u64 phys; } put;
        struct { u64 seq_no; u64 unused; } checkpoint;
    };
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

	/* map[lba] = 물리 섹터. 크기가 수십 MiB이므로 vmalloc 사용 */
	c->map = vmalloc(c->nr_sectors * sizeof(sector_t));
	if (!c->map) {
		ti->error = "out of memory (map)";
		return -ENOMEM;
	}

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

	DMINFO("ctr: target attached on top of '%s'", argv[0]);
	return 0;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;

	dm_put_device(ti, c->dev);
	vfree(c->map);
	skiplist_destroy(c->memtable);
	kfree(c->memtable);
	kfree(c->zp->wp);
	kfree(c->zp->zone_tag);
	kfree(c->zp);
	kfree(c);
	DMINFO("dtr: target detached");
}

static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;

	sector_t lba = bio->bi_iter.bi_sector;
	sector_t nr = bio_sectors(bio);

	switch (bio_op(bio)) {
	case REQ_OP_WRITE: {
		sector_t phys; // zone storage의 물리 sector
		int ret;

		spin_lock_irq(&c->lock);
		ret = zone_pool_alloc(c->zp, ZONE_TAG_USER_DATA, nr, &phys);
		if (ret) {
			spin_unlock_irq(&c->lock);
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		c->map[lba] = phys;
		spin_unlock_irq(&c->lock);
		bio->bi_iter.bi_sector = phys;
		break;
	}
	case REQ_OP_READ: {
		spin_lock_irq(&c->lock);
		bio->bi_iter.bi_sector = c->map[lba];
		spin_unlock_irq(&c->lock);
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
