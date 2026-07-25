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
#include <linux/blkdev.h>
#include <linux/atomic.h>

#define DM_MSG_PREFIX "zns-base"
#define ZNS_BASE_BLOCK_SIZE 4096
#define ZNS_BASE_SECTOR_SIZE 512
#define SECTORS_PER_BLOCK 8
#define MEMTABLE_POOL_SIZE 4
#define INITIAL_RUN_CAPACITY 4
#define IO_POOL_SIZE 128
#define GC_RESERVE_ZONES 2
#define GC_LOW_WATERMARK 3
#define GC_TARGET_FREE_ZONES 5
#define ZNS_BASE_NO_ZONE ((unsigned int)-1)


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

enum zns_base_zone_state {
  	ZNS_BASE_ZONE_FREE,
  	ZNS_BASE_ZONE_ACTIVE,
  	ZNS_BASE_ZONE_FULL,
	ZNS_BASE_ZONE_GC_DEST,
  	ZNS_BASE_ZONE_GC_VICTIM,
};

struct zns_base_zone_slot {
  	size_t logical_block;
  	bool valid;
};

struct zns_base_zone {
  	sector_t start_sector;
  	sector_t capacity_sectors;
  	sector_t write_pointer;
  	unsigned int nr_blocks;
  	unsigned int valid_blocks;
	struct zns_base_zone_slot *slots;
  	enum zns_base_zone_state state;

	atomic_t inflight_reads;
	wait_queue_head_t read_waitq;
};

struct zns_base_zone_state_table {
  	struct zns_base_zone *zones;
  	unsigned int nr_zones;
  	unsigned int active_zone_idx;
	unsigned int gc_dest_zone_idx;
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

	struct zns_base_zone_state_table zone_state;

	struct mapping_state mapping;
	size_t nr_logical_blocks;

	struct list_head pending_bios;
	
	struct workqueue_struct *io_wq;
	struct workqueue_struct *gc_wq;
	
	struct work_struct io_work;
	struct work_struct gc_work;

	bool io_work_scheduled;
	bool gc_scheduled;
	bool gc_running;
	int gc_error;

	mempool_t *io_pool;
	bool stopping;

	wait_queue_head_t spare_waitq;
	wait_queue_head_t gc_waitq;

	// 나중에 io_lock이랑 mapping_lock이랑 나누기.
	spinlock_t lock;
};

static int mapping_update(struct zns_base_c *c, size_t logical_block, sector_t physical_sector);
static int mapping_lookup(struct zns_base_c *c, size_t logical_block, struct mapping_entry *entry);
static int mapping_update_if_match(struct zns_base_c *c, size_t logical_block,
  				   sector_t expected_physical_sector, u64 expected_seq, sector_t new_physical_sector);
static int mapping_wait_for_write_slot(struct zns_base_c *c);
static int zns_base_allocate_block(struct zns_base_c *c, sector_t *physical_sector);
static int zns_base_commit_block(struct zns_base_c *c, sector_t physical_sector);
static int zns_base_get_zone_slot(struct zns_base_c *c,sector_t physical_sector,
  				  struct zns_base_zone **zone_out, unsigned int *slot_out);
static void zns_base_gc_work(struct work_struct *work);
static void zns_base_schedule_gc(struct zns_base_c *c);
static int zns_base_gc_move_block(struct zns_base_c *c, struct zns_base_zone *victim,
  				  unsigned int victim_slot);
static int zns_base_reset_victim(struct zns_base_c *c, struct zns_base_zone *victim);
static unsigned int zns_base_count_free_zones(struct zns_base_c *c);
static int zns_base_select_victim(struct zns_base_c *c, struct zns_base_zone **victim_out);
static void zns_base_release_victim(struct zns_base_c *c, struct zns_base_zone *victim);
static bool zns_base_gc_space_ready(struct zns_base_c *c);
static int zns_base_wait_for_gc_space(struct zns_base_c *c);


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
		queue_work(c -> io_wq, &c -> io_work);

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

static void zns_base_zone_read_get(struct zns_base_zone *zone)
{
  	atomic_inc(&zone->inflight_reads);
}

static void zns_base_zone_read_put(struct zns_base_zone *zone)
{
  	if (atomic_dec_and_test(&zone->inflight_reads))
  		wake_up_all(&zone->read_waitq);
}

