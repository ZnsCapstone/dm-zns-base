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
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/sort.h>

#define DM_MSG_PREFIX "zns-base"
#define ZNS_BASE_BLOCK_SIZE 4096
#define SECTORS_PER_BLOCK 8
#define MEMTABLE_POOL_SIZE 4
#define INITIAL_RUN_CAPACITY 4


struct mapping_entry {
	size_t logical_block;
	sector_t physical_sector;
	u64 seq;
};

//run은 MemTable이 꽉 찼을 때 그 내용을 정렬해서 얼려둔 mapping entry 묶음이다.
struct mapping_run {
	struct mapping_entry *entries;
	size_t entry_count;
	size_t entry_capacity;
};

struct mapping_memtable {
	struct mapping_entry *entries;
	size_t entry_count;
	size_t entry_capacity;
	struct list_head node;
};

struct mapping_state {
	struct mapping_memtable *active_memtable;
	struct list_head spare_memtables;
	struct list_head frozen_memtables;

	struct mapping_run *runs;
	size_t run_count;
	size_t run_capacity;
	u64 next_seq;

	size_t spare_count;
	struct work_struct flush_work;
	bool flush_pending;
	int flush_error;
};


struct zns_base_c {
	struct dm_dev *dev;

	//sector_t는 Linux 커널에서 블록 디바이스의 sector 번호를 저장할 때 쓰는 정수 타입이다.
	sector_t next_write_sector; // 실제 아래쪽 zoned device의 physical sector 번호를 뜻한다.
	struct mapping_state mapping;
	size_t nr_logical_blocks;


	spinlock_t lock;
};

static void mapping_memtable_free(struct mapping_memtable *memtable) {
	if(!memtable)
		return;

	kvfree(memtable -> entries);
	kfree(memtable);
}

static int mapping_entry_cmp(const void *left, const void *right) {
	const struct mapping_entry *a = left;
	const struct mapping_entry *b = right;

	if(a -> logical_block < b -> logical_block)
		return -1;
	if(a -> logical_block > b -> logical_block)
		return 1;
	if(a -> seq > b -> seq)
		return -1;
	if(a -> seq < b -> seq)
		return 1;

	return 0;
}

static int mapping_reserve_run_slot(struct zns_base_c *c) {
	struct mapping_run *new_runs;
	struct mapping_run *old_runs;
	size_t new_capacity;

	spin_lock(&c -> lock);

	if(c -> mapping.run_count < c -> mapping.run_capacity) {
		spin_unlock(&c -> lock);
		return 0;
	}

	if(c -> mapping.run_capacity == 0)
		new_capacity = INITIAL_RUN_CAPACITY;
	else
		new_capacity = c -> mapping.run_capacity * 2;

	spin_unlock(&c -> lock);

	new_runs = kvcalloc(new_capacity, sizeof(*new_runs), GFP_KERNEL);
	if(!new_runs)
		return -ENOMEM;

	spin_lock(&c -> lock);
    // 현재는 flush_work 하나만 runs를 추가하므로 lock을 푼 사이 다른 worker가 runs를 늘리지 않는다는 전제
	if(c -> mapping.run_count > 0){
		memcpy(new_runs, c -> mapping.runs, c -> mapping.run_count * sizeof(*new_runs));
	}

	old_runs = c -> mapping.runs;
	c -> mapping.runs = new_runs;
	c -> mapping.run_capacity = new_capacity;
	
	spin_unlock(&c -> lock);

	kvfree(old_runs);
	return 0;
}

