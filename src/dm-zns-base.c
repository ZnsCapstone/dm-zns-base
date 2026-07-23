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
#include <linux/mempool.h>
#include <linux/wait.h>
#include <linux/highmem.h>
#include <linux/mm.h>

#define DM_MSG_PREFIX "zns-base"
#define ZNS_BASE_BLOCK_SIZE 4096
#define ZNS_BASE_SECTOR_SIZE 512
#define SECTORS_PER_BLOCK 8
#define MEMTABLE_POOL_SIZE 4
#define INITIAL_RUN_CAPACITY 4
#define IO_POOL_SIZE 128


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

struct zns_base_chunk {
	size_t logical_block;
	unsigned int block_offset_bytes;
	unsigned int bio_offset_bytes;
	unsigned int length_bytes;
};

struct zns_base_io {
	struct bio *bio;
	struct list_head node;
};

struct zns_base_c {
	struct dm_dev *dev;

	//sector_t는 Linux 커널에서 블록 디바이스의 sector 번호를 저장할 때 쓰는 정수 타입이다.
	sector_t next_write_sector; // 실제 아래쪽 zoned device의 physical sector 번호를 뜻한다.
	struct mapping_state mapping;
	size_t nr_logical_blocks;

	struct list_head pending_bios;
	struct work_struct io_work;
	bool io_work_scheduled;
	mempool_t *io_pool;
	bool stopping;

	wait_queue_head_t spare_waitq;

	// 나중에 io_lock이랑 mapping_lock이랑 나누기.
	spinlock_t lock;
};

static int mapping_update(struct zns_base_c *c, size_t logical_block,
			  sector_t physical_sector);
static int mapping_lookup(struct zns_base_c *c, size_t logical_block,
			  sector_t *physical_sector);
static int mapping_wait_for_write_slot(struct zns_base_c *c);


static void zns_base_next_chunk(sector_t current_sector, unsigned int remaining_bytes, 
				unsigned int bio_offset_bytes,struct zns_base_chunk *chunk)
{
  	unsigned int offset_sectors;
  	unsigned int block_remaining_bytes;

  	offset_sectors = current_sector % SECTORS_PER_BLOCK;

  	chunk->logical_block = current_sector / SECTORS_PER_BLOCK;
  	chunk->block_offset_bytes =
  		offset_sectors * ZNS_BASE_SECTOR_SIZE;
  	chunk->bio_offset_bytes = bio_offset_bytes;

  	block_remaining_bytes =
  		ZNS_BASE_BLOCK_SIZE - chunk->block_offset_bytes;

  	chunk->length_bytes =
  		min_t(unsigned int, remaining_bytes, block_remaining_bytes);
}

static int zns_base_queue_bio(struct zns_base_c *c, struct bio *bio){
	struct zns_base_io *io;
	bool schedule = false;

	io = mempool_alloc(c -> io_pool, GFP_ATOMIC);
	if(!io){
		return -ENOMEM;
	}

	io -> bio = bio;
	INIT_LIST_HEAD(&io -> node);

	spin_lock(&c -> lock);

	if(c -> stopping){
		spin_unlock(&c -> lock);
		mempool_free(io, c -> io_pool);
		return -EIO;
	}

	list_add_tail(&io -> node, &c -> pending_bios);
	if(!c -> io_work_scheduled){
		c -> io_work_scheduled = true;
		schedule = true;
	}

	spin_unlock(&c -> lock);

	if(schedule)
		schedule_work(&c -> io_work);

	return 0;
}

static int zns_base_submit_clone(struct zns_base_c *c, struct bio *bio, sector_t physical_sector){
	struct bio *clone;
	int ret;

	clone = bio_clone_fast(bio, GFP_KERNEL, &fs_bio_set);
	if(!clone)
		return -ENOMEM;

	bio_set_dev(clone, c->dev->bdev);
	clone -> bi_iter.bi_sector = physical_sector;

	ret = submit_bio_wait(clone);
	bio_put(clone);

	return ret;
}

static int zns_base_submit_clone_range(struct zns_base_c *c,
  				       struct bio *bio,
  				       unsigned int bio_offset_bytes,
  				       unsigned int length_bytes,
  				       sector_t physical_sector)
{
  	struct bio *clone;
  	int ret;

  	if (bio_offset_bytes % ZNS_BASE_SECTOR_SIZE ||
  	    length_bytes % ZNS_BASE_SECTOR_SIZE)
  		return -EINVAL;

  	clone = bio_clone_fast(bio, GFP_KERNEL, &fs_bio_set);
  	if (!clone)
  		return -ENOMEM;

  	/*
  	 * clone에 원본 bio 전체가 아니라,
  	 * [bio_offset_bytes, length_bytes] 범위만 남긴다.
  	 */
  	bio_trim(clone,
  		 bio_offset_bytes / ZNS_BASE_SECTOR_SIZE,
  		 length_bytes / ZNS_BASE_SECTOR_SIZE);

  	bio_set_dev(clone, c->dev->bdev);
  	clone->bi_iter.bi_sector = physical_sector;

  	ret = submit_bio_wait(clone);
  	bio_put(clone);

  	return ret;
}