static int zns_base_read_chunk(struct zns_base_c *c, struct bio *bio,
  			       struct zns_base_chunk *chunk)
{
	struct zns_base_zone *zone;
	struct mapping_entry entry;
	unsigned int slot;
	bool zone_pinned;
  	sector_t physical_sector;
  	int ret;
	
	zone_pinned = false;

  	spin_lock(&c->lock);
  	ret = mapping_lookup(c, chunk->logical_block, &entry);
	if (!ret) {
		physical_sector = entry.physical_sector;
		ret = zns_base_get_zone_slot(c, physical_sector, &zone, &slot);
		if (!ret) {
			zns_base_zone_read_get(zone);
			zone_pinned = true;
		}
	}
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

  	ret = zns_base_submit_clone_range(c, bio,
  					   chunk->bio_offset_bytes,
  					   chunk->length_bytes,
  					   physical_sector);
	if (zone_pinned)
		zns_base_zone_read_put(zone);

	return ret;
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
  				struct bio *bio, struct zns_base_chunk *chunk)
{
  	sector_t new_physical_sector;
	struct mapping_entry old_entry;
	struct zns_base_zone *old_zone;
  	struct zns_base_zone *new_zone;
  	struct page *scratch_page;
  	void *scratch_addr;
  	bool full_block;
	unsigned int old_slot;
  	unsigned int new_slot;
  	bool had_old_mapping;
	bool old_zone_pinned;
  	int ret;

  	full_block = chunk->block_offset_bytes == 0 &&
  		     chunk->length_bytes == ZNS_BASE_BLOCK_SIZE;
  	scratch_page = NULL;
	had_old_mapping = false;
	old_zone_pinned = false;

  	if (!full_block) {
  		scratch_page = alloc_page(GFP_KERNEL);
  		if (!scratch_page)
  			return -ENOMEM;

  		spin_lock(&c->lock);

		ret = mapping_lookup(c, chunk->logical_block, &old_entry);

		if (ret == -ENOENT) {
			clear_highpage(scratch_page);
			ret = 0;
		} 
		else if (!ret) {
			ret = zns_base_get_zone_slot(c, old_entry.physical_sector,
										&old_zone, &old_slot);

			if (!ret &&
				(!old_zone->slots[old_slot].valid ||
				old_zone->slots[old_slot].logical_block !=
				chunk->logical_block))
				ret = -EIO;

			if (!ret) {
				zns_base_zone_read_get(old_zone);
				old_zone_pinned = true;
			}
		}

		spin_unlock(&c->lock);

		if (ret)
			goto out_free_page;

		if (old_zone_pinned) {
			ret = zns_base_submit_page(c, scratch_page, REQ_OP_READ,
										old_entry.physical_sector);

			zns_base_zone_read_put(old_zone);
			old_zone_pinned = false;

			if (ret)
				goto out_free_page;
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

	for (;;) {
		spin_lock(&c->lock);
		ret = zns_base_allocate_block(c, &new_physical_sector);
		spin_unlock(&c->lock);

		if (ret != -EAGAIN)
			break;

		ret = zns_base_wait_for_gc_space(c);
		if (ret)
			goto out_free_page;
	}

	if (ret)
		goto out_free_page;

	spin_lock(&c->lock);

  	ret = mapping_lookup(c, chunk->logical_block, &old_entry);
  	if (ret == -ENOENT) {
  		had_old_mapping = false;
  		ret = 0;
  	} else if (ret) {
  		spin_unlock(&c->lock);
  		goto out_free_page;
  	} else {
  		had_old_mapping = true;
  	}

  	if (!ret)
  		ret = zns_base_get_zone_slot(c, new_physical_sector,
  					     &new_zone, &new_slot);

  	if (!ret && had_old_mapping)
  		ret = zns_base_get_zone_slot(c, old_entry.physical_sector,
  					     &old_zone, &old_slot);

  	/*
  	 * 새 PBA는 아직 사용되지 않은 slot이어야 하고,
  	 * 기존 mapping이 있다면 old slot은 valid여야 한다.
  	 */
  	if (!ret && new_zone->slots[new_slot].valid)
  		ret = -EIO;

  	if (!ret && had_old_mapping &&
  	    (!old_zone->slots[old_slot].valid ||
  	     old_zone->slots[old_slot].logical_block !=
  	     chunk->logical_block))
  		ret = -EIO;

  	spin_unlock(&c->lock);

  	if (ret)
  		goto out_free_page;

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

	/* lower write가 성공했으므로 underlying WP와 맞춰 먼저 증가시킨다. */
	ret = zns_base_commit_block(c, new_physical_sector);
	if (!ret)
		ret = mapping_update(c, chunk->logical_block,
					new_physical_sector);

	if (!ret) {
  		new_zone->slots[new_slot].logical_block =
  			chunk->logical_block;
  		new_zone->slots[new_slot].valid = true;
  		new_zone->valid_blocks++;

  		if (had_old_mapping) {
  			old_zone->slots[old_slot].valid = false;
  			old_zone->valid_blocks--;
  		}
  	}

	spin_unlock(&c->lock);

	if (!ret)
  		zns_base_schedule_gc(c);
	
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

static void zns_base_gc_work(struct work_struct *work)
{
  	struct zns_base_c *c;
	struct zns_base_zone *victim;
  	unsigned int slot;
  	int ret;

  	c = container_of(work, struct zns_base_c, gc_work);

  	spin_lock(&c->lock);
  	c->gc_running = true;
	c->gc_error = 0;
  	spin_unlock(&c->lock);

  	ret = 0;

  	for (;;) {
  		spin_lock(&c->lock);

  		if (c->stopping ||
  		    zns_base_count_free_zones(c) >=
  		    GC_TARGET_FREE_ZONES) {
  			spin_unlock(&c->lock);
  			break;
  		}

  		spin_unlock(&c->lock);

  		ret = zns_base_select_victim(c, &victim);
  		if (ret == -ENOENT) {
  			ret = -ENOSPC;
  			break;
  		}

  		for (slot = 0; slot < victim->nr_blocks; slot++) {
  			ret = zns_base_gc_move_block(c, victim, slot);
  			if (ret)
  				break;
  		}

  		if (ret) {
  			zns_base_release_victim(c, victim);
  			break;
  		}

  		ret = zns_base_reset_victim(c, victim);
  		if (ret) {
  			zns_base_release_victim(c, victim);
  			break;
  		}
  	}

  	spin_lock(&c->lock);

  	if (ret && !c->stopping)
  		c->gc_error = ret;

  	c->gc_running = false;
  	c->gc_scheduled = false;

  	spin_unlock(&c->lock);

  	wake_up_all(&c->gc_waitq);
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
			  struct mapping_entry *entry)
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
			*entry = active_memtable -> entries[i - 1];
			return 0;
		}
	}
	