static void mapping_flush_work(struct work_struct *work) {
	struct mapping_state *mapping;
	struct zns_base_c *c;
	struct mapping_memtable *memtable;
	struct mapping_run run;
	int ret;
	size_t i, out;

	mapping = container_of(work, struct mapping_state, flush_work);
	c = container_of(mapping, struct zns_base_c, mapping);

	for(;;){
		spin_lock(&c -> lock);

		if(list_empty(&c -> mapping.frozen_memtables)) {
			c -> mapping.flush_pending = false;
			spin_unlock(&c -> lock);
			return;
		}

		memtable = list_first_entry(&c -> mapping.frozen_memtables, struct mapping_memtable, node);

		spin_unlock(&c -> lock);

		run.entry_capacity = memtable -> entry_count;
		run.entry_count = memtable -> entry_count;
		run.entries = kvcalloc(run.entry_capacity, sizeof(struct mapping_entry), GFP_KERNEL);
		if(run.entries == NULL){
			spin_lock(&c -> lock);
			c -> mapping.flush_error = -ENOMEM;
			c -> mapping.flush_pending = false;
			spin_unlock(&c -> lock);
			return;
		}

		memcpy(run.entries, memtable -> entries, memtable -> entry_count * sizeof(struct mapping_entry));
		sort(run.entries, run.entry_count, sizeof(*run.entries), mapping_entry_cmp, NULL);

		out = 0;
		for(i = 0; i < run.entry_count; i++) {
			if(out == 0 || run.entries[i].logical_block != run.entries[out - 1].logical_block){
				run.entries[out] = run.entries[i];
				out++;
			}
		}
		run.entry_count = out;

		ret = mapping_reserve_run_slot(c);
		if(ret){
			kvfree(run.entries);

			spin_lock(&c -> lock);
			c -> mapping.flush_error = ret;
			c -> mapping.flush_pending = false;
			spin_unlock(&c -> lock);

			return;
		}

		spin_lock(&c -> lock);
		c -> mapping.runs[c -> mapping.run_count] = run;
		c -> mapping.run_count++;

		list_del_init(&memtable -> node);
		memtable -> entry_count = 0;
		list_add_tail(&memtable->node, &c->mapping.spare_memtables);
		c -> mapping.spare_count++;

		spin_unlock(&c -> lock);
	}
}

static int mapping_init(struct zns_base_c *c, sector_t target_sectors)
{
	/* TODO: move mapping table allocation/initialization here. */
	size_t nr_logical_blocks;
	struct mapping_memtable *memtable;
	struct mapping_memtable *next;
	size_t mem_capacity = 4096;
	size_t i;
	int ret;

	if(target_sectors % SECTORS_PER_BLOCK != 0){
		return -EINVAL;
	}
	nr_logical_blocks = target_sectors / SECTORS_PER_BLOCK;
	
	INIT_LIST_HEAD(&c -> mapping.spare_memtables);
	INIT_LIST_HEAD(&c -> mapping.frozen_memtables);

	c -> mapping.spare_count = 0;

	for(i = 0; i < MEMTABLE_POOL_SIZE; i++){
		memtable = kzalloc(sizeof(*memtable), GFP_KERNEL);
		if(memtable == NULL){
			ret = -ENOMEM; 
			goto out_free_pool;
		}
		memtable -> entries = kvcalloc(mem_capacity, sizeof(struct mapping_entry), GFP_KERNEL);
		if(memtable -> entries == NULL){
			mapping_memtable_free(memtable);
			ret = -ENOMEM; 
			goto out_free_pool;
		}
		memtable -> entry_count = 0;
		memtable -> entry_capacity = mem_capacity;
		INIT_LIST_HEAD(&memtable -> node);

		if(i == 0){
			c -> mapping.active_memtable = memtable;
		}
		else{
			list_add_tail(&memtable->node, &c->mapping.spare_memtables);
			c -> mapping.spare_count++;
		}
	}

	c -> nr_logical_blocks = nr_logical_blocks;
	c -> mapping.runs = NULL;
	c -> mapping.run_count = 0;
	c -> mapping.run_capacity = 0;
	c -> mapping.next_seq = 1;
	INIT_WORK(&c -> mapping.flush_work, mapping_flush_work);
	c -> mapping.flush_pending = false;
	c -> mapping.flush_error = 0;

	return 0;

out_free_pool:
	mapping_memtable_free(c -> mapping.active_memtable);
	c -> mapping.active_memtable = NULL;

	list_for_each_entry_safe(memtable, next, &c -> mapping.spare_memtables, node){
		list_del(&memtable -> node);
		mapping_memtable_free(memtable);
	}

	c->mapping.spare_count = 0;

	return ret;
}

