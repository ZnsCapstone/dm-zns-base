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
#include <linux/spinlock.h>
#include <linux/slab.h>

#define DM_MSG_PREFIX "zns-base"
#define ZNS_BASE_BLOCK_SIZE 4096
#define SECTORS_PER_BLOCK 8

static const sector_t invalid_sector = (sector_t) - 1;

struct zns_base_c {
	struct dm_dev *dev;

	//sector_t는 Linux 커널에서 블록 디바이스의 sector 번호를 저장할 때 쓰는 정수 타입이다.
	sector_t next_write_sector; // 실제 아래쪽 zoned device의 physical sector 번호를 뜻한다.
	sector_t *map;
	size_t nr_logical_blocks;

	spinlock_t lock;
};

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
	sector_t *mapping_table;
	size_t nr_logical_blocks;
	size_t i;
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

	if(ti -> len % SECTORS_PER_BLOCK != 0){
		ti -> error = "target length is not 4KiB aligned";
		dm_put_device(ti, c->dev);
		kfree(c);
	
		return -EINVAL; // 인자가 잘못됐다
	}
	nr_logical_blocks = ti -> len / SECTORS_PER_BLOCK;

	/*
	 	map[0] -> logical block 0의 최신 physical 시작 sector
		map[1] -> logical block 1의 최신 physical 시작 sector
		map[2] -> logical block 2의 최신 physical 시작 sector

		logical block 0은 sector 0~7을 포함해.

		logical block 0 = logical sector 0~7
		logical block 1 = logical sector 8~15
		logical block 2 = logical sector 16~23
	*/
	/*
		여기서는 kzalloc이 아니라 kvcalloc을 사용. 이것은 꼭 연속된 메모리 공간이 아니여도 된다.
		nr_logical_blocks개만큼, 각 entry는 sector_t 크기, 0으로 초기화해서 할당
		
		GFP_KERNEL의 의미는:
		일반적인 커널 context에서 쓰는 메모리 할당
		필요하면 sleep 가능
		메모리를 확보하기 위해 reclaim 가능
	*/
	mapping_table = kvcalloc(nr_logical_blocks, sizeof(sector_t), GFP_KERNEL);
	
	if(mapping_table == NULL){
		ti->error = "failed to allocate mapping table";
		dm_put_device(ti, c->dev);
		kfree(c);
		// Error: Not enough memory
		return -ENOMEM;
	}

	for(i = 0; i < nr_logical_blocks; i++){
		mapping_table[i] = invalid_sector;
	}
	c -> map = mapping_table;
	c -> nr_logical_blocks = nr_logical_blocks;
	c -> next_write_sector = 0;
	spin_lock_init(&c -> lock); // lock 초기화

	ti->private = c;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;

	DMINFO("ctr: target attached on top of '%s'", argv[0]);
	return 0;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;

	kvfree(c -> map);
	dm_put_device(ti, c->dev);
	kfree(c);
	DMINFO("dtr: target detached");
}

static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;
	size_t logical_block_num;
  	sector_t physical_sector;

	/* Student work goes here: translate random writes into sequential ones. */
	if(bio -> bi_iter.bi_sector % SECTORS_PER_BLOCK != 0 || bio -> bi_iter.bi_size != ZNS_BASE_BLOCK_SIZE){
		bio->bi_status = BLK_STS_NOTSUPP; // 이 bio는 지원하지 않는 요청으로 표시
		bio_endio(bio); // bio 끝냄
		return DM_MAPIO_SUBMITTED; // DM에게 내가 처리했다 라고 알리는것.
	}
	logical_block_num = bio -> bi_iter.bi_sector / SECTORS_PER_BLOCK;

	if(logical_block_num >= c -> nr_logical_blocks){
		bio_io_error(bio);
  		return DM_MAPIO_SUBMITTED;
	}

	// write bio 처리
	if(bio_op(bio) == REQ_OP_WRITE){
		spin_lock(&c -> lock);
		if(c -> next_write_sector + SECTORS_PER_BLOCK > ti -> len){
			spin_unlock(&c->lock);
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		physical_sector = c -> next_write_sector;
		c -> map[logical_block_num] = physical_sector;
		c -> next_write_sector += SECTORS_PER_BLOCK;
		spin_unlock(&c -> lock);
		bio -> bi_iter.bi_sector = physical_sector;
	}
	// read bio 처리
	else if(bio_op(bio) == REQ_OP_READ){
		spin_lock(&c -> lock);
		physical_sector = c -> map[logical_block_num];
		spin_unlock(&c -> lock);
		if(physical_sector == invalid_sector){
			bio_io_error(bio);
  			return DM_MAPIO_SUBMITTED;
		}
		bio -> bi_iter.bi_sector = physical_sector;
	}
	// read와 write 둘 다 아닌 경우 ex) flush, discard
	else{
		bio->bi_status = BLK_STS_NOTSUPP;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	// 이 bio 요청이 내려갈 대상 block device를 아래쪽 underlying device로 바꾸는 코드.
	bio_set_dev(bio, c->dev->bdev);
	return DM_MAPIO_REMAPPED;
}

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 1, 0},
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
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