	list_for_each_entry_reverse(memtable, &c -> mapping.frozen_memtables, node){
		for(i = memtable -> entry_count; i > 0; i--){
			if(memtable -> entries[i - 1].logical_block == logical_block){
				*entry = memtable -> entries[i - 1];
				return 0;
			}
		}
	}

	for(i = c -> mapping.run_count; i > 0; i--){
		run = &c -> mapping.runs[i - 1];
		for(j = run -> entry_count; j > 0; j--){
			if(run -> entries[j - 1].logical_block == logical_block){
				*entry = run -> entries[j - 1];
				return 0;
			}
		}
	}
	return -ENOENT;
}

static int mapping_update_if_match(struct zns_base_c *c, size_t logical_block,
  				   sector_t expected_physical_sector, u64 expected_seq, sector_t new_physical_sector)
{
  	struct mapping_entry current_entry;
  	int ret;

  	ret = mapping_lookup(c, logical_block, &current_entry);
  	if (ret == -ENOENT)
  		return -ESTALE;

  	if (ret)
  		return ret;

  	if (current_entry.physical_sector != expected_physical_sector ||
  	    current_entry.seq != expected_seq)
  		return -ESTALE;

  	return mapping_update(c, logical_block, new_physical_sector);
}

static int zns_base_get_zone_slot(struct zns_base_c *c,
  				  sector_t physical_sector,
  				  struct zns_base_zone **zone_out,
  				  unsigned int *slot_out)
{
  	struct zns_base_zone *zone;
  	sector_t zone_end;
  	unsigned int i;

  	for (i = 0; i < c->zone_state.nr_zones; i++) {
  		zone = &c->zone_state.zones[i];
  		zone_end = zone->start_sector + zone->capacity_sectors;

  		if (physical_sector < zone->start_sector ||
  		    physical_sector + SECTORS_PER_BLOCK > zone_end)
  			continue;

  		if ((physical_sector - zone->start_sector) %
  		    SECTORS_PER_BLOCK != 0)
  			return -EINVAL;

  		*zone_out = zone;
  		*slot_out = (physical_sector - zone->start_sector) /
  			    SECTORS_PER_BLOCK;
  		return 0;
  	}

  	return -EINVAL;
}