static void mapping_destroy(struct zns_base_c *c)
{
	/* TODO: move mapping table cleanup here. */
	struct mapping_memtable *memtable;
	struct mapping_memtable *next;
	size_t i;

	cancel_work_sync(&c->mapping.flush_work);

	mapping_memtable_free(c -> mapping.active_memtable);
	c -> mapping.active_memtable = NULL;

	list_for_each_entry_safe(memtable, next, &c -> mapping.spare_memtables, node){
		list_del(&memtable -> node);
		mapping_memtable_free(memtable);
	}

	list_for_each_entry_safe(memtable, next, &c -> mapping.frozen_memtables, node){
		list_del(&memtable -> node);
		mapping_memtable_free(memtable);
	}

	for(i = 0; i < c -> mapping.run_count; i++){
		kvfree(c -> mapping.runs[i].entries);
	}

	kvfree(c -> mapping.runs);
	
	c -> nr_logical_blocks = 0;
	c->mapping.spare_count = 0;
	c -> mapping.runs = NULL;
	c -> mapping.run_count = 0;
	c -> mapping.run_capacity = 0;
	c -> mapping.next_seq = 0;
	c -> mapping.flush_pending = false;
	c -> mapping.flush_error = 0;
}

static int mapping_update(struct zns_base_c *c, size_t logical_block,
			  sector_t physical_sector)
{
	/* TODO: replace direct c->map[] write path with this helper. */
	/*
	active에 자리가 있는지 확인
	만약 자리가 풀이라면 active를 frozen으로 만들기
	spare를 active로 만들기
	이때 spare list에 자리가 있는지 확인하기
	만약 자리가 없다면 새로운 memtable 할당? or spare memtable 여분이 나올때까지 기다리기?
	*/
	struct mapping_entry *cur_entry;
	struct mapping_memtable *active_memtable;
	struct mapping_memtable *new_active;

	active_memtable = c -> mapping.active_memtable;

	if(active_memtable -> entry_count >= active_memtable -> entry_capacity){
		if(list_empty(&c -> mapping.spare_memtables)){
			return -EAGAIN;
		}

		new_active = list_first_entry(&c -> mapping.spare_memtables, struct mapping_memtable, node);
		list_del_init(&new_active -> node);
		c -> mapping.spare_count--;

		list_add_tail(&active_memtable->node, &c->mapping.frozen_memtables);

		c -> mapping.active_memtable = new_active;
		active_memtable = new_active;

		if(!c -> mapping.flush_pending){
			c -> mapping.flush_pending = true;
			schedule_work(&c -> mapping.flush_work);
		}
	}

	cur_entry = &active_memtable -> entries[active_memtable -> entry_count];
	cur_entry -> logical_block = logical_block;
	cur_entry -> physical_sector = physical_sector;
	cur_entry -> seq = c -> mapping.next_seq;

	c -> mapping.next_seq++;
	active_memtable -> entry_count++;

	return 0;
}