static int zns_base_submit_page(struct zns_base_c *c,
  				struct page *page,
  				unsigned int op,
  				sector_t physical_sector)
{
  	struct bio *page_bio;
  	int added;
  	int ret;

  	page_bio = bio_alloc(GFP_KERNEL, 1);
  	if (!page_bio)
  		return -ENOMEM;

  	bio_set_dev(page_bio, c->dev->bdev);
  	bio_set_op_attrs(page_bio, op, 0);
  	page_bio->bi_iter.bi_sector = physical_sector;

  	added = bio_add_page(page_bio, page, ZNS_BASE_BLOCK_SIZE, 0);
  	if (added != ZNS_BASE_BLOCK_SIZE) {
  		bio_put(page_bio);
  		return -EIO;
  	}

  	ret = submit_bio_wait(page_bio);
  	bio_put(page_bio);

  	return ret;
}

static void zns_base_copy_from_bio(struct bio *bio,
  				   unsigned int bio_offset_bytes,
  				   void *dst,
  				   unsigned int length_bytes)
{
  	struct bvec_iter iter;
  	struct bio_vec bvec;
  	void *src;
  	unsigned int bytes;
  	char *dst_ptr;

  	iter = bio->bi_iter;
  	bio_advance_iter(bio, &iter, bio_offset_bytes);
  	dst_ptr = dst;

  	while (length_bytes > 0) {
  		bvec = bio_iter_iovec(bio, iter);
  		bytes = min_t(unsigned int, length_bytes, bvec.bv_len);

  		src = kmap_local_page(bvec.bv_page);
  		memcpy(dst_ptr, (char *)src + bvec.bv_offset, bytes);
  		kunmap_local(src);

  		dst_ptr += bytes;
  		bio_advance_iter(bio, &iter, bytes);
  		length_bytes -= bytes;
  	}
}

static void zns_base_zero_bio_range(struct bio *bio,
  				    unsigned int bio_offset_bytes,
  				    unsigned int length_bytes)
{
  	struct bvec_iter iter;
  	struct bio_vec bvec;
  	void *addr;
  	unsigned int bytes;

  	iter = bio->bi_iter;
  	bio_advance_iter(bio, &iter, bio_offset_bytes);

  	while (length_bytes > 0) {
  		bvec = bio_iter_iovec(bio, iter);
  		bytes = min_t(unsigned int, length_bytes, bvec.bv_len);

  		addr = kmap_local_page(bvec.bv_page);
  		memset((char *)addr + bvec.bv_offset, 0, bytes);
  		kunmap_local(addr);

  		bio_advance_iter(bio, &iter, bytes);
  		length_bytes -= bytes;
  	}
}

static int zns_base_read_chunk(struct zns_base_c *c,
  			       struct bio *bio,
  			       struct zns_base_chunk *chunk)
{
  	sector_t physical_sector;
  	int ret;

  	spin_lock(&c->lock);
  	ret = mapping_lookup(c, chunk->logical_block, &physical_sector);
  	spin_unlock(&c->lock);

  	if (ret == -ENOENT) {
  		zns_base_zero_bio_range(bio,
  					chunk->bio_offset_bytes,
  					chunk->length_bytes);
  		return 0;
  	}

  	if (ret)
  		return ret;

  	physical_sector +=
  		chunk->block_offset_bytes / ZNS_BASE_SECTOR_SIZE;

  	return zns_base_submit_clone_range(c, bio,
  					   chunk->bio_offset_bytes,
  					   chunk->length_bytes,
  					   physical_sector);
}

static void zns_base_process_read_bio(struct zns_base_c *c, struct bio *bio)
{
  	struct zns_base_chunk chunk;
  	sector_t current_sector;
  	unsigned int remaining_bytes;
  	unsigned int bio_offset_bytes;
  	int ret;

  	if (bio->bi_iter.bi_size % ZNS_BASE_SECTOR_SIZE) {
  		bio->bi_status = BLK_STS_NOTSUPP;
  		bio_endio(bio);
  		return;
  	}

  	current_sector = bio->bi_iter.bi_sector;
  	remaining_bytes = bio->bi_iter.bi_size;
  	bio_offset_bytes = 0;

  	while (remaining_bytes > 0) {
  		zns_base_next_chunk(current_sector, remaining_bytes,
  				    bio_offset_bytes, &chunk);

  		if (chunk.logical_block >= c->nr_logical_blocks) {
  			bio_io_error(bio);
  			return;
  		}

  		ret = zns_base_read_chunk(c, bio, &chunk);
  		if (ret) {
  			bio_io_error(bio);
  			return;
  		}

  		current_sector +=
  			chunk.length_bytes / ZNS_BASE_SECTOR_SIZE;
  		remaining_bytes -= chunk.length_bytes;
  		bio_offset_bytes += chunk.length_bytes;
  	}

  	bio_endio(bio);
}