static int zns_base_report_zone(struct blk_zone *zone,
  				unsigned int idx, void *data)
{
  	struct zns_base_c *c = data;
  	struct zns_base_zone *z;

  	if (idx >= c->zone_state.nr_zones)
  		return -EINVAL;

  	if (zone->type != BLK_ZONE_TYPE_SEQWRITE_REQ)
  		return -EINVAL;

  	if (zone->capacity == 0 ||
  	    zone->capacity % SECTORS_PER_BLOCK != 0)
  		return -EINVAL;

  	/*
  	 * 현재 mapping은 RAM에만 있다.
  	 * 따라서 이전 실행에서 이미 write된 zone은 복구할 수 없다.
  	 */
  	if (zone->wp != zone->start)
  		return -EBUSY;

  	z = &c->zone_state.zones[idx];
  	z->start_sector = zone->start;
  	z->capacity_sectors = zone->capacity;
  	z->write_pointer = zone->start;
  	z->nr_blocks = zone->capacity / SECTORS_PER_BLOCK;
  	z->valid_blocks = 0;
  	z->state = ZNS_BASE_ZONE_FREE;
	atomic_set(&z->inflight_reads, 0);
	init_waitqueue_head(&z->read_waitq);

  	return 0;
}

static void zns_base_zone_destroy(struct zns_base_c *c)
{
	unsigned int i;

	for(i = 0; i < c -> zone_state.nr_zones; i++)
		kvfree(c -> zone_state.zones[i].slots);
	
  	kvfree(c->zone_state.zones);
  	c->zone_state.zones = NULL;
  	c->zone_state.nr_zones = 0;
  	c->zone_state.active_zone_idx = 0;
	c->zone_state.gc_dest_zone_idx = ZNS_BASE_NO_ZONE;
}

static int zns_base_zone_init(struct zns_base_c *c)
{
  	struct request_queue *queue;
  	unsigned int nr_zones;
	unsigned int i;
  	int ret;

  	if (!bdev_is_zoned(c->dev->bdev))
  		return -EINVAL;

  	queue = bdev_get_queue(c->dev->bdev);
  	if (!queue)
  		return -ENODEV;

  	nr_zones = blk_queue_nr_zones(queue);
  	if (!nr_zones || !bdev_zone_sectors(c->dev->bdev))
  		return -EINVAL;

  	c->zone_state.zones = kvcalloc(nr_zones,
  				       sizeof(*c->zone_state.zones),
  				       GFP_KERNEL);
  	if (!c->zone_state.zones)
  		return -ENOMEM;

  	c->zone_state.nr_zones = nr_zones;
  	c->zone_state.active_zone_idx = 0;
	c -> zone_state.gc_dest_zone_idx = ZNS_BASE_NO_ZONE;

  	ret = blkdev_report_zones(c->dev->bdev, 0, nr_zones,
  				  zns_base_report_zone, c);
  	if (ret < 0) {
  		kvfree(c->zone_state.zones);
  		c->zone_state.zones = NULL;
  		c->zone_state.nr_zones = 0;
  		return ret;
  	}

  	if (ret != nr_zones) {
  		kvfree(c->zone_state.zones);
  		c->zone_state.zones = NULL;
  		c->zone_state.nr_zones = 0;
  		return -EINVAL;
  	}

	for (i = 0; i < nr_zones; i++) {
		c->zone_state.zones[i].slots =
			kvcalloc(c->zone_state.zones[i].nr_blocks,
				sizeof(struct zns_base_zone_slot),
				GFP_KERNEL);

		if (!c->zone_state.zones[i].slots) {
			zns_base_zone_destroy(c);
			return -ENOMEM;
		}
	}

  	c->zone_state.zones[0].state = ZNS_BASE_ZONE_ACTIVE;
  	return 0;
}

static unsigned int zns_base_count_free_zones(struct zns_base_c *c)
{
  	unsigned int i;
  	unsigned int free_count = 0;

  	for (i = 0; i < c->zone_state.nr_zones; i++) {
  		if (c->zone_state.zones[i].state == ZNS_BASE_ZONE_FREE)
  			free_count++;
  	}

  	return free_count;
}