//binary search로 바꾸기 
static int mapping_lookup(struct zns_base_c *c, size_t logical_block,
			  sector_t *physical_sector)
{
	/* TODO: replace direct c->map[] read path with this helper. */
	/*
	탐색 순서는 active -> frozen -> runs?
	*/
	size_t i, j;
	struct mapping_memtable *active_memtable;
	struct mapping_memtable *memtable;
	struct mapping_run *run;

	active_memtable = c -> mapping.active_memtable;
	for(i = active_memtable -> entry_count; i > 0; i--){
		if(active_memtable -> entries[i - 1].logical_block == logical_block){
			*physical_sector = active_memtable -> entries[i - 1].physical_sector;
			return 0;
		}
	}
	
	list_for_each_entry_reverse(memtable, &c -> mapping.frozen_memtables, node){
		for(i = memtable -> entry_count; i > 0; i--){
			if(memtable -> entries[i - 1].logical_block == logical_block){
				*physical_sector = memtable -> entries[i - 1].physical_sector;
				return 0;
			}
		}
	}

	for(i = c -> mapping.run_count; i > 0; i--){
		run = &c -> mapping.runs[i - 1];
		for(j = run -> entry_count; j > 0; j--){
			if(run -> entries[j - 1].logical_block == logical_block){
				*physical_sector = run -> entries[j - 1].physical_sector;
				return 0;
			}
		}
	}
	return -ENOENT;
}


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

	spin_lock_init(&c -> lock);
	ret = mapping_init(c, ti -> len);
	
	if(ret){
		ti->error = "failed to allocate mapping state";
		dm_put_device(ti, c->dev);
		kfree(c);
		// Error: Not enough memory
		return ret;
	}

	c -> next_write_sector = 0;

	ti->private = c;
	ti->num_flush_bios = 1;
	// ti->num_discard_bios = 1;

	DMINFO("ctr: target attached on top of '%s'", argv[0]);
	return 0;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;

	mapping_destroy(c);
	dm_put_device(ti, c->dev);
	kfree(c);
	DMINFO("dtr: target detached");
}


static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;
	size_t logical_block_num;
  	sector_t physical_sector;
	int ret;

	/* Student work goes here: translate random writes into sequential ones. */

	// write bio 처리
	if(bio_op(bio) == REQ_OP_WRITE){
		//4kb검사. 나중에 지우기.
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
		// 여기까지가 4kb 검사

		spin_lock(&c -> lock);
		if(c -> next_write_sector + SECTORS_PER_BLOCK > ti -> len){
			spin_unlock(&c -> lock);
			bio->bi_status = BLK_STS_NOSPC;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		/*
		- 단순 MVP 정책: flush_error != 0이면 이후 write를 bio_io_error()로 실패시킨다.
		- 재시도 정책: worker가 일정 조건에서 flush를 다시 시도한다.
		- 더 발전된 정책: 메모리 압박이 해소될 때까지 대기하거나, spare pool을 관리한다.
		현재는 단순 MVP로 구현
		*/
		if(c -> mapping.flush_error){
			spin_unlock(&c -> lock);
			bio_io_error(bio);
			return DM_MAPIO_SUBMITTED;
		}

		physical_sector = c -> next_write_sector;
		ret = mapping_update(c, logical_block_num, physical_sector);
		if (ret == -EAGAIN){
			spin_unlock(&c -> lock);
  			return DM_MAPIO_REQUEUE;
		}
		else if(ret) {
			spin_unlock(&c -> lock);
			bio_io_error(bio);
  			return DM_MAPIO_SUBMITTED;
		}
		c -> next_write_sector += SECTORS_PER_BLOCK;
		spin_unlock(&c -> lock);
		bio -> bi_iter.bi_sector = physical_sector;
	}
	// read bio 처리
	else if(bio_op(bio) == REQ_OP_READ){
		//4kb검사. 나중에 지우기.
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
		// 여기까지가 4kb 검사

		spin_lock(&c -> lock);
		ret = mapping_lookup(c, logical_block_num, &physical_sector);
		if(ret == -ENOENT){
			spin_unlock(&c -> lock);
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		else if(ret){
			spin_unlock(&c -> lock);
			bio_io_error(bio);
			return DM_MAPIO_SUBMITTED;
		}
		spin_unlock(&c -> lock);
		bio -> bi_iter.bi_sector = physical_sector;
	}
	else if(bio_op(bio) == REQ_OP_FLUSH){
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}
	else{ // discard는 m3에서 구현
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
