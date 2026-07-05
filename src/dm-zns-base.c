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

#define DM_MSG_PREFIX "zns-base"

struct zns_base_c {
	struct dm_dev *dev;
	
	sector_t		zone_sectors;
	unsigned int	nr_zones;
	sector_t 		nr_sectors;

	sector_t 		*wp;
	unsigned int 	active_zone;

	sector_t 		*map; // 물리 섹터

	spinlock_t 		lock;
};

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
	int ret;

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

	/* underlying 디바이스에서 zone 하나의 크기(섹터 단위)를 읽어옴 */
	c->zone_sectors = bdev_zone_sectors(c->dev->bdev);
	/* ti->len: DM이 이 타깃에 할당한 총 논리 섹터 수 */
	c->nr_sectors = ti->len;
	/* 전체 섹터 / zone 크기 = zone 개수 */
	c->nr_zones = c->nr_sectors / c->zone_sectors;
	/* map[lba] = 물리 섹터. 크기가 수십 MiB이므로 vmalloc 사용 */
	c->map = vmalloc(c->nr_sectors * sizeof(sector_t));
	if (!c->map) {
		ti->error = "out of memory (map)";
		return -ENOMEM;
	}
	/* wp[zone_id] = 해당 zone에 쓴 섹터 수. zone 개수만큼만 필요 */
	c->wp = kcalloc(c->nr_zones, sizeof(sector_t), GFP_KERNEL);
	if (!c->wp) {
		ti->error = "out of memory (wp)";
		return -ENOMEM;
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
	kfree(c->wp);
	kfree(c);
	DMINFO("dtr: target detached");
}

static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;

	sector_t lba = bio->bi_iter.bi_sector;
	sector_t nr = bio_sectors(bio);

	memset(c->map, c->nr_zones, sizeof(sector_t));
	switch (bio_op(bio)) {
	case REQ_OP_WRITE: {
		sector_t phys; // zone storage의 물리 sector
		spin_lock_irq(&c->lock);
		while(c->wp[c->active_zone] + nr > c->zone_sectors) {
			c->active_zone++;
			if (c->active_zone >= c->nr_zones) {
				spin_unlock_irq(&c->lock);
				bio->bi_status = BLK_STS_NOSPC;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
		}
		
		phys = c->active_zone * c->zone_sectors + c->wp[c->active_zone]; // 해당 zone의 시작 섹터 + zone이 지금까지 쓴 섹터 수
		c->map[lba] = phys;
		c->wp[c->active_zone] += nr;
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
		DMEMIT("active=%u zones=%u zone_sectors=%llu wp:", c->active_zone, c->nr_zones, (unsigned long long)c->zone_sectors);
		for (i = 0; i < c->nr_zones; i++)
			DMEMIT(" %llu/%llu", (unsigned long long)c->wp[i], (unsigned long long)c->zone_sectors);
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