static bool zns_base_gc_space_ready(struct zns_base_c *c)
{
  	bool ready;

  	spin_lock(&c->lock);

  	ready = c->stopping ||
  		c->gc_error ||
  		zns_base_count_free_zones(c) > GC_RESERVE_ZONES;

  	spin_unlock(&c->lock);

  	return ready;
}

static int zns_base_wait_for_gc_space(struct zns_base_c *c)
{
  	for (;;) {
  		spin_lock(&c->lock);

  		if (c->stopping) {
  			spin_unlock(&c->lock);
  			return -EIO;
  		}

  		if (c->gc_error) {
  			int ret = c->gc_error;

  			spin_unlock(&c->lock);
  			return ret;
  		}

  		if (zns_base_count_free_zones(c) > GC_RESERVE_ZONES) {
  			spin_unlock(&c->lock);
  			return 0;
  		}

  		spin_unlock(&c->lock);

  		/* lock 밖에서 GC를 예약해야 한다. */
  		zns_base_schedule_gc(c);

  		/*
  		 * reset 성공, GC 실패, dtr 종료 중 하나가 발생하면
  		 * 깨어나서 위 조건을 다시 검사한다.
  		 */
  		wait_event(c->gc_waitq, zns_base_gc_space_ready(c));
  	}
}

static bool zns_base_gc_needed(struct zns_base_c *c)
{
  	return zns_base_count_free_zones(c) <= GC_LOW_WATERMARK;
}

static void zns_base_schedule_gc(struct zns_base_c *c)
{
  	bool queue_gc = false;

  	spin_lock(&c->lock);

  	if (!c->stopping &&
  	    !c->gc_scheduled &&
  	    zns_base_gc_needed(c)) {
  		c->gc_scheduled = true;
  		queue_gc = true;
  	}

  	spin_unlock(&c->lock);

  	if (queue_gc)
  		queue_work(c->gc_wq, &c->gc_work);
}

static int zns_base_select_victim(struct zns_base_c *c,
  				  struct zns_base_zone **victim_out)
{
  	struct zns_base_zone *victim = NULL;
  	struct zns_base_zone *zone;
  	unsigned int i;

  	spin_lock(&c->lock);

  	for (i = 0; i < c->zone_state.nr_zones; i++) {
  		zone = &c->zone_state.zones[i];

  		/*
  		 * FULL이 아닌 zone은 모두 제외된다.
  		 * 따라서 ACTIVE, FREE, GC_DEST, GC_VICTIM은
  		 * 자동으로 후보에서 제외된다.
  		 */
  		if (zone->state != ZNS_BASE_ZONE_FULL)
  			continue;

  		/*
  		 * 모든 block이 valid이면 옮겨도 FREE zone이
  		 * 늘어나지 않으므로 GC victim으로 고르지 않는다.
  		 */
  		if (zone->valid_blocks >= zone->nr_blocks)
  			continue;

  		if (!victim ||
  		    zone->valid_blocks < victim->valid_blocks)
  			victim = zone;
  	}

  	if (!victim) {
  		spin_unlock(&c->lock);
  		return -ENOENT;
  	}

  	/*
  	 * 선택과 상태 변경을 같은 lock 안에서 처리한다.
  	 * 이후 다른 GC 작업이 같은 zone을 victim으로
  	 * 선택하지 못한다.
  	 */
  	victim->state = ZNS_BASE_ZONE_GC_VICTIM;
  	*victim_out = victim;

  	spin_unlock(&c->lock);

  	return 0;
}