static int zns_base_write_chunk(struct zns_base_c *c,
  				struct bio *bio,
  				struct zns_base_chunk *chunk)
{
  	struct page *scratch_page;
  	sector_t old_physical_sector;
  	sector_t new_physical_sector;
  	void *scratch_addr;
  	bool full_block;
  	int ret;

  	full_block = chunk->block_offset_bytes == 0 &&
  		     chunk->length_bytes == ZNS_BASE_BLOCK_SIZE;
  	scratch_page = NULL;

  	if (!full_block) {
  		scratch_page = alloc_page(GFP_KERNEL);
  		if (!scratch_page)
  			return -ENOMEM;

  		spin_lock(&c->lock);
  		ret = mapping_lookup(c, chunk->logical_block,
  				     &old_physical_sector);
  		spin_unlock(&c->lock);

  		if (ret == -ENOENT) {
  			clear_highpage(scratch_page);
  		} else if (ret) {
  			__free_page(scratch_page);
  			return ret;
  		} else {
  			ret = zns_base_submit_page(c, scratch_page,
  						   REQ_OP_READ,
  						   old_physical_sector);
  			if (ret) {
  				__free_page(scratch_page);
  				return ret;
  			}
  		}

  		scratch_addr = kmap_local_page(scratch_page);
  		zns_base_copy_from_bio(bio, chunk->bio_offset_bytes,
  				       (char *)scratch_addr +
  				       chunk->block_offset_bytes,
  				       chunk->length_bytes);
  		kunmap_local(scratch_addr);
  	}

  	ret = mapping_wait_for_write_slot(c);
  	if (ret)
  		goto out_free_page;

  	spin_lock(&c->lock);
  	if (c->next_write_sector + SECTORS_PER_BLOCK >
  	    c->nr_logical_blocks * SECTORS_PER_BLOCK) {
  		spin_unlock(&c->lock);
  		ret = -ENOSPC;
  		goto out_free_page;
  	}

  	new_physical_sector = c->next_write_sector;
  	spin_unlock(&c->lock);

  	if (full_block) {
  		ret = zns_base_submit_clone_range(c, bio,
  						  chunk->bio_offset_bytes,
  						  chunk->length_bytes,
  						  new_physical_sector);
  	} else {
  		ret = zns_base_submit_page(c, scratch_page,
  					   REQ_OP_WRITE,
  					   new_physical_sector);
  	}

  	if (ret)
  		goto out_free_page;

  	spin_lock(&c->lock);
  	ret = mapping_update(c, chunk->logical_block,
  			     new_physical_sector);
  	if (!ret)
  		c->next_write_sector += SECTORS_PER_BLOCK;
  	spin_unlock(&c->lock);

  out_free_page:
  	if (scratch_page)
  		__free_page(scratch_page);

  	return ret;
}

static void zns_base_process_write_bio(struct zns_base_c *c,
  				       struct bio *bio)
{
  	struct zns_base_chunk chunk;
  	sector_t current_sector;
  	unsigned int remaining_bytes;
  	unsigned int bio_offset_bytes;
  	int ret;

  	if (bio->bi_iter.bi_size % ZNS_BASE_SECTOR_SIZE) {
  		bio->bi_status = BLK_STS_NOTSUPP;
  		bio_endio(bio);
  		return;
  	}

  	current_sector = bio->bi_iter.bi_sector;
  	remaining_bytes = bio->bi_iter.bi_size;
  	bio_offset_bytes = 0;

  	while (remaining_bytes > 0) {
  		zns_base_next_chunk(current_sector, remaining_bytes,
  				    bio_offset_bytes, &chunk);

  		if (chunk.logical_block >= c->nr_logical_blocks) {
  			bio_io_error(bio);
  			return;
  		}

  		ret = zns_base_write_chunk(c, bio, &chunk);
  		if (ret) {
  			bio_io_error(bio);
  			return;
  		}

  		current_sector +=
  			chunk.length_bytes / ZNS_BASE_SECTOR_SIZE;
  		remaining_bytes -= chunk.length_bytes;
  		bio_offset_bytes += chunk.length_bytes;
  	}

  	bio_endio(bio);
}