static int zns_base_get_gc_destination(struct zns_base_c *c, struct zns_base_zone **destination_out)
{
  	struct zns_base_zone *zone;
  	sector_t zone_end;
  	unsigned int i;

  	spin_lock(&c->lock);

  	/*
  	 * 기존 GC destination이 있고 4KiB를 더 쓸 수 있으면
  	 * 그대로 계속 사용한다.
  	 */
  	if (c->zone_state.gc_dest_zone_idx != ZNS_BASE_NO_ZONE) {
  		zone = &c->zone_state.zones[c->zone_state.gc_dest_zone_idx];
  		zone_end = zone->start_sector + zone->capacity_sectors;

  		if (zone->state != ZNS_BASE_ZONE_GC_DEST) {
  			spin_unlock(&c->lock);
  			return -EIO;
  		}

  		if (zone->write_pointer + SECTORS_PER_BLOCK <= zone_end) {
  			*destination_out = zone;
  			spin_unlock(&c->lock);
  			return 0;
  		}

  		/*
  		 * GC destination이 꽉 찼다.
  		 * 일반 write용 ACTIVE로 바꾸지 않고 FULL로 둔다.
  		 */
  		zone->state = ZNS_BASE_ZONE_FULL;
  		c->zone_state.gc_dest_zone_idx = ZNS_BASE_NO_ZONE;
  	}

  	/*
  	 * 새 FREE zone을 GC 전용 destination으로 확보한다.
  	 * GC는 reserve zone도 목적지로 사용할 수 있어야 한다.
  	 */
  	for (i = 0; i < c->zone_state.nr_zones; i++) {
  		zone = &c->zone_state.zones[i];

  		if (zone->state != ZNS_BASE_ZONE_FREE)
  			continue;

  		zone->state = ZNS_BASE_ZONE_GC_DEST;
  		c->zone_state.gc_dest_zone_idx = i;
  		*destination_out = zone;

  		spin_unlock(&c->lock);
  		return 0;
  	}

  	spin_unlock(&c->lock);
  	return -ENOSPC;
}

static int zns_base_allocate_gc_block(struct zns_base_c *c, sector_t *physical_sector,
  				      struct zns_base_zone **zone_out, unsigned int *slot_out)
{
  	struct zns_base_zone *zone;
  	sector_t zone_end;
  	int ret;

  	ret = zns_base_get_gc_destination(c, &zone);
  	if (ret)
  		return ret;

  	spin_lock(&c->lock);

  	zone_end = zone->start_sector + zone->capacity_sectors;

  	if (zone->state != ZNS_BASE_ZONE_GC_DEST ||
  	    zone->write_pointer + SECTORS_PER_BLOCK > zone_end) {
  		spin_unlock(&c->lock);
  		return -EIO;
  	}

  	*physical_sector = zone->write_pointer;

  	ret = zns_base_get_zone_slot(c, *physical_sector, zone_out, slot_out);
  	if (!ret && (*zone_out)->slots[*slot_out].valid)
  		ret = -EIO;

  	spin_unlock(&c->lock);

  	return ret;
}

static int zns_base_commit_gc_block(struct zns_base_c *c, struct zns_base_zone *zone,
  				    sector_t physical_sector)
{
  	sector_t zone_end;

  	zone_end = zone->start_sector + zone->capacity_sectors;

  	if (zone->state != ZNS_BASE_ZONE_GC_DEST)
  		return -EIO;

  	if (zone->write_pointer != physical_sector)
  		return -EIO;

  	zone->write_pointer += SECTORS_PER_BLOCK;

  	if (zone->write_pointer > zone_end)
  		return -EIO;

  	if (zone->write_pointer == zone_end) {
  		zone->state = ZNS_BASE_ZONE_FULL;
  		c->zone_state.gc_dest_zone_idx = ZNS_BASE_NO_ZONE;
  	}

  	return 0;
}

static int zns_base_gc_move_block(struct zns_base_c *c, struct zns_base_zone *victim,
  				  unsigned int victim_slot)
{
  	struct mapping_entry expected_entry;
  	struct zns_base_zone *new_zone;
  	struct page *page;
  	sector_t old_physical_sector;
  	sector_t new_physical_sector;
  	size_t logical_block;
  	unsigned int new_slot;
  	int ret;

  	/*
  	 * victim slot과 현재 mapping의 snapshot을 잡는다.
  	 * lower I/O 중에는 lock을 잡지 않는다.
  	 */
  	spin_lock(&c->lock);

  	if (victim->state != ZNS_BASE_ZONE_GC_VICTIM ||
  	    victim_slot >= victim->nr_blocks ||
  	    !victim->slots[victim_slot].valid) {
  		spin_unlock(&c->lock);
  		return 0;
  	}

  	logical_block = victim->slots[victim_slot].logical_block;
  	old_physical_sector = victim->start_sector +
  		((sector_t)victim_slot * SECTORS_PER_BLOCK);

  	ret = mapping_lookup(c, logical_block, &expected_entry);

  	/*
  	 * reverse map slot은 valid지만 mapping이 이미 다른 PBA를
  	 * 가리키면, 이 slot은 stale data다. 복사할 필요가 없다.
  	 */
  	if (ret == -ENOENT ||
  	    (!ret &&
  	     expected_entry.physical_sector != old_physical_sector)) {
  		victim->slots[victim_slot].valid = false;
  		victim->valid_blocks--;
  		spin_unlock(&c->lock);
  		return 0;
  	}

  	if (ret) {
  		spin_unlock(&c->lock);
  		return ret;
  	}

  	spin_unlock(&c->lock);

  	/*
  	 * GC mapping publish도 MemTable 공간이 필요하다.
  	 */
  	ret = mapping_wait_for_write_slot(c);
  	if (ret)
  		return ret;

  	ret = zns_base_allocate_gc_block(c, &new_physical_sector,
  					 &new_zone, &new_slot);
  	if (ret)
  		return ret;

  	page = alloc_page(GFP_KERNEL);
  	if (!page)
  		return -ENOMEM;

  	ret = zns_base_submit_page(c, page, REQ_OP_READ,
  				   old_physical_sector);
  	if (ret)
  		goto out_free_page;

  	ret = zns_base_submit_page(c, page, REQ_OP_WRITE,
  				   new_physical_sector);
  	if (ret)
  		goto out_free_page;

  	/*
  	 * lower write 성공 뒤에만 write pointer, mapping,
  	 * reverse map을 publish한다.
  	 */
  	spin_lock(&c->lock);

  	ret = zns_base_commit_gc_block(c, new_zone,
  				       new_physical_sector);
  	if (ret){
		spin_unlock(&c->lock);
		goto out_free_page;
	}

  	/*
  	 * foreground write가 같은 LBA를 갱신했는지 확인한다.
  	 */
  	if (!victim->slots[victim_slot].valid){
  		ret = -ESTALE;
		
	}
	else if(victim->slots[victim_slot].logical_block != logical_block) {
  		ret = -EIO;
  	} 
	else {
  		ret = mapping_update_if_match(
  			c, logical_block,
  			expected_entry.physical_sector,
  			expected_entry.seq,
  			new_physical_sector);
  	}

  	if (!ret) {
  		new_zone->slots[new_slot].logical_block = logical_block;
  		new_zone->slots[new_slot].valid = true;
  		new_zone->valid_blocks++;

  		victim->slots[victim_slot].valid = false;
  		victim->valid_blocks--;
  	} else if (ret == -ESTALE) {
  		/*
  		 * 새 PBA는 이미 physical zone에 기록됐지만
  		 * mapping에 연결하지 않는다.
  		 */
  		if (victim->slots[victim_slot].valid &&
  		    victim->slots[victim_slot].logical_block ==
  		    logical_block) {
  			victim->slots[victim_slot].valid = false;
  			victim->valid_blocks--;
  		}

  		ret = 0;
  	}

	spin_unlock(&c -> lock);

  out_free_page:
  	__free_page(page);
  	return ret;
}

static int zns_base_reset_victim(struct zns_base_c *c,
  				 struct zns_base_zone *victim)
{
  	int ret;

  	spin_lock(&c->lock);

  	if (victim->state != ZNS_BASE_ZONE_GC_VICTIM ||
  	    victim->valid_blocks != 0) {
  		spin_unlock(&c->lock);
  		return -EIO;
  	}

  	spin_unlock(&c->lock);

  	/*
  	 * lower read가 완전히 끝난 뒤에만 reset한다.
  	 * 여기서는 spinlock을 잡으면 안 된다.
  	 */
  	wait_event(victim->read_waitq,
  		   atomic_read(&victim->inflight_reads) == 0);

  	ret = blkdev_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET,
  			       victim->start_sector,
  			       victim->capacity_sectors,
  			       GFP_KERNEL);
  	if (ret)
  		return ret;

  	/*
  	 * victim은 아직 GC_VICTIM 상태이므로 다른 write가
  	 * 이 zone을 사용하지 않는다. reset된 slot metadata를 비운다.
  	 */
  	memset(victim->slots, 0,
  	       victim->nr_blocks * sizeof(*victim->slots));

  	spin_lock(&c->lock);

  	victim->write_pointer = victim->start_sector;
  	victim->valid_blocks = 0;
  	victim->state = ZNS_BASE_ZONE_FREE;

  	spin_unlock(&c->lock);

  	wake_up_all(&c->gc_waitq);
  	return 0;
}

static void zns_base_release_victim(struct zns_base_c *c,
  				    struct zns_base_zone *victim)
{
  	spin_lock(&c->lock);