static bool mapping_write_ready(struct zns_base_c *c){
	bool ready;

	spin_lock(&c -> lock);

	ready = c -> stopping ||
			c -> mapping.flush_error ||
			c -> mapping.active_memtable -> entry_count < c -> mapping.active_memtable -> entry_capacity ||
			!list_empty(&c -> mapping.spare_memtables);
		
	spin_unlock(&c -> lock);

	return ready;
}

static int mapping_wait_for_write_slot(struct zns_base_c *c){
	for(;;){
		spin_lock(&c -> lock);

		if(c -> stopping){
			spin_unlock(&c -> lock);
			return -EIO;
		}

		if(c -> mapping.flush_error){
			spin_unlock(&c -> lock);
			return c -> mapping.flush_error;
		}

		if(c -> mapping.active_memtable -> entry_count < c -> mapping.active_memtable -> entry_capacity ||
			!list_empty(&c -> mapping.spare_memtables)){
				spin_unlock(&c -> lock);
				return 0;
		}

		spin_unlock(&c -> lock);

		wait_event(c -> spare_waitq, mapping_write_ready(c));
	}
}

static void zns_base_process_bio(struct zns_base_c *c, struct bio *bio){
	int ret;

	/* Student work goes here: translate random writes into sequential ones. */

	if(bio_op(bio) == REQ_OP_FLUSH){
		ret = zns_base_submit_clone(c, bio, bio -> bi_iter.bi_sector);
		if(ret)
			bio_io_error(bio);
		else
			bio_endio(bio);		
		return;
	}
	// write bio 처리
	else if(bio_op(bio) == REQ_OP_WRITE){
		zns_base_process_write_bio(c, bio);
		return;
	}
	// read bio 처리
	else if(bio_op(bio) == REQ_OP_READ){
		zns_base_process_read_bio(c, bio);
		return;
	}
	bio->bi_status = BLK_STS_NOTSUPP;
	bio_endio(bio);
}

static void zns_base_io_work(struct work_struct *work){
	struct zns_base_c *c;
	struct zns_base_io *io;

	c = container_of(work, struct zns_base_c, io_work);

	for(;;){
		spin_lock(&c -> lock);

		if(list_empty(&c -> pending_bios)){
			c -> io_work_scheduled = false;
			spin_unlock(&c -> lock);
			return;
		}

		io = list_first_entry(&c -> pending_bios, struct zns_base_io, node);
		list_del_init(&io -> node);

		spin_unlock(&c -> lock);

		zns_base_process_bio(c, io -> bio);
		mempool_free(io, c -> io_pool);
	}
}

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

			wake_up_all(&c -> spare_waitq);
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

			wake_up_all(&c -> spare_waitq);

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

		wake_up_all(&c -> spare_waitq);
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

	INIT_LIST_HEAD(&c -> pending_bios);
	init_waitqueue_head(&c -> spare_waitq);
	c -> io_work_scheduled = false;
	c -> stopping = false;

	c -> io_pool = mempool_create_kmalloc_pool(IO_POOL_SIZE, sizeof(struct zns_base_io));

	if(!c -> io_pool){
		ti -> error = "failed to allocate I/O context pool";
		mapping_destroy(c);
		dm_put_device(ti, c -> dev);
		kfree(c);
		return -ENOMEM; 
	}

	INIT_WORK(&c -> io_work, zns_base_io_work);

	ti->private = c;
	ti->num_flush_bios = 1;
	// ti->num_discard_bios = 1;

	DMINFO("ctr: target attached on top of '%s'", argv[0]);
	return 0;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;
	struct zns_base_io *io;

	spin_lock(&c -> lock);
	c -> stopping = true;
	spin_unlock(&c -> lock);

	wake_up_all(&c -> spare_waitq);

	cancel_work_sync(&c -> io_work);

	for(;;){
		spin_lock(&c -> lock);
		if(list_empty(&c -> pending_bios)){
			c -> io_work_scheduled = false;
			spin_unlock(&c -> lock);
			break;
		}
		io = list_first_entry(&c -> pending_bios, struct zns_base_io, node);
		list_del_init(&io -> node);
		spin_unlock(&c -> lock);

		bio_io_error(io -> bio);
		mempool_free(io, c -> io_pool);
	}

	mempool_destroy(c -> io_pool);
	c -> io_pool = NULL;

	mapping_destroy(c);
	dm_put_device(ti, c->dev);
	kfree(c);
	DMINFO("dtr: target detached");
}


static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti -> private;
	int ret;

	if(bio_op(bio) != REQ_OP_READ && bio_op(bio) != REQ_OP_WRITE && bio_op(bio) != REQ_OP_FLUSH){
		bio -> bi_status = BLK_STS_NOTSUPP;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	ret = zns_base_queue_bio(c, bio);
	if(ret)
		bio_io_error(bio);

	return DM_MAPIO_SUBMITTED;
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