  	if (victim->state == ZNS_BASE_ZONE_GC_VICTIM)
  		victim->state = ZNS_BASE_ZONE_FULL;

  	spin_unlock(&c->lock);
}

static int zns_base_activate_next_zone(struct zns_base_c *c)
{
  	unsigned int i;
  	struct zns_base_zone *zone;

	if (zns_base_count_free_zones(c) <= GC_RESERVE_ZONES)
  		return -EAGAIN;

  	for (i = 0; i < c->zone_state.nr_zones; i++) {
  		zone = &c->zone_state.zones[i];

  		if (zone->state != ZNS_BASE_ZONE_FREE)
  			continue;

  		zone->state = ZNS_BASE_ZONE_ACTIVE;
  		c->zone_state.active_zone_idx = i;
  		return 0;
  	}

  	return -ENOSPC;
}

static int zns_base_allocate_block(struct zns_base_c *c,
					  sector_t *physical_sector)
{
  	struct zns_base_zone *zone;
  	sector_t zone_end;
  	int ret;

  	zone = &c->zone_state.zones[c->zone_state.active_zone_idx];
  	zone_end = zone->start_sector + zone->capacity_sectors;

  	if (zone->state != ZNS_BASE_ZONE_ACTIVE ||
  	    zone->write_pointer + SECTORS_PER_BLOCK > zone_end) {
  		zone->state = ZNS_BASE_ZONE_FULL;

		ret = zns_base_activate_next_zone(c);
  		if (ret)
  			return ret;

  		zone = &c->zone_state.zones[c->zone_state.active_zone_idx];
  	}

  	*physical_sector = zone->write_pointer;
  	return 0;
}

static int zns_base_commit_block(struct zns_base_c *c,
					sector_t physical_sector)
{
  	struct zns_base_zone *zone;
  	sector_t zone_end;

  	zone = &c->zone_state.zones[c->zone_state.active_zone_idx];
  	zone_end = zone->start_sector + zone->capacity_sectors;

  	if (zone->write_pointer != physical_sector)
  		return -EIO;

  	zone->write_pointer += SECTORS_PER_BLOCK;

  	if (zone->write_pointer > zone_end)
  		return -EIO;

  	if (zone->write_pointer == zone_end)
  		zone->state = ZNS_BASE_ZONE_FULL;

  	return 0;
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

	ret = zns_base_zone_init(c);
	if (ret) {
		ti->error = "failed to initialize zone metadata";
		mapping_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return ret;
	}

	INIT_LIST_HEAD(&c -> pending_bios);
	init_waitqueue_head(&c -> spare_waitq);
	init_waitqueue_head(&c -> gc_waitq);

	c -> io_work_scheduled = false;
	c -> gc_scheduled = false;
	c -> gc_running = false;
	c->gc_error = 0;
	c -> stopping = false;

	c->io_wq = alloc_workqueue("zns-base-io",
  			    WQ_MEM_RECLAIM, 1);

	if (!c->io_wq) {
		ti->error = "failed to allocate foreground I/O workqueue";
		zns_base_zone_destroy(c);
		mapping_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	c->gc_wq = alloc_workqueue("zns-base-gc",
					WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!c->gc_wq) {
		ti->error = "failed to allocate GC workqueue";
		destroy_workqueue(c->io_wq);
		c->io_wq = NULL;
		zns_base_zone_destroy(c);
		mapping_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	c -> io_pool = mempool_create_kmalloc_pool(IO_POOL_SIZE, sizeof(struct zns_base_io));

	if(!c -> io_pool){
		ti -> error = "failed to allocate I/O context pool";
		destroy_workqueue(c->gc_wq);
		c->gc_wq = NULL;
		destroy_workqueue(c->io_wq);
		c->io_wq = NULL;

		zns_base_zone_destroy(c);
		mapping_destroy(c);
		dm_put_device(ti, c -> dev);
		kfree(c);
		return -ENOMEM; 
	}

	INIT_WORK(&c -> io_work, zns_base_io_work);
	INIT_WORK(&c -> gc_work, zns_base_gc_work);

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
	wake_up_all(&c -> gc_waitq);

	cancel_work_sync(&c -> io_work);
	cancel_work_sync(&c -> gc_work);

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

	destroy_workqueue(c -> gc_wq);
	c -> gc_wq = NULL;
	destroy_workqueue(c -> io_wq);
	c -> io_wq = NULL;

	zns_base_zone_destroy(c);
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
