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
#include <linux/rbtree.h>
#include <linux/mempool.h>
#include <linux/wait.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/blkdev.h>
#include <linux/atomic.h>
#include <linux/crc32c.h>
#include <linux/build_bug.h>
#include <linux/byteorder/generic.h>
#include <linux/mutex.h>

#define DM_MSG_PREFIX "zns-base"
#define ZNS_BASE_BLOCK_SIZE 4096
#define ZNS_BASE_SECTOR_SIZE 512
#define SECTORS_PER_BLOCK 8
#define MEMTABLE_POOL_SIZE 4
#define INITIAL_RUN_CAPACITY 4
#define RUN_COMPACTION_THRESHOLD 8
#define IO_POOL_SIZE 128
#define GC_RESERVE_ZONES 2
#define GC_LOW_WATERMARK 3
#define GC_TARGET_FREE_ZONES 5
#define ZNS_BASE_NO_ZONE ((unsigned int)-1)
#define ZNS_BASE_MANIFEST_ZONES 2
#define ZNS_BASE_WAL_ZONES      2
#define ZNS_BASE_SSTABLE_ZONES  3
#define ZNS_BASE_METADATA_ZONES (ZNS_BASE_MANIFEST_ZONES + ZNS_BASE_WAL_ZONES + ZNS_BASE_SSTABLE_ZONES)
#define ZNS_BASE_FORMAT_VERSION 1
#define ZNS_BASE_WAL_MAGIC      0x4c41575aU // ZWAL
#define ZNS_BASE_SSTABLE_MAGIC  0x4254535aU // ZSTB
#define ZNS_BASE_MANIFEST_MAGIC 0x4e414d5aU	// ZMAN
#define ZNS_BASE_WAL_RECORD_SIZE 32
#define ZNS_BASE_SSTABLE_ENTRY_SIZE 16
#define ZNS_BASE_WAL_OP_PUT 1
#define ZNS_BASE_WAL_PAGE_MAGIC 0x4750575aU /* ZWPG */
#define ZNS_BASE_WAL_PAGE_HEADER_SIZE 64
#define ZNS_BASE_WAL_RECORDS_PER_PAGE ((ZNS_BASE_BLOCK_SIZE - ZNS_BASE_WAL_PAGE_HEADER_SIZE) / ZNS_BASE_WAL_RECORD_SIZE)

enum zns_base_failpoint {
	ZNS_BASE_FAIL_NONE = 0,
	ZNS_BASE_FAIL_AFTER_DATA_WRITE = 1,
	ZNS_BASE_FAIL_BEFORE_WAL_WRITE = 2,
	ZNS_BASE_FAIL_AFTER_SSTABLE_WRITE = 3,
	ZNS_BASE_FAIL_AFTER_MANIFEST_WRITE = 4,
	ZNS_BASE_FAIL_BEFORE_ZONE_RESET = 5,
	ZNS_BASE_FAIL_CORRUPT_WAL_PAGE_CRC = 6,
};

static unsigned int zns_base_failpoint;
module_param_named(failpoint, zns_base_failpoint, uint, 0644);
MODULE_PARM_DESC(failpoint,
	"One-shot test failpoint: 1=data, 2=WAL, 3=SSTable, 4=manifest, 5=zone reset, 6=corrupt WAL CRC");

enum zns_base_zone_state {
  	ZNS_BASE_ZONE_FREE,
  	ZNS_BASE_ZONE_ACTIVE,
  	ZNS_BASE_ZONE_FULL,
	ZNS_BASE_ZONE_GC_DEST,
  	ZNS_BASE_ZONE_GC_VICTIM,
};

enum zns_base_zone_role {
  	ZNS_BASE_ZONE_MANIFEST,
  	ZNS_BASE_ZONE_WAL,
  	ZNS_BASE_ZONE_SSTABLE,
  	ZNS_BASE_ZONE_DATA,
};

struct mapping_entry {
	size_t logical_block;
	sector_t physical_sector;
	u64 seq;
};

/* A MemTable preallocates these nodes, then links them by logical block. */
struct mapping_memtable_entry {
	struct rb_node node;
	struct mapping_entry entry;
};

/* A run is an immutable, LBA-sorted mapping snapshot from a frozen MemTable. */
struct mapping_run {
	struct mapping_entry *entries;
	size_t entry_count;
	size_t entry_capacity;
};

struct mapping_memtable {
	struct rb_root root;
	struct mapping_memtable_entry *entries;
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

struct zns_base_zone_slot {
  	size_t logical_block;
  	bool valid;
	bool pending;
};

struct zns_base_zone {
  	sector_t start_sector;
  	sector_t capacity_sectors;
  	sector_t write_pointer;
   	unsigned int nr_blocks;
   	unsigned int valid_blocks;
	unsigned int pending_blocks;
	struct zns_base_zone_slot *slots;
  	enum zns_base_zone_state state;
	enum zns_base_zone_role role;

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

	unsigned int pending_commits;
  	bool write_staging_done;
  	int write_error;
  	bool completed;
};

struct zns_base_wal_zone_header_disk {
  	__le32 magic;
  	__le16 version;
  	__le16 header_bytes;

  	__le64 generation;
  	__le64 first_seq;

  	__le32 record_bytes;
  	__le32 flags;

  	__le32 header_crc32c;
  	__le32 reserved;
} __packed;

struct zns_base_wal_page_header_disk {
  	__le32 magic;
  	__le16 version;
  	__le16 header_bytes;

  	__le64 generation;
  	__le64 first_seq;

  	__le16 record_count;
  	__le16 record_bytes;

  	__le32 payload_crc32c;
  	__le32 header_crc32c;

  	__le32 reserved[7];
} __packed;

struct zns_base_wal_record_disk {
  	__le64 logical_block;
  	__le64 physical_sector;
  	__le64 seq;

  	__le32 op_flags;
  	__le32 crc32c;
} __packed;

struct zns_base_sstable_header_disk {
  	__le32 magic;
  	__le16 version;
  	__le16 header_bytes;

  	__le64 generation;
  	__le64 entry_count;

  	__le64 min_logical_block;
  	__le64 max_logical_block;
  	__le64 max_seq;

  	__le64 entries_bytes;

  	__le32 entries_crc32c;
  	__le32 header_crc32c;
} __packed;

struct zns_base_sstable_entry_disk {
  	__le64 logical_block;
  	__le64 physical_sector;
} __packed;

struct zns_base_manifest_header_disk {
  	__le32 magic;
  	__le16 version;
  	__le16 header_bytes;

  	__le64 generation;
  	__le64 checkpoint_last_seq;

  	__le32 wal_zone_idx;
  	__le32 wal_record_idx;

  	__le64 descriptor_bytes;

  	__le32 sstable_count;
  	__le32 descriptors_crc32c;

  	__le32 header_crc32c;
  	__le32 reserved[3];
} __packed;

struct zns_base_sstable_descriptor_disk {
  	__le32 zone_idx;
  	__le32 flags;

  	__le64 start_sector;
  	__le64 length_bytes;

  	__le64 min_logical_block;
  	__le64 max_logical_block;
  	__le64 generation;

  	__le32 payload_crc32c;
  	__le32 reserved;
} __packed;

struct zns_base_metadata_stream {
  	enum zns_base_zone_role role;
  	unsigned int first_zone_idx;
  	unsigned int zone_count;
  	unsigned int active_zone_idx;
  	u64 generation;
};

enum zns_base_wal_commit_type {
  	ZNS_BASE_WAL_COMMIT_FOREGROUND,
  	ZNS_BASE_WAL_COMMIT_GC,
};

struct zns_base_wal_pending_commit {
  	struct list_head node;

  	enum zns_base_wal_commit_type type;

  	size_t logical_block;
  	sector_t new_physical_sector;
  	u64 seq;

  	struct zns_base_zone *new_zone;
  	unsigned int new_slot;

  	bool had_old_mapping;
  	struct zns_base_zone *old_zone;
  	unsigned int old_slot;

  	sector_t expected_physical_sector;
  	u64 expected_seq;

  	/* 다음 단계에서 원본 bio 완료 상태를 연결한다. */
  	struct zns_base_io *io;

	int result;
};

struct zns_base_wal_state {
  	struct zns_base_metadata_stream stream;
  	bool header_written;

	u8 *page_buffer;
  	unsigned int record_count;
  	u64 first_seq;

  	struct list_head pending_commits;

  	struct delayed_work flush_work;
  	bool flush_scheduled;
	int flush_error;
  };

struct zns_base_metadata_state {
  	struct zns_base_metadata_stream manifest;
  	struct zns_base_wal_state wal;
   struct zns_base_metadata_stream sstable;
	u64 checkpoint_seq;
	u64 checkpoint_generation;
	unsigned int checkpoint_sstable_zone_idx;
	unsigned int checkpoint_manifest_zone_idx;
   struct mutex lock;
};

struct zns_base_c {
	struct dm_dev *dev;

	struct zns_base_zone_state_table zone_state;

	struct mapping_state mapping;
	size_t nr_logical_blocks;

	struct list_head pending_bios;
	
	struct workqueue_struct *io_wq;
	struct workqueue_struct *gc_wq;
	struct workqueue_struct *wal_wq;
	
	struct work_struct io_work;
	struct work_struct gc_work;
	

	struct zns_base_metadata_state metadata;

	bool io_work_scheduled;
	bool gc_scheduled;
	bool gc_running;
	int gc_error;
	int gc_last_error;
	u64 gc_runs;
	u64 gc_reset_count;
	u64 gc_moved_blocks;

	mempool_t *io_pool;
	bool stopping;

	wait_queue_head_t spare_waitq;
	wait_queue_head_t gc_waitq;

	// 나중에 io_lock이랑 mapping_lock이랑 나누기.
	spinlock_t lock;
	struct mutex mapping_wal_lock;
};



static int mapping_update(struct zns_base_c *c, size_t logical_block, sector_t physical_sector, u64 seq);
static int mapping_lookup(struct zns_base_c *c, size_t logical_block, struct mapping_entry *entry);
static int mapping_lookup_visible(struct zns_base_c *c, size_t logical_block,
				  struct mapping_entry *entry);
static int mapping_update_if_match(struct zns_base_c *c, size_t logical_block,
  				   sector_t expected_physical_sector, u64 expected_seq, sector_t new_physical_sector, u64 seq);
static int mapping_wait_for_write_slot(struct zns_base_c *c,
				       size_t logical_block);
static struct mapping_memtable_entry *
mapping_memtable_find(struct mapping_memtable *memtable,
			      size_t logical_block);
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
static bool zns_base_metadata_has_space(
  	struct zns_base_c *c,
  	struct zns_base_metadata_stream *stream,
  	unsigned int blocks);
static int zns_base_wal_rotate(struct zns_base_c *c);
static int zns_base_checkpoint_locked(struct zns_base_c *c);
static int zns_base_replay_wal_put(struct zns_base_c *c,
				   size_t logical_block, sector_t physical_sector, u64 seq);
static void zns_base_wal_flush_work(struct work_struct *work);
static void zns_base_wal_abort_pending(struct zns_base_c *c, int error);
static int zns_base_wal_flush_sync(struct zns_base_c *c);
static int zns_base_wal_stage_foreground(struct zns_base_c *c,
		struct zns_base_io *io, size_t logical_block,
		sector_t new_physical_sector, struct zns_base_zone *new_zone,
		unsigned int new_slot, bool *page_full);
static int zns_base_wal_stage_gc(struct zns_base_c *c,
		size_t logical_block, sector_t new_physical_sector,
		struct zns_base_zone *new_zone, unsigned int new_slot,
		struct zns_base_zone *old_zone, unsigned int old_slot,
		const struct mapping_entry *expected_entry, bool *page_full);
static int zns_base_wal_publish_gc_locked(struct zns_base_c *c,
		struct zns_base_wal_pending_commit *commit);
static int zns_base_reserve_pending_slot_locked(struct zns_base_zone *zone,
		unsigned int slot, size_t logical_block);
static void zns_base_release_pending_slot(struct zns_base_c *c,
		struct zns_base_zone *zone, unsigned int slot,
		size_t logical_block);

static bool zns_base_failpoint_hit(enum zns_base_failpoint point)
{
	if (cmpxchg(&zns_base_failpoint, point,
		    ZNS_BASE_FAIL_NONE) != point)
		return false;

	DMWARN("test failpoint %u triggered", point);
	return true;
}

static enum zns_base_zone_role zns_base_zone_role_from_index(unsigned int idx)
{
  	if (idx < ZNS_BASE_MANIFEST_ZONES)
  		return ZNS_BASE_ZONE_MANIFEST;

  	if (idx < ZNS_BASE_MANIFEST_ZONES + ZNS_BASE_WAL_ZONES)
  		return ZNS_BASE_ZONE_WAL;

  	if (idx < ZNS_BASE_METADATA_ZONES)
  		return ZNS_BASE_ZONE_SSTABLE;

  	return ZNS_BASE_ZONE_DATA;
}

static void zns_base_check_disk_format(void)
{
  	BUILD_BUG_ON(sizeof(struct zns_base_wal_zone_header_disk) != 40);
  	BUILD_BUG_ON(sizeof(struct zns_base_wal_record_disk) != 32);
  	BUILD_BUG_ON(sizeof(struct zns_base_sstable_header_disk) != 64);
  	BUILD_BUG_ON(sizeof(struct zns_base_sstable_entry_disk) != 16);
  	BUILD_BUG_ON(sizeof(struct zns_base_manifest_header_disk) != 64);
  	BUILD_BUG_ON(sizeof(struct zns_base_sstable_descriptor_disk) != 56);
	BUILD_BUG_ON(sizeof(struct zns_base_wal_page_header_disk) != 64);
  	BUILD_BUG_ON(ZNS_BASE_WAL_PAGE_HEADER_SIZE + ZNS_BASE_WAL_RECORDS_PER_PAGE * ZNS_BASE_WAL_RECORD_SIZE !=
  	     ZNS_BASE_BLOCK_SIZE);
}

static sector_t zns_base_usable_logical_sectors(struct zns_base_c *c)
{
  	sector_t capacity = 0;
  	unsigned int i;

  	for (i = 0; i < c->zone_state.nr_zones; i++) {
  		if (c->zone_state.zones[i].role ==
  		    ZNS_BASE_ZONE_DATA)
  			capacity +=
  				c->zone_state.zones[i].capacity_sectors;
  	}

  	/*
  	 * 현재 null_blk처럼 모든 data zone 크기가 동일하다는
  	 * 첫 구현 전제다.
  	 */
  	capacity -= (sector_t)GC_RESERVE_ZONES *
  		c->zone_state.zones[ZNS_BASE_METADATA_ZONES]
  			.capacity_sectors;

  	return capacity;
}
  
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

static void zns_base_io_try_complete(
  	struct zns_base_c *c, struct zns_base_io *io)
{
  	bool complete = false;
  	int error;

  	spin_lock(&c->lock);

  	if (io->write_staging_done &&
  	    io->pending_commits == 0 &&
  	    !io->completed) {
  		io->completed = true;
  		error = io->write_error;
  		complete = true;
  	}

  	spin_unlock(&c->lock);

  	if (!complete)
  		return;

  	if (error)
  		bio_io_error(io->bio);
  	else
  		bio_endio(io->bio);

  	mempool_free(io, c->io_pool);
}	

static void zns_base_io_add_pending_commit(
  	struct zns_base_c *c,
  	struct zns_base_io *io)
{
  	spin_lock(&c->lock);
  	io->pending_commits++;
  	spin_unlock(&c->lock);
}

  static void zns_base_io_finish_staging(
  	struct zns_base_c *c,
  	struct zns_base_io *io,
  	int error)
{
  	spin_lock(&c->lock);

  	if (error && !io->write_error)
  		io->write_error = error;

  	io->write_staging_done = true;

  	spin_unlock(&c->lock);

  	zns_base_io_try_complete(c, io);
}

  static void zns_base_io_finish_commit(
  	struct zns_base_c *c,
  	struct zns_base_io *io,
  	int error)
{
  	spin_lock(&c->lock);

  	if (error && !io->write_error)
  		io->write_error = error;

  	if (WARN_ON_ONCE(!io->pending_commits)) {
  		spin_unlock(&c->lock);
  		return;
  	}

  	io->pending_commits--;

  	spin_unlock(&c->lock);

  	zns_base_io_try_complete(c, io);
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

	io->pending_commits = 0;
	io->write_staging_done = false;
	io->write_error = 0;
	io->completed = false;

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

static int zns_base_submit_clone_range(struct zns_base_c *c, struct bio *bio,
						unsigned int bio_offset_bytes, unsigned int length_bytes,
  				       	sector_t physical_sector, unsigned int op_flags)
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
	clone->bi_opf |= op_flags;

  	ret = submit_bio_wait(clone);
  	bio_put(clone);

  	return ret;
}

static int zns_base_submit_page(struct zns_base_c *c,
  				struct page *page,
  				unsigned int op, unsigned int op_flags,
  				sector_t physical_sector)
{
  	struct bio *page_bio;
  	int added;
  	int ret;

  	page_bio = bio_alloc(GFP_KERNEL, 1);
  	if (!page_bio)
  		return -ENOMEM;

  	bio_set_dev(page_bio, c->dev->bdev);
  	bio_set_op_attrs(page_bio, op, op_flags);
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

	ret = mapping_lookup_visible(c, chunk->logical_block, &entry);
	spin_lock(&c->lock);
	if (!ret) {
		physical_sector = entry.physical_sector;
		ret = zns_base_get_zone_slot(c, physical_sector, &zone, &slot);
		if (!ret &&
		    (zone->slots[slot].valid || zone->slots[slot].pending) &&
		    zone->slots[slot].logical_block == chunk->logical_block) {
			zns_base_zone_read_get(zone);
			zone_pinned = true;
		} else if (!ret) {
			ret = -EIO;
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
  					   physical_sector, 0);
	if (zone_pinned)
		zns_base_zone_read_put(zone);

	return ret;
}

/* Makes all completed lower-device writes durable before a WAL publish. */
static int zns_base_submit_flush(struct zns_base_c *c)
{
	struct bio *flush_bio;
	int ret;

	flush_bio = bio_alloc(GFP_KERNEL, 0);
	if (!flush_bio)
		return -ENOMEM;

	bio_set_dev(flush_bio, c->dev->bdev);
	bio_set_op_attrs(flush_bio, REQ_OP_FLUSH, 0);

	ret = submit_bio_wait(flush_bio);
	bio_put(flush_bio);
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
				struct zns_base_io *io, struct zns_base_chunk *chunk)
{
	struct bio *bio = io->bio;
  	sector_t new_physical_sector;
	struct mapping_entry old_entry;
	struct zns_base_zone *old_zone;
  	struct zns_base_zone *new_zone;
  	struct page *scratch_page;
  	void *scratch_addr;
  	bool full_block;
	unsigned int old_slot;
  	unsigned int new_slot;
	bool old_zone_pinned;
	int ret;

  	full_block = chunk->block_offset_bytes == 0 &&
  		     chunk->length_bytes == ZNS_BASE_BLOCK_SIZE;
  	scratch_page = NULL;
	old_zone_pinned = false;

  	if (!full_block) {
  		scratch_page = alloc_page(GFP_KERNEL);
  		if (!scratch_page)
  			return -ENOMEM;

		ret = mapping_lookup_visible(c, chunk->logical_block, &old_entry);
		spin_lock(&c->lock);

		if (ret == -ENOENT) {
			clear_highpage(scratch_page);
			ret = 0;
		} 
		else if (!ret) {
			ret = zns_base_get_zone_slot(c, old_entry.physical_sector,
										&old_zone, &old_slot);

			if (!ret &&
				((!old_zone->slots[old_slot].valid &&
				  !old_zone->slots[old_slot].pending) ||
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
			ret = zns_base_submit_page(c, scratch_page, REQ_OP_READ, 0,
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

	  	ret = mapping_wait_for_write_slot(c, chunk->logical_block);
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
	ret = zns_base_get_zone_slot(c, new_physical_sector,
				     &new_zone, &new_slot);
	if (!ret && (new_zone->slots[new_slot].valid ||
		     new_zone->slots[new_slot].pending))
		ret = -EIO;
	spin_unlock(&c->lock);

  	if (ret)
  		goto out_free_page;

  	if (full_block) {
		ret = zns_base_submit_clone_range(c, bio,
					  chunk->bio_offset_bytes,
					  chunk->length_bytes,
					  new_physical_sector, 0);
  	} else {
  		ret = zns_base_submit_page(c, scratch_page,
					   REQ_OP_WRITE, 0,
  					   new_physical_sector);
  	}

  	if (ret)
  		goto out_free_page;

	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_DATA_WRITE)) {
		ret = -EIO;
		goto out_free_page;
	}

	/* Make the new PBA visible to the WAL overlay before it can become GC victim. */
	spin_lock(&c->lock);
	ret = zns_base_commit_block(c, new_physical_sector);
	if (!ret)
		ret = zns_base_reserve_pending_slot_locked(new_zone, new_slot,
							  chunk->logical_block);
	spin_unlock(&c->lock);
	if (ret)
		goto out_free_page;

	/*
	 * WAL MVP policy: persist one PUT record per 4 KiB WAL page. The page
	 * buffer is reset before staging, so every unused record slot is zero.
	 * Mapping publication remains deferred until this flush succeeds.
	 */
	ret = zns_base_wal_stage_foreground(c, io, chunk->logical_block,
					   new_physical_sector, new_zone, new_slot, NULL);
	if (!ret)
		ret = zns_base_wal_flush_sync(c);
	if (ret)
		zns_base_release_pending_slot(c, new_zone, new_slot,
					      chunk->logical_block);
	
  out_free_page:
  	if (scratch_page)
  		__free_page(scratch_page);

  	return ret;
}

static void zns_base_process_write_bio(struct zns_base_c *c,
  				       struct zns_base_io *io)
{
  	struct bio *bio = io->bio;
  	struct zns_base_chunk chunk;
  	sector_t current_sector;
  	unsigned int remaining_bytes;
  	unsigned int bio_offset_bytes;
  	int ret;

  	if (bio->bi_iter.bi_size % ZNS_BASE_SECTOR_SIZE) {
  		zns_base_io_finish_staging(c, io, -EOPNOTSUPP);
  		return;
  	}

  	current_sector = bio->bi_iter.bi_sector;
  	remaining_bytes = bio->bi_iter.bi_size;
  	bio_offset_bytes = 0;

  	while (remaining_bytes > 0) {
  		zns_base_next_chunk(current_sector, remaining_bytes,
  				    bio_offset_bytes, &chunk);

  		if (chunk.logical_block >= c->nr_logical_blocks) {
  			zns_base_io_finish_staging(c, io, -EIO);
  			return;
  		}

  		ret = zns_base_write_chunk(c, io, &chunk);
  		if (ret) {
  			zns_base_io_finish_staging(c, io, ret);
  			return;
  		}

  		current_sector +=
  			chunk.length_bytes / ZNS_BASE_SECTOR_SIZE;
  		remaining_bytes -= chunk.length_bytes;
  		bio_offset_bytes += chunk.length_bytes;
  	}

  	zns_base_io_finish_staging(c, io, 0);
}

static bool mapping_write_ready(struct zns_base_c *c, size_t logical_block){
	bool ready;

	spin_lock(&c -> lock);

	ready = c -> stopping ||
			c -> mapping.flush_error ||
			c -> mapping.active_memtable -> entry_count < c -> mapping.active_memtable -> entry_capacity ||
			mapping_memtable_find(c->mapping.active_memtable,
					       logical_block) ||
			!list_empty(&c -> mapping.spare_memtables);
		
	spin_unlock(&c -> lock);

	return ready;
}

static int mapping_wait_for_write_slot(struct zns_base_c *c,
				       size_t logical_block){
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
			mapping_memtable_find(c->mapping.active_memtable,
					       logical_block) ||
			!list_empty(&c -> mapping.spare_memtables)){
				spin_unlock(&c -> lock);
				return 0;
		}

		spin_unlock(&c -> lock);

		wait_event(c -> spare_waitq,
			   mapping_write_ready(c, logical_block));
	}
}

static bool zns_base_process_bio(struct zns_base_c *c, struct zns_base_io *io){
	struct bio *bio = io->bio;
	int ret;

	/* Student work goes here: translate random writes into sequential ones. */

	if(bio_op(bio) == REQ_OP_FLUSH){
		ret = zns_base_wal_flush_sync(c);
		if (!ret)
			ret = zns_base_submit_clone(c, bio, bio -> bi_iter.bi_sector);
		if(ret)
			bio_io_error(bio);
		else
			bio_endio(bio);		
		return false;
	}
	// write bio 처리
	else if(bio_op(bio) == REQ_OP_WRITE){
		zns_base_process_write_bio(c, io);
		return true;
	}
	// read bio 처리
	else if(bio_op(bio) == REQ_OP_READ){
		zns_base_process_read_bio(c, bio);
		return false;
	}
	bio->bi_status = BLK_STS_NOTSUPP;
	bio_endio(bio);
	return false;
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

		if (!zns_base_process_bio(c, io))
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
	c->gc_runs++;
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

		/* Publish every staged GC move before the victim can be reset. */
		ret = zns_base_wal_flush_sync(c);
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

	if (ret && !c->stopping) {
		c->gc_error = ret;
		c->gc_last_error = ret;
		DMERR("GC stopped with error %d", ret);
	}

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

static struct mapping_memtable_entry *
mapping_memtable_find(struct mapping_memtable *memtable,
			      size_t logical_block)
{
	struct rb_node *node = memtable->root.rb_node;
	struct mapping_memtable_entry *entry;

	while (node) {
		entry = rb_entry(node, struct mapping_memtable_entry, node);

		if (logical_block < entry->entry.logical_block)
			node = node->rb_left;
		else if (logical_block > entry->entry.logical_block)
			node = node->rb_right;
		else
			return entry;
	}

	return NULL;
}

/* Caller holds c->lock. Returns -ENOSPC only for a new key in a full tree. */
static int mapping_memtable_upsert(struct mapping_memtable *memtable,
				   size_t logical_block,
				   sector_t physical_sector, u64 seq)
{
	struct rb_node **link = &memtable->root.rb_node;
	struct rb_node *parent = NULL;
	struct mapping_memtable_entry *entry;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct mapping_memtable_entry, node);

		if (logical_block < entry->entry.logical_block)
			link = &(*link)->rb_left;
		else if (logical_block > entry->entry.logical_block)
			link = &(*link)->rb_right;
		else {
			entry->entry.physical_sector = physical_sector;
			entry->entry.seq = seq;
			return 0;
		}
	}

	if (memtable->entry_count >= memtable->entry_capacity)
		return -ENOSPC;

	entry = &memtable->entries[memtable->entry_count++];
	entry->entry.logical_block = logical_block;
	entry->entry.physical_sector = physical_sector;
	entry->entry.seq = seq;
	rb_link_node(&entry->node, parent, link);
	rb_insert_color(&entry->node, &memtable->root);

	return 0;
}

static void mapping_memtable_reset(struct mapping_memtable *memtable)
{
	memtable->root = RB_ROOT;
	memtable->entry_count = 0;
}

static void mapping_memtable_copy_sorted(struct mapping_memtable *memtable,
					 struct mapping_entry *entries)
{
	struct rb_node *node;
	struct mapping_memtable_entry *entry;
	size_t i = 0;

	for (node = rb_first(&memtable->root); node; node = rb_next(node)) {
		entry = rb_entry(node, struct mapping_memtable_entry, node);
		entries[i++] = entry->entry;
	}
}

static bool mapping_run_lookup(const struct mapping_run *run,
			       size_t logical_block,
			       struct mapping_entry *entry)
{
	size_t left = 0;
	size_t right = run->entry_count;

	while (left < right) {
		size_t middle = left + (right - left) / 2;
		const struct mapping_entry *candidate = &run->entries[middle];

		if (candidate->logical_block < logical_block)
			left = middle + 1;
		else
			right = middle;
	}

	if (left == run->entry_count ||
	    run->entries[left].logical_block != logical_block)
		return false;

	*entry = run->entries[left];
	return true;
}

static int mapping_compact_runs(struct zns_base_c *c)
{
	struct mapping_run *runs;
	struct mapping_entry *merged;
	struct mapping_entry *scratch;
	struct mapping_entry **old_entries;
	size_t run_count;
	size_t total_entries = 0;
	size_t merged_count;
	size_t i;
	int ret = 0;

	spin_lock(&c->lock);
	if (c->mapping.run_count < RUN_COMPACTION_THRESHOLD) {
		spin_unlock(&c->lock);
		return 0;
	}

	run_count = c->mapping.run_count;
	runs = c->mapping.runs;
	for (i = 0; i < run_count; i++)
		total_entries += runs[i].entry_count;
	spin_unlock(&c->lock);

	merged = kvcalloc(total_entries, sizeof(*merged), GFP_KERNEL);
	scratch = kvcalloc(total_entries, sizeof(*scratch), GFP_KERNEL);
	old_entries = kvcalloc(run_count, sizeof(*old_entries), GFP_KERNEL);
	if (!merged || !scratch || !old_entries) {
		ret = -ENOMEM;
		goto out_free;
	}

	merged_count = runs[0].entry_count;
	memcpy(merged, runs[0].entries, merged_count * sizeof(*merged));

	for (i = 1; i < run_count; i++) {
		size_t left = 0;
		size_t right = 0;
		size_t out = 0;

		while (left < merged_count && right < runs[i].entry_count) {
			const struct mapping_entry *older = &merged[left];
			const struct mapping_entry *newer = &runs[i].entries[right];

			if (older->logical_block < newer->logical_block)
				scratch[out++] = merged[left++];
			else if (older->logical_block > newer->logical_block)
				scratch[out++] = runs[i].entries[right++];
			else {
				scratch[out++] = older->seq >= newer->seq ?
					*older : *newer;
				left++;
				right++;
			}
		}

		while (left < merged_count)
			scratch[out++] = merged[left++];
		while (right < runs[i].entry_count)
			scratch[out++] = runs[i].entries[right++];

		memcpy(merged, scratch, out * sizeof(*merged));
		merged_count = out;
	}

	spin_lock(&c->lock);
	if (c->mapping.run_count != run_count || c->mapping.runs != runs) {
		spin_unlock(&c->lock);
		ret = -EAGAIN;
		goto out_free;
	}

	for (i = 0; i < run_count; i++)
		old_entries[i] = c->mapping.runs[i].entries;

	c->mapping.runs[0].entries = merged;
	c->mapping.runs[0].entry_count = merged_count;
	c->mapping.runs[0].entry_capacity = merged_count;
	c->mapping.run_count = 1;
	spin_unlock(&c->lock);

	for (i = 0; i < run_count; i++)
		kvfree(old_entries[i]);
	merged = NULL;

out_free:
	kvfree(old_entries);
	kvfree(scratch);
	kvfree(merged);
	return ret;
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

		/* The rbtree is already unique and sorted by logical block. */
		mapping_memtable_copy_sorted(memtable, run.entries);

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
		mapping_memtable_reset(memtable);
		list_add_tail(&memtable->node, &c->mapping.spare_memtables);
		c -> mapping.spare_count++;

		spin_unlock(&c -> lock);

		ret = mapping_compact_runs(c);
		if (ret && ret != -EAGAIN)
			DMWARN("run compaction skipped: %d", ret);

		wake_up_all(&c -> spare_waitq);
	}
}

static int mapping_init(struct zns_base_c *c, sector_t target_sectors)
{
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
		memtable -> entries = kvcalloc(mem_capacity,
					       sizeof(*memtable->entries), GFP_KERNEL);
		if(memtable -> entries == NULL){
			mapping_memtable_free(memtable);
			ret = -ENOMEM; 
			goto out_free_pool;
		}
		memtable -> root = RB_ROOT;
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
			  sector_t physical_sector, u64 seq)
{
	struct mapping_memtable *active_memtable;
	struct mapping_memtable *new_active;
	int ret;

	active_memtable = c -> mapping.active_memtable;

	/* Existing logical blocks are updated in place and do not consume a slot. */
	ret = mapping_memtable_upsert(active_memtable, logical_block,
					 physical_sector, seq);
	if (ret != -ENOSPC)
		return ret;

	if (ret == -ENOSPC) {
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

	return mapping_memtable_upsert(active_memtable, logical_block,
					 physical_sector, seq);
}

static int mapping_lookup(struct zns_base_c *c, size_t logical_block,
			  struct mapping_entry *entry)
{
	size_t i;
	struct mapping_memtable *active_memtable;
	struct mapping_memtable *memtable;
	struct mapping_run *run;
	struct mapping_memtable_entry *memtable_entry;

	active_memtable = c -> mapping.active_memtable;
	memtable_entry = mapping_memtable_find(active_memtable, logical_block);
	if (memtable_entry) {
		*entry = memtable_entry->entry;
		return 0;
	}
	
	list_for_each_entry_reverse(memtable, &c -> mapping.frozen_memtables, node){
		memtable_entry = mapping_memtable_find(memtable, logical_block);
		if (memtable_entry) {
			*entry = memtable_entry->entry;
			return 0;
		}
	}

	for(i = c -> mapping.run_count; i > 0; i--){
		run = &c -> mapping.runs[i - 1];
		if (mapping_run_lookup(run, logical_block, entry))
			return 0;
	}
	return -ENOENT;
}

/* Includes writes that reached the data zone but are waiting for WAL FUA. */
static int mapping_lookup_visible(struct zns_base_c *c, size_t logical_block,
				  struct mapping_entry *entry)
{
	struct zns_base_wal_pending_commit *commit;
	int ret;

	mutex_lock(&c->metadata.lock);
	list_for_each_entry_reverse(commit, &c->metadata.wal.pending_commits,
				    node) {
		if (commit->logical_block != logical_block)
			continue;

		entry->logical_block = logical_block;
		entry->physical_sector = commit->new_physical_sector;
		entry->seq = commit->seq;
		mutex_unlock(&c->metadata.lock);
		return 0;
	}
	mutex_unlock(&c->metadata.lock);

	spin_lock(&c->lock);
	ret = mapping_lookup(c, logical_block, entry);
	spin_unlock(&c->lock);
	return ret;
}

static int mapping_update_if_match(struct zns_base_c *c, size_t logical_block,
  				   sector_t expected_physical_sector, u64 expected_seq, sector_t new_physical_sector, u64 seq)
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

  	return mapping_update(c, logical_block, new_physical_sector, seq);
}

static int zns_base_get_zone_slot(struct zns_base_c *c,
  				  sector_t physical_sector,
  				  struct zns_base_zone **zone_out,
  				  unsigned int *slot_out)
{
  	struct zns_base_zone *zone;
  	sector_t zone_end;
  	unsigned int i;

  	for (i = ZNS_BASE_METADATA_ZONES; i < c->zone_state.nr_zones; i++) {
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

	z = &c->zone_state.zones[idx];
	z->start_sector = zone->start;
	z->capacity_sectors = zone->capacity;
	/* Recovery uses the device-reported write pointer as the source of truth. */
	z->write_pointer = zone->wp;
	z->nr_blocks = zone->capacity / SECTORS_PER_BLOCK;
	z->valid_blocks = 0;
	z->pending_blocks = 0;
	z->role = zns_base_zone_role_from_index(idx);
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

	if (nr_zones <= ZNS_BASE_METADATA_ZONES + GC_RESERVE_ZONES)
  		return -ENOSPC;

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
		if (c->zone_state.zones[i].role != ZNS_BASE_ZONE_DATA)
  			continue;
		c->zone_state.zones[i].slots =
			kvcalloc(c->zone_state.zones[i].nr_blocks,
				sizeof(struct zns_base_zone_slot),
				GFP_KERNEL);

		if (!c->zone_state.zones[i].slots) {
			zns_base_zone_destroy(c);
			return -ENOMEM;
		}
	}

	c->zone_state.active_zone_idx = ZNS_BASE_METADATA_ZONES;
	c->zone_state.zones[ZNS_BASE_METADATA_ZONES].state = ZNS_BASE_ZONE_ACTIVE;
  	return 0;
}

static unsigned int zns_base_count_free_zones(struct zns_base_c *c)
{
  	unsigned int i;
  	unsigned int free_count = 0;

  	for (i = ZNS_BASE_METADATA_ZONES; i < c->zone_state.nr_zones; i++) {
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

  	for (i = ZNS_BASE_METADATA_ZONES; i < c->zone_state.nr_zones; i++) {
  		zone = &c->zone_state.zones[i];

  		/*
  		 * FULL이 아닌 zone은 모두 제외된다.
  		 * 따라서 ACTIVE, FREE, GC_DEST, GC_VICTIM은
  		 * 자동으로 후보에서 제외된다.
  		 */
		if (zone->state != ZNS_BASE_ZONE_FULL)
			continue;

		if (zone->pending_blocks != 0)
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
  	for (i = ZNS_BASE_METADATA_ZONES; i < c->zone_state.nr_zones; i++) {
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
	if (!ret && ((*zone_out)->slots[*slot_out].valid ||
		     (*zone_out)->slots[*slot_out].pending))
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

	ret = zns_base_allocate_gc_block(c, &new_physical_sector,
					 &new_zone, &new_slot);
  	if (ret)
  		return ret;

  	page = alloc_page(GFP_KERNEL);
  	if (!page)
  		return -ENOMEM;

  	ret = zns_base_submit_page(c, page, REQ_OP_READ, 0,
  				   old_physical_sector);
  	if (ret)
  		goto out_free_page;

	ret = zns_base_submit_page(c, page, REQ_OP_WRITE, 0,
  				   new_physical_sector);
  	if (ret)
  		goto out_free_page;

	spin_lock(&c->lock);
	ret = zns_base_commit_gc_block(c, new_zone,
				       new_physical_sector);
	if (!ret)
		ret = zns_base_reserve_pending_slot_locked(new_zone, new_slot,
							  logical_block);
	spin_unlock(&c->lock);

	if (ret)
		goto out_free_page;

	ret = zns_base_wal_stage_gc(c, logical_block, new_physical_sector,
					    new_zone, new_slot, victim, victim_slot,
					    &expected_entry, NULL);
	if (ret) {
		zns_base_release_pending_slot(c, new_zone, new_slot,
					      logical_block);
		goto out_free_page;
	}

	/* GC migration uses the same one-record durable WAL policy. */
	ret = zns_base_wal_flush_sync(c);
	
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

	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_BEFORE_ZONE_RESET))
		return -EIO;

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
	victim->pending_blocks = 0;
  	victim->state = ZNS_BASE_ZONE_FREE;
	c->gc_reset_count++;

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

  	for (i = ZNS_BASE_METADATA_ZONES; i < c->zone_state.nr_zones; i++) {
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
	if (zone->role != ZNS_BASE_ZONE_DATA)
  		return -EIO;
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
	if (zone->role != ZNS_BASE_ZONE_DATA)
  		return -EIO;
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

static void zns_base_metadata_stream_init(
  	struct zns_base_metadata_stream *stream,
  	enum zns_base_zone_role role,
  	unsigned int first_zone_idx,
  	unsigned int zone_count)
{
  	stream->role = role;
  	stream->first_zone_idx = first_zone_idx;
  	stream->zone_count = zone_count;
  	stream->active_zone_idx = first_zone_idx;
  	stream->generation = 1;
}

static int zns_base_metadata_init(struct zns_base_c *c)
{
  	mutex_init(&c->metadata.lock);

  	zns_base_metadata_stream_init(&c->metadata.manifest,
  		ZNS_BASE_ZONE_MANIFEST, 0,
  		ZNS_BASE_MANIFEST_ZONES);

  	zns_base_metadata_stream_init(&c->metadata.wal.stream,
  		ZNS_BASE_ZONE_WAL, ZNS_BASE_MANIFEST_ZONES,
  		ZNS_BASE_WAL_ZONES);

  	zns_base_metadata_stream_init(&c->metadata.sstable,
  		ZNS_BASE_ZONE_SSTABLE,
  		ZNS_BASE_MANIFEST_ZONES + ZNS_BASE_WAL_ZONES,
  		ZNS_BASE_SSTABLE_ZONES);

	c -> metadata.wal.header_written = false;

	c->metadata.wal.page_buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!c->metadata.wal.page_buffer)
		return -ENOMEM;

	c->metadata.wal.record_count = 0;
	c->metadata.wal.first_seq = 0;
	INIT_LIST_HEAD(&c->metadata.wal.pending_commits);

	INIT_DELAYED_WORK(&c->metadata.wal.flush_work, zns_base_wal_flush_work);

	c->metadata.wal.flush_scheduled = false;
  	c->metadata.wal.flush_error = 0;
	c->metadata.checkpoint_seq = 0;
	c->metadata.checkpoint_generation = 0;
	c->metadata.checkpoint_sstable_zone_idx = ZNS_BASE_NO_ZONE;
	c->metadata.checkpoint_manifest_zone_idx = ZNS_BASE_NO_ZONE;

	c -> zone_state.zones[c -> metadata.manifest.active_zone_idx].state =
			ZNS_BASE_ZONE_ACTIVE;
	c -> zone_state.zones[c -> metadata.wal.stream.active_zone_idx].state = 
			ZNS_BASE_ZONE_ACTIVE;
	c -> zone_state.zones[c -> metadata.sstable.active_zone_idx].state =
			ZNS_BASE_ZONE_ACTIVE;

  	return 0;
}

static void zns_base_metadata_destroy(struct zns_base_c *c)
{
  	kvfree(c->metadata.wal.page_buffer);
  	c->metadata.wal.page_buffer = NULL;
  	c->metadata.wal.record_count = 0;
  	c->metadata.wal.first_seq = 0;
	c->metadata.wal.flush_scheduled = false;
  	c->metadata.wal.flush_error = 0;
}

static struct zns_base_wal_record_disk 
	*zns_base_wal_record_at(struct zns_base_wal_state *wal, unsigned int index)
{
  	u8 *records;

  	if (index >= ZNS_BASE_WAL_RECORDS_PER_PAGE)
  		return NULL;

  	records = wal->page_buffer +
  		sizeof(struct zns_base_wal_page_header_disk);

  	return (struct zns_base_wal_record_disk *)
  		(records + index * ZNS_BASE_WAL_RECORD_SIZE);
}

  static void zns_base_wal_reset_page_locked(struct zns_base_wal_state *wal)
{
  	memset(wal->page_buffer, 0, ZNS_BASE_BLOCK_SIZE);
  	wal->record_count = 0;
  	wal->first_seq = 0;
}

  static int zns_base_wal_stage_commit_locked(
  	struct zns_base_c *c,
  	struct zns_base_wal_pending_commit *commit,
  	bool *page_full)
{
  	struct zns_base_wal_state *wal = &c->metadata.wal;

  	/* 호출자는 c->metadata.lock을 잡은 상태여야 한다. */
  	if (wal->flush_error)
  		return wal->flush_error;

  	/*
  	 * page가 꽉 찼다면 caller가 flush를 예약한 뒤,
  	 * 나중에 다시 stage해야 한다.
  	 */
  	if (wal->record_count >= ZNS_BASE_WAL_RECORDS_PER_PAGE)
  		return -EAGAIN;

  	if (wal->record_count == 0)
  		zns_base_wal_reset_page_locked(wal);

  	/*
  	 * 여기서는 아직 disk WAL record를 만들지 않는다.
  	 * seq, record 내용, first_seq는 flush worker가
  	 * 실제 durable write 직전에 확정한다.
  	 */
  	list_add_tail(&commit->node, &wal->pending_commits);
  	wal->record_count++;

  	*page_full =
  		wal->record_count == ZNS_BASE_WAL_RECORDS_PER_PAGE;

  	return 0;
}

static int zns_base_wal_stage_foreground(
  	struct zns_base_c *c,
  	struct zns_base_io *io,
  	size_t logical_block,
  	sector_t new_physical_sector,
  	struct zns_base_zone *new_zone,
  	unsigned int new_slot,
  	bool *page_full)
{
  	struct zns_base_wal_pending_commit *commit;
  	bool full = false;
  	int ret;

  	if (page_full)
  		*page_full = false;

  	commit = kzalloc(sizeof(*commit), GFP_KERNEL);
  	if (!commit)
  		return -ENOMEM;

  	INIT_LIST_HEAD(&commit->node);

  	commit->type = ZNS_BASE_WAL_COMMIT_FOREGROUND;
  	commit->logical_block = logical_block;
  	commit->new_physical_sector = new_physical_sector;

  	/*
  	 * seq는 아직 부여하지 않는다.
  	 * WAL flush worker가 실제 기록 순서에 따라 부여한다.
  	 */
  	commit->seq = 0;

  	commit->new_zone = new_zone;
  	commit->new_slot = new_slot;

  	commit->had_old_mapping = false;
	commit->old_zone = NULL;
	commit->old_slot = 0;

  	commit->io = io;

  	/*
  	 * pending list에 넣기 전에 io count를 먼저 올린다.
  	 * 이후 WAL worker가 commit을 완료할 때 count를 내린다.
  	 */
  	zns_base_io_add_pending_commit(c, io);

	for (;;) {
		mutex_lock(&c->mapping_wal_lock);
		mutex_lock(&c->metadata.lock);

		ret = zns_base_wal_stage_commit_locked(c, commit, &full);

		mutex_unlock(&c->metadata.lock);
		mutex_unlock(&c->mapping_wal_lock);

		if (ret != -EAGAIN)
			break;

		ret = zns_base_wal_flush_sync(c);
		if (ret)
			break;
	}

  	if (ret) {
  		/*
  		 * stage 실패 시 add_pending으로 올린 count를 되돌리고,
  		 * io에 오류를 기록한다.
  		 */
  		zns_base_io_finish_commit(c, io, ret);
  		kfree(commit);
  		return ret;
  	}

  	if (page_full)
  		*page_full = full;
	
   	/*
  	 * 성공 시 commit의 소유권은 wal.pending_commits list로 넘어간다.
  	 * 여기서 kfree(commit)를 하면 안 된다.
  	 */
	return 0;
}

static int zns_base_wal_stage_gc(
	struct zns_base_c *c,
	size_t logical_block,
	sector_t new_physical_sector,
	struct zns_base_zone *new_zone,
	unsigned int new_slot,
	struct zns_base_zone *old_zone,
	unsigned int old_slot,
	const struct mapping_entry *expected_entry,
	bool *page_full)
{
	struct zns_base_wal_pending_commit *commit;
	bool full = false;
	int ret;

	if (page_full)
		*page_full = false;

	commit = kzalloc(sizeof(*commit), GFP_KERNEL);
	if (!commit)
		return -ENOMEM;

	INIT_LIST_HEAD(&commit->node);
	commit->type = ZNS_BASE_WAL_COMMIT_GC;
	commit->logical_block = logical_block;
	commit->new_physical_sector = new_physical_sector;
	commit->new_zone = new_zone;
	commit->new_slot = new_slot;
	commit->old_zone = old_zone;
	commit->old_slot = old_slot;
	commit->expected_physical_sector = expected_entry->physical_sector;
	commit->expected_seq = expected_entry->seq;
	commit->io = NULL;

	for (;;) {
		mutex_lock(&c->mapping_wal_lock);
		mutex_lock(&c->metadata.lock);

		ret = zns_base_wal_stage_commit_locked(c, commit, &full);

		mutex_unlock(&c->metadata.lock);
		mutex_unlock(&c->mapping_wal_lock);

		if (ret != -EAGAIN)
			break;

		ret = zns_base_wal_flush_sync(c);
		if (ret)
			break;
	}

	if (ret) {
		kfree(commit);
		return ret;
	}

	if (page_full)
		*page_full = full;

	return 0;
}

static int zns_base_reserve_pending_slot_locked(
	struct zns_base_zone *zone,
	unsigned int slot,
	size_t logical_block)
{
	if (zone->slots[slot].valid || zone->slots[slot].pending)
		return -EIO;

	zone->slots[slot].logical_block = logical_block;
	zone->slots[slot].pending = true;
	zone->pending_blocks++;
	return 0;
}

static void zns_base_release_pending_slot(
	struct zns_base_c *c,
	struct zns_base_zone *zone,
	unsigned int slot,
	size_t logical_block)
{
	spin_lock(&c->lock);
	if (zone->slots[slot].pending &&
	    zone->slots[slot].logical_block == logical_block) {
		zone->slots[slot].pending = false;
		zone->pending_blocks--;
	}
	spin_unlock(&c->lock);
}

static int zns_base_wal_publish_foreground_locked(
  	struct zns_base_c *c,
  	struct zns_base_wal_pending_commit *commit)
{
  	struct mapping_entry old_entry;
  	struct zns_base_zone *old_zone;
  	unsigned int old_slot;
  	bool had_old_mapping;
  	int ret;

	/*
	 * 호출자는 c->mapping_wal_lock을 잡은 상태여야 한다.
	 * 이 함수 안에서는 sleep하지 않는다.
	 */
	ret = mapping_wait_for_write_slot(c, commit->logical_block);
	if (ret)
		return ret;

	spin_lock(&c->lock);

	if (!commit->new_zone->slots[commit->new_slot].pending ||
	    commit->new_zone->slots[commit->new_slot].logical_block !=
	    commit->logical_block) {
  		ret = -EIO;
  		goto out_unlock;
  	}

  	/*
  	 * WAL flush 시점의 최신 old mapping을 조회한다.
  	 * stage 시점의 old PBA snapshot은 사용하지 않는다.
  	 */
  	ret = mapping_lookup(c, commit->logical_block, &old_entry);

  	if (ret == -ENOENT) {
  		had_old_mapping = false;
  		ret = 0;
  	} else if (ret) {
  		goto out_unlock;
  	} else {
  		had_old_mapping = true;

  		ret = zns_base_get_zone_slot(c,
  					     old_entry.physical_sector,
  					     &old_zone,
  					     &old_slot);
  		if (ret)
  			goto out_unlock;

  		if (!old_zone->slots[old_slot].valid ||
  		    old_zone->slots[old_slot].logical_block !=
  		    commit->logical_block) {
  			ret = -EIO;
  			goto out_unlock;
  		}
  	}

  	ret = mapping_update(c, commit->logical_block,
  			     commit->new_physical_sector,
  			     commit->seq);
  	if (ret)
  		goto out_unlock;

	commit->new_zone->slots[commit->new_slot].logical_block =
		commit->logical_block;
	commit->new_zone->slots[commit->new_slot].pending = false;
	commit->new_zone->pending_blocks--;
	commit->new_zone->slots[commit->new_slot].valid = true;
  	commit->new_zone->valid_blocks++;

  	if (had_old_mapping) {
  		old_zone->slots[old_slot].valid = false;
  		old_zone->valid_blocks--;
  	}

  out_unlock:
  	spin_unlock(&c->lock);
  	return ret;
}

static int zns_base_metadata_allocate_block(
  	struct zns_base_c *c,
  	struct zns_base_metadata_stream *stream,
  	sector_t *physical_sector)
{
  	struct zns_base_zone *zone;
  	sector_t zone_end;

  	zone = &c->zone_state.zones[stream->active_zone_idx];

  	if (zone->role != stream->role)
  		return -EINVAL;

  	zone_end = zone->start_sector + zone->capacity_sectors;

  	if (zone->write_pointer + SECTORS_PER_BLOCK > zone_end)
  		return -ENOSPC;

  	*physical_sector = zone->write_pointer;
  	return 0;
}

static int zns_base_metadata_commit_block(
  	struct zns_base_c *c,
  	struct zns_base_metadata_stream *stream,
  	sector_t physical_sector)
{
  	struct zns_base_zone *zone;

  	zone = &c->zone_state.zones[stream->active_zone_idx];

  	if (zone->role != stream->role)
  		return -EINVAL;

  	if (zone->write_pointer != physical_sector)
  		return -EIO;

  	zone->write_pointer += SECTORS_PER_BLOCK;
  	return 0;
}

static int zns_base_metadata_write_block_locked(
  	struct zns_base_c *c,
  	struct zns_base_metadata_stream *stream,
  	const void *buffer,
  	sector_t *written_sector)
{
  	struct page *page;
  	void *addr;
  	sector_t physical_sector;
  	int ret;

  	page = alloc_page(GFP_KERNEL);
  	if (!page)
  		return -ENOMEM;

  	clear_highpage(page);

  	addr = kmap_local_page(page);
  	memcpy(addr, buffer, ZNS_BASE_BLOCK_SIZE);
  	kunmap_local(addr);

	ret = zns_base_metadata_allocate_block(c, stream,
  					       &physical_sector);
  	if (!ret)
  		ret = zns_base_submit_page(c, page, REQ_OP_WRITE, REQ_FUA,
  					   physical_sector);
  	if (!ret)
  		ret = zns_base_metadata_commit_block(c, stream,
  						     physical_sector);

  	__free_page(page);

  	if (!ret && written_sector)
  		*written_sector = physical_sector;

  	return ret;
}

static int zns_base_metadata_read_block(
  	struct zns_base_c *c,
  	sector_t physical_sector,
  	void *buffer)
{
  	struct page *page;
  	void *addr;
  	int ret;

  	page = alloc_page(GFP_KERNEL);
  	if (!page)
  		return -ENOMEM;

  	ret = zns_base_submit_page(c, page, REQ_OP_READ, 0,
  				   physical_sector);
  	if (!ret) {
  		addr = kmap_local_page(page);
  		memcpy(buffer, addr, ZNS_BASE_BLOCK_SIZE);
  		kunmap_local(addr);
  	}

	__free_page(page);
	return ret;
}

static void zns_base_snapshot_consider(struct mapping_entry *snapshot,
					       const struct mapping_entry *entry)
{
	if (!entry->seq || entry->logical_block == SIZE_MAX)
		return;

	if (snapshot[entry->logical_block].seq < entry->seq)
		snapshot[entry->logical_block] = *entry;
}

/* Caller holds mapping_wal_lock. The spinlock is held only while copying RAM metadata. */
static struct mapping_entry *zns_base_build_snapshot(struct zns_base_c *c,
						      size_t *entry_count,
						      u64 *max_seq)
{
	struct mapping_entry *snapshot;
	struct mapping_memtable *memtable;
	struct mapping_memtable_entry *memtable_entry;
	struct mapping_run *run;
	struct rb_node *node;
	size_t i, j, out = 0;

	snapshot = kvcalloc(c->nr_logical_blocks, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return NULL;

	spin_lock(&c->lock);
	for (node = rb_first(&c->mapping.active_memtable->root); node;
	     node = rb_next(node)) {
		memtable_entry = rb_entry(node,
			struct mapping_memtable_entry, node);
		zns_base_snapshot_consider(snapshot, &memtable_entry->entry);
	}

	list_for_each_entry(memtable, &c->mapping.frozen_memtables, node) {
		for (node = rb_first(&memtable->root); node;
		     node = rb_next(node)) {
			memtable_entry = rb_entry(node,
				struct mapping_memtable_entry, node);
			zns_base_snapshot_consider(snapshot, &memtable_entry->entry);
		}
	}

	for (i = 0; i < c->mapping.run_count; i++) {
		run = &c->mapping.runs[i];
		for (j = 0; j < run->entry_count; j++)
			zns_base_snapshot_consider(snapshot, &run->entries[j]);
	}
	spin_unlock(&c->lock);

	*max_seq = 0;
	for (i = 0; i < c->nr_logical_blocks; i++) {
		if (!snapshot[i].seq)
			continue;
		snapshot[out++] = snapshot[i];
		*max_seq = max(*max_seq, snapshot[out - 1].seq);
	}

	*entry_count = out;
	return snapshot;
}

static int zns_base_reset_metadata_zone_locked(struct zns_base_c *c,
							unsigned int zone_idx)
{
	struct zns_base_zone *zone = &c->zone_state.zones[zone_idx];
	int ret;

	ret = blkdev_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET,
				      zone->start_sector, zone->capacity_sectors,
				      GFP_KERNEL);
	if (ret)
		return ret;

	zone->write_pointer = zone->start_sector;
	zone->state = ZNS_BASE_ZONE_FREE;
	return 0;
}

static int zns_base_choose_checkpoint_sstable_zone_locked(
		struct zns_base_c *c, unsigned int *zone_idx)
{
	unsigned int i;

	for (i = 0; i < c->metadata.sstable.zone_count; i++) {
		unsigned int idx = c->metadata.sstable.first_zone_idx + i;
		struct zns_base_zone *zone = &c->zone_state.zones[idx];

		if (idx == c->metadata.checkpoint_sstable_zone_idx)
			continue;
		if (zone->role != ZNS_BASE_ZONE_SSTABLE)
			return -EIO;

		if (zone->write_pointer != zone->start_sector) {
			int ret = zns_base_reset_metadata_zone_locked(c, idx);
			if (ret)
				return ret;
		}

		zone->state = ZNS_BASE_ZONE_ACTIVE;
		*zone_idx = idx;
		return 0;
	}

	return -ENOSPC;
}

static int zns_base_write_sstable_locked(struct zns_base_c *c,
					 const struct mapping_entry *entries,
					 size_t entry_count, u64 checkpoint_seq,
					 unsigned int *zone_idx_out,
					 sector_t *start_sector_out,
					 u64 *length_bytes_out,
					 u32 *entries_crc_out)
{
	struct zns_base_sstable_header_disk *header;
	struct zns_base_sstable_entry_disk entry;
	u8 *buffer;
	u32 entries_crc = ~0;
	sector_t start_sector;
	size_t i, page_entry, blocks;
	unsigned int zone_idx;
	int ret;

	ret = zns_base_choose_checkpoint_sstable_zone_locked(c, &zone_idx);
	if (ret)
		return ret;

	c->metadata.sstable.active_zone_idx = zone_idx;
	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	for (i = 0; i < entry_count; i++) {
		entry.logical_block = cpu_to_le64(entries[i].logical_block);
		entry.physical_sector = cpu_to_le64(entries[i].physical_sector);
		entries_crc = crc32c(entries_crc, &entry, sizeof(entry));
	}

	header = (struct zns_base_sstable_header_disk *)buffer;
	header->magic = cpu_to_le32(ZNS_BASE_SSTABLE_MAGIC);
	header->version = cpu_to_le16(ZNS_BASE_FORMAT_VERSION);
	header->header_bytes = cpu_to_le16(sizeof(*header));
	header->generation = cpu_to_le64(c->metadata.checkpoint_generation + 1);
	header->entry_count = cpu_to_le64(entry_count);
	header->min_logical_block = cpu_to_le64(entry_count ? entries[0].logical_block : 0);
	header->max_logical_block = cpu_to_le64(entry_count ? entries[entry_count - 1].logical_block : 0);
	header->max_seq = cpu_to_le64(checkpoint_seq);
	header->entries_bytes = cpu_to_le64(entry_count * sizeof(entry));
	header->entries_crc32c = cpu_to_le32(entries_crc);
	header->header_crc32c = 0;
	header->header_crc32c = cpu_to_le32(crc32c(~0, header, sizeof(*header)));

	ret = zns_base_metadata_write_block_locked(c, &c->metadata.sstable,
						   buffer, &start_sector);
	if (ret)
		goto out;

	for (i = 0; i < entry_count; ) {
		memset(buffer, 0, ZNS_BASE_BLOCK_SIZE);
		for (page_entry = 0;
		     page_entry < ZNS_BASE_BLOCK_SIZE / sizeof(entry) && i < entry_count;
		     page_entry++, i++) {
			struct zns_base_sstable_entry_disk *disk_entry =
				(struct zns_base_sstable_entry_disk *)buffer + page_entry;
			disk_entry->logical_block = cpu_to_le64(entries[i].logical_block);
			disk_entry->physical_sector = cpu_to_le64(entries[i].physical_sector);
		}
		ret = zns_base_metadata_write_block_locked(c, &c->metadata.sstable,
							   buffer, NULL);
		if (ret)
			goto out;
	}

	blocks = 1 + DIV_ROUND_UP(entry_count,
					 ZNS_BASE_BLOCK_SIZE / sizeof(entry));
	*zone_idx_out = zone_idx;
	*start_sector_out = start_sector;
	*length_bytes_out = blocks * ZNS_BASE_BLOCK_SIZE;
	*entries_crc_out = entries_crc;
	DMINFO("checkpoint SSTable: zone=%u start=%llu entries=%zu crc=%08x",
	       zone_idx, (unsigned long long)start_sector, entry_count,
	       entries_crc);
out:
	kvfree(buffer);
	return ret;
}

static int zns_base_write_manifest_locked(struct zns_base_c *c,
						  unsigned int sstable_zone_idx,
						  sector_t sstable_start_sector,
						  u64 sstable_length_bytes,
						  const struct mapping_entry *entries,
						  size_t entry_count, u64 checkpoint_seq,
						  u32 entries_crc)
{
	struct zns_base_manifest_header_disk *header;
	struct zns_base_sstable_descriptor_disk *descriptor;
	u8 *buffer;
	u32 descriptor_crc;
	int ret;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	header = (struct zns_base_manifest_header_disk *)buffer;
	descriptor = (struct zns_base_sstable_descriptor_disk *)(buffer + sizeof(*header));

	descriptor->zone_idx = cpu_to_le32(sstable_zone_idx);
	descriptor->start_sector = cpu_to_le64(sstable_start_sector);
	descriptor->length_bytes = cpu_to_le64(sstable_length_bytes);
	descriptor->min_logical_block = cpu_to_le64(entry_count ? entries[0].logical_block : 0);
	descriptor->max_logical_block = cpu_to_le64(entry_count ? entries[entry_count - 1].logical_block : 0);
	descriptor->generation = cpu_to_le64(c->metadata.checkpoint_generation + 1);
	descriptor->payload_crc32c = cpu_to_le32(entries_crc);
	descriptor_crc = crc32c(~0, descriptor, sizeof(*descriptor));

	header->magic = cpu_to_le32(ZNS_BASE_MANIFEST_MAGIC);
	header->version = cpu_to_le16(ZNS_BASE_FORMAT_VERSION);
	header->header_bytes = cpu_to_le16(sizeof(*header));
	header->generation = cpu_to_le64(c->metadata.checkpoint_generation + 1);
	header->checkpoint_last_seq = cpu_to_le64(checkpoint_seq);
	header->descriptor_bytes = cpu_to_le64(sizeof(*descriptor));
	header->sstable_count = cpu_to_le32(1);
	header->descriptors_crc32c = cpu_to_le32(descriptor_crc);
	header->header_crc32c = 0;
	header->header_crc32c = cpu_to_le32(crc32c(~0, header, sizeof(*header)));

	ret = zns_base_metadata_write_block_locked(c, &c->metadata.manifest,
						   buffer, NULL);
	kvfree(buffer);
	return ret;
}

static int zns_base_manifest_rotate_and_write_locked(
	struct zns_base_c *c, unsigned int sstable_zone_idx,
	sector_t sstable_start_sector, u64 sstable_length_bytes,
	const struct mapping_entry *entries, size_t entry_count,
	u64 checkpoint_seq, u32 entries_crc, unsigned int *written_zone_idx)
{
	unsigned int old_idx = c->metadata.checkpoint_manifest_zone_idx;
	unsigned int target_idx;
	int ret;

	if (old_idx == ZNS_BASE_NO_ZONE)
		target_idx = c->metadata.manifest.first_zone_idx;
	else if (old_idx == c->metadata.manifest.first_zone_idx)
		target_idx = old_idx + 1;
	else
		target_idx = c->metadata.manifest.first_zone_idx;

	if (target_idx >= c->metadata.manifest.first_zone_idx +
	    c->metadata.manifest.zone_count)
		return -EINVAL;

	/* The old manifest remains durable while this target is reset and written. */
	if (c->zone_state.zones[target_idx].write_pointer !=
	    c->zone_state.zones[target_idx].start_sector) {
		ret = zns_base_reset_metadata_zone_locked(c, target_idx);
		if (ret)
			return ret;
	}

	c->zone_state.zones[target_idx].state = ZNS_BASE_ZONE_ACTIVE;
	c->metadata.manifest.active_zone_idx = target_idx;
	ret = zns_base_write_manifest_locked(c, sstable_zone_idx,
					       sstable_start_sector, sstable_length_bytes,
					       entries, entry_count, checkpoint_seq, entries_crc);
	if (ret)
		return ret;

	*written_zone_idx = target_idx;
	return 0;
}

static int zns_base_checkpoint_locked(struct zns_base_c *c)
{
	struct mapping_entry *entries;
	sector_t start_sector;
	u64 checkpoint_seq, length_bytes;
	u32 entries_crc;
	unsigned int zone_idx, old_zone_idx, manifest_idx;
	size_t entry_count;
	int ret;

	entries = zns_base_build_snapshot(c, &entry_count, &checkpoint_seq);
	if (!entries)
		return -ENOMEM;

	ret = zns_base_write_sstable_locked(c, entries, entry_count,
						checkpoint_seq, &zone_idx, &start_sector,
						&length_bytes, &entries_crc);
	if (ret)
		goto out;
	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_SSTABLE_WRITE)) {
		ret = -EIO;
		goto out;
	}

	ret = zns_base_manifest_rotate_and_write_locked(c, zone_idx, start_sector,
								  length_bytes, entries, entry_count,
								  checkpoint_seq, entries_crc, &manifest_idx);
	if (ret)
		goto out;
	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_MANIFEST_WRITE)) {
		ret = -EIO;
		goto out;
	}

	old_zone_idx = c->metadata.checkpoint_sstable_zone_idx;
	c->metadata.checkpoint_sstable_zone_idx = zone_idx;
	c->metadata.checkpoint_manifest_zone_idx = manifest_idx;
	c->metadata.checkpoint_seq = checkpoint_seq;
	c->metadata.checkpoint_generation++;

	if (old_zone_idx != ZNS_BASE_NO_ZONE && old_zone_idx != zone_idx)
		zns_base_reset_metadata_zone_locked(c, old_zone_idx);
	/*
	 * Keep the previous manifest as a fallback. Its zone is reset only when
	 * it becomes the target of the next A/B publish.
	 */

	/* The manifest now covers every published WAL record. */
	ret = zns_base_reset_metadata_zone_locked(c, ZNS_BASE_MANIFEST_ZONES);
	if (!ret)
		ret = zns_base_reset_metadata_zone_locked(c, ZNS_BASE_MANIFEST_ZONES + 1);
	if (!ret) {
		c->metadata.wal.stream.active_zone_idx = ZNS_BASE_MANIFEST_ZONES;
		c->metadata.wal.stream.generation++;
		c->metadata.wal.header_written = false;
	}
out:
	kvfree(entries);
	return ret;
}

static bool zns_base_wal_header_valid(struct zns_base_wal_zone_header_disk *header)
{
	u32 stored_crc = le32_to_cpu(header->header_crc32c);
	u32 actual_crc;

	if (le32_to_cpu(header->magic) != ZNS_BASE_WAL_MAGIC ||
	    le16_to_cpu(header->version) != ZNS_BASE_FORMAT_VERSION ||
	    le16_to_cpu(header->header_bytes) != sizeof(*header) ||
	    le32_to_cpu(header->record_bytes) != ZNS_BASE_WAL_RECORD_SIZE)
		return false;

	header->header_crc32c = 0;
	actual_crc = crc32c(~0, header, sizeof(*header));
	header->header_crc32c = cpu_to_le32(stored_crc);
	return actual_crc == stored_crc;
}

static bool zns_base_sstable_header_valid(struct zns_base_sstable_header_disk *header)
{
	u32 stored_crc = le32_to_cpu(header->header_crc32c);
	u32 actual_crc;

	if (le32_to_cpu(header->magic) != ZNS_BASE_SSTABLE_MAGIC ||
	    le16_to_cpu(header->version) != ZNS_BASE_FORMAT_VERSION ||
	    le16_to_cpu(header->header_bytes) != sizeof(*header) ||
	    le64_to_cpu(header->entries_bytes) !=
		le64_to_cpu(header->entry_count) * ZNS_BASE_SSTABLE_ENTRY_SIZE)
		return false;

	header->header_crc32c = 0;
	actual_crc = crc32c(~0, header, sizeof(*header));
	header->header_crc32c = cpu_to_le32(stored_crc);
	return actual_crc == stored_crc;
}

static bool zns_base_manifest_page_valid(
	struct zns_base_manifest_header_disk *header,
	struct zns_base_sstable_descriptor_disk *descriptor)
{
	u32 stored_crc = le32_to_cpu(header->header_crc32c);
	u32 actual_crc;

	if (le32_to_cpu(header->magic) != ZNS_BASE_MANIFEST_MAGIC ||
	    le16_to_cpu(header->version) != ZNS_BASE_FORMAT_VERSION ||
	    le16_to_cpu(header->header_bytes) != sizeof(*header) ||
	    le32_to_cpu(header->sstable_count) != 1 ||
	    le64_to_cpu(header->descriptor_bytes) != sizeof(*descriptor) ||
	    le32_to_cpu(header->descriptors_crc32c) !=
		crc32c(~0, descriptor, sizeof(*descriptor)))
		return false;

	header->header_crc32c = 0;
	actual_crc = crc32c(~0, header, sizeof(*header));
	header->header_crc32c = cpu_to_le32(stored_crc);
	return actual_crc == stored_crc;
}

static int zns_base_manifest_recover(struct zns_base_c *c)
{
	struct zns_base_zone *manifest_zone;
	struct zns_base_manifest_header_disk *manifest;
	struct zns_base_sstable_descriptor_disk *descriptor;
	struct zns_base_sstable_descriptor_disk latest_descriptor;
	struct zns_base_sstable_header_disk *sstable_header;
	struct zns_base_sstable_entry_disk *entry;
	u8 *buffer;
	sector_t sector;
	u64 latest_generation = 0;
	u64 entry_count, i = 0;
	unsigned int manifest_idx;
	unsigned int latest_manifest_idx = ZNS_BASE_NO_ZONE;
	u32 expected_entries_crc;
	u32 entries_crc = ~0;
	int ret = 0;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	for (manifest_idx = c->metadata.manifest.first_zone_idx;
	     manifest_idx < c->metadata.manifest.first_zone_idx +
		c->metadata.manifest.zone_count;
	     manifest_idx++) {
		manifest_zone = &c->zone_state.zones[manifest_idx];
		for (sector = manifest_zone->start_sector;
		     sector < manifest_zone->write_pointer;
		     sector += SECTORS_PER_BLOCK) {
		ret = zns_base_metadata_read_block(c, sector, buffer);
		if (ret)
			goto out;
		manifest = (struct zns_base_manifest_header_disk *)buffer;
		descriptor = (struct zns_base_sstable_descriptor_disk *)
			(buffer + sizeof(*manifest));
		if (!zns_base_manifest_page_valid(manifest, descriptor))
			continue;
		if (le64_to_cpu(manifest->generation) >= latest_generation) {
			latest_generation = le64_to_cpu(manifest->generation);
			latest_manifest_idx = manifest_idx;
			latest_descriptor = *descriptor;
			c->metadata.checkpoint_seq =
				le64_to_cpu(manifest->checkpoint_last_seq);
		}
		}
	}

	if (!latest_generation)
		goto out;

	if (le32_to_cpu(latest_descriptor.zone_idx) >= c->zone_state.nr_zones ||
	    c->zone_state.zones[le32_to_cpu(latest_descriptor.zone_idx)].role !=
		ZNS_BASE_ZONE_SSTABLE) {
		DMERR("manifest descriptor has invalid SSTable zone %u",
		      le32_to_cpu(latest_descriptor.zone_idx));
		ret = -EINVAL;
		goto out;
	}

	ret = zns_base_metadata_read_block(c,
		le64_to_cpu(latest_descriptor.start_sector), buffer);
	if (ret)
		goto out;
	sstable_header = (struct zns_base_sstable_header_disk *)buffer;
	if (!zns_base_sstable_header_valid(sstable_header)) {
		DMERR("SSTable header validation failed at sector %llu",
		      (unsigned long long)le64_to_cpu(latest_descriptor.start_sector));
		ret = -EIO;
		goto out;
	}

	entry_count = le64_to_cpu(sstable_header->entry_count);
	expected_entries_crc = le32_to_cpu(sstable_header->entries_crc32c);
	DMINFO("recover SSTable: zone=%u start=%llu entries=%llu crc=%08x",
	       le32_to_cpu(latest_descriptor.zone_idx),
	       (unsigned long long)le64_to_cpu(latest_descriptor.start_sector),
	       (unsigned long long)entry_count,
	       expected_entries_crc);
	if (entry_count > c->nr_logical_blocks ||
	    expected_entries_crc !=
		le32_to_cpu(latest_descriptor.payload_crc32c)) {
		DMERR("SSTable descriptor mismatch: entries=%llu max=%zu header_crc=%08x descriptor_crc=%08x",
		      (unsigned long long)entry_count, c->nr_logical_blocks,
		      expected_entries_crc,
		      le32_to_cpu(latest_descriptor.payload_crc32c));
		ret = -EIO;
		goto out;
	}

	for (sector = le64_to_cpu(latest_descriptor.start_sector) + SECTORS_PER_BLOCK;
	     i < entry_count; sector += SECTORS_PER_BLOCK) {
		unsigned int page_entries;

		ret = zns_base_metadata_read_block(c, sector, buffer);
		if (ret)
			goto out;
		page_entries = min_t(u64, entry_count - i,
			ZNS_BASE_BLOCK_SIZE / sizeof(*entry));
		entry = (struct zns_base_sstable_entry_disk *)buffer;
		while (page_entries--) {
			entries_crc = crc32c(entries_crc, entry, sizeof(*entry));
			ret = zns_base_replay_wal_put(c,
				le64_to_cpu(entry->logical_block),
				le64_to_cpu(entry->physical_sector),
				c->metadata.checkpoint_seq);
			if (ret)
				DMERR("SSTable replay failed at entry %llu: %d",
				      (unsigned long long)i, ret);
			if (ret)
				goto out;
			entry++;
			i++;
		}
	}

	if (entries_crc != expected_entries_crc) {
		DMERR("SSTable payload CRC mismatch: actual=%08x stored=%08x",
		      entries_crc, expected_entries_crc);
		ret = -EIO;
		goto out;
	}

	c->metadata.checkpoint_generation = latest_generation;
	c->metadata.checkpoint_sstable_zone_idx =
		le32_to_cpu(latest_descriptor.zone_idx);
	c->metadata.checkpoint_manifest_zone_idx = latest_manifest_idx;
	c->metadata.manifest.active_zone_idx = latest_manifest_idx;
	c->metadata.sstable.active_zone_idx =
		c->metadata.checkpoint_sstable_zone_idx;
	c->mapping.next_seq = c->metadata.checkpoint_seq + 1;
out:
	if (ret)
		DMERR("manifest/SSTable recovery failed: %d", ret);
	kvfree(buffer);
	return ret;
}

static bool zns_base_wal_page_valid(struct zns_base_wal_page_header_disk *header)
{
	u8 *payload = (u8 *)header + sizeof(*header);
	u32 stored_crc;
	u32 actual_crc;
	unsigned int count = le16_to_cpu(header->record_count);

	if (le32_to_cpu(header->magic) != ZNS_BASE_WAL_PAGE_MAGIC ||
	    le16_to_cpu(header->version) != ZNS_BASE_FORMAT_VERSION ||
	    le16_to_cpu(header->header_bytes) != sizeof(*header) ||
	    le16_to_cpu(header->record_bytes) != ZNS_BASE_WAL_RECORD_SIZE ||
	    count == 0 || count > ZNS_BASE_WAL_RECORDS_PER_PAGE)
		return false;

	if (crc32c(~0, payload, count * ZNS_BASE_WAL_RECORD_SIZE) !=
	    le32_to_cpu(header->payload_crc32c))
		return false;

	stored_crc = le32_to_cpu(header->header_crc32c);
	header->header_crc32c = 0;
	actual_crc = crc32c(~0, header, sizeof(*header));
	header->header_crc32c = cpu_to_le32(stored_crc);
	return actual_crc == stored_crc;
}

static int zns_base_replay_wal_put(struct zns_base_c *c,
				   size_t logical_block, sector_t physical_sector, u64 seq)
{
	struct mapping_entry old_entry;
	struct zns_base_zone *zone;
	unsigned int slot;
	int ret;

	if (logical_block >= c->nr_logical_blocks)
		return -EINVAL;

	/* Recovery can rebuild more entries than one MemTable generation holds. */
	ret = mapping_wait_for_write_slot(c, logical_block);
	if (ret)
		return ret;

	spin_lock(&c->lock);
	ret = mapping_lookup(c, logical_block, &old_entry);
	if (!ret) {
		ret = zns_base_get_zone_slot(c, old_entry.physical_sector,
						     &zone, &slot);
		if (!ret && zone->slots[slot].valid) {
			zone->slots[slot].valid = false;
			zone->valid_blocks--;
		}
	} else if (ret == -ENOENT) {
		ret = 0;
	}

	if (!ret)
		ret = zns_base_get_zone_slot(c, physical_sector, &zone, &slot);
	if (!ret && !zone->slots[slot].valid) {
		zone->slots[slot].logical_block = logical_block;
		zone->slots[slot].valid = true;
		zone->valid_blocks++;
	}
	if (!ret)
		ret = mapping_update(c, logical_block, physical_sector, seq);
	spin_unlock(&c->lock);

	return ret;
}

static int zns_base_wal_recover(struct zns_base_c *c)
{
	struct zns_base_wal_zone_header_disk *zone_header;
	struct zns_base_wal_page_header_disk *page_header;
	struct zns_base_wal_record_disk *record;
	u8 *buffer;
	u64 generation[ZNS_BASE_WAL_ZONES] = {};
	u64 max_seq = c->metadata.checkpoint_seq;
	u64 latest_generation = 1;
	unsigned int zone_idx[ZNS_BASE_WAL_ZONES];
	unsigned int i, order, page, record_idx;
	unsigned int latest = ZNS_BASE_NO_ZONE;
	unsigned int active_data_zone = ZNS_BASE_NO_ZONE;
	int ret = 0;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	for (i = 0; i < ZNS_BASE_WAL_ZONES; i++) {
		zone_idx[i] = ZNS_BASE_MANIFEST_ZONES + i;
		if (c->zone_state.zones[zone_idx[i]].write_pointer ==
		    c->zone_state.zones[zone_idx[i]].start_sector)
			continue;

		ret = zns_base_metadata_read_block(c,
			c->zone_state.zones[zone_idx[i]].start_sector, buffer);
		if (ret)
			goto out;
		zone_header = (struct zns_base_wal_zone_header_disk *)buffer;
		if (!zns_base_wal_header_valid(zone_header))
			continue;
		generation[i] = le64_to_cpu(zone_header->generation);
	}

	for (order = 0; order < ZNS_BASE_WAL_ZONES; order++) {
		u64 lowest = U64_MAX;
		unsigned int selected = ZNS_BASE_NO_ZONE;
		sector_t sector;
		sector_t wp;

		for (i = 0; i < ZNS_BASE_WAL_ZONES; i++) {
			if (generation[i] && generation[i] < lowest) {
				lowest = generation[i];
				selected = i;
			}
		}
		if (selected == ZNS_BASE_NO_ZONE)
			break;

		generation[selected] = 0;
		latest = zone_idx[selected];
		latest_generation = lowest;
		sector = c->zone_state.zones[latest].start_sector + SECTORS_PER_BLOCK;
		wp = c->zone_state.zones[latest].write_pointer;

		for (page = 0; sector < wp; page++, sector += SECTORS_PER_BLOCK) {
			ret = zns_base_metadata_read_block(c, sector, buffer);
			if (ret)
				goto out;
			page_header = (struct zns_base_wal_page_header_disk *)buffer;
			if (!zns_base_wal_page_valid(page_header))
				break; /* torn or unused tail */

			for (record_idx = 0;
			     record_idx < le16_to_cpu(page_header->record_count);
			     record_idx++) {
				u32 stored_crc;
				u32 actual_crc;

				record = (struct zns_base_wal_record_disk *)
					(buffer + sizeof(*page_header) +
					 record_idx * ZNS_BASE_WAL_RECORD_SIZE);
				stored_crc = le32_to_cpu(record->crc32c);
				record->crc32c = 0;
				actual_crc = crc32c(~0, record, sizeof(*record));
				record->crc32c = cpu_to_le32(stored_crc);
				if (actual_crc != stored_crc)
					goto next_zone;

				if (le64_to_cpu(record->seq) >
				    c->metadata.checkpoint_seq) {
					ret = zns_base_replay_wal_put(c,
						le64_to_cpu(record->logical_block),
						le64_to_cpu(record->physical_sector),
						le64_to_cpu(record->seq));
					if (ret)
						goto out;
				}
				max_seq = max(max_seq, le64_to_cpu(record->seq));
			}
		}
next_zone:
		;
	}

	if (max_seq)
		c->mapping.next_seq = max_seq + 1;

	/* Rebuild the foreground allocator from device write pointers. */
	for (i = ZNS_BASE_METADATA_ZONES; i < c->zone_state.nr_zones; i++) {
		struct zns_base_zone *zone = &c->zone_state.zones[i];
		sector_t end = zone->start_sector + zone->capacity_sectors;

		if (zone->write_pointer == zone->start_sector) {
			zone->state = ZNS_BASE_ZONE_FREE;
		} else if (zone->write_pointer < end) {
			/* Only the newest partially written zone may be foreground ACTIVE. */
			if (active_data_zone != ZNS_BASE_NO_ZONE)
				c->zone_state.zones[active_data_zone].state =
					ZNS_BASE_ZONE_FULL;
			zone->state = ZNS_BASE_ZONE_ACTIVE;
			active_data_zone = i;
		} else {
			zone->state = ZNS_BASE_ZONE_FULL;
		}
	}

	/* A fresh target has no partial DATA zone, so start at the first one. */
	if (active_data_zone == ZNS_BASE_NO_ZONE) {
		active_data_zone = ZNS_BASE_METADATA_ZONES;
		c->zone_state.zones[active_data_zone].state = ZNS_BASE_ZONE_ACTIVE;
	}
	c->zone_state.active_zone_idx = active_data_zone;

	if (latest != ZNS_BASE_NO_ZONE) {
		for (i = 0; i < ZNS_BASE_WAL_ZONES; i++) {
			struct zns_base_zone *zone =
				&c->zone_state.zones[ZNS_BASE_MANIFEST_ZONES + i];

			if (zone->write_pointer == zone->start_sector)
				zone->state = ZNS_BASE_ZONE_FREE;
			else if (ZNS_BASE_MANIFEST_ZONES + i == latest)
				zone->state = ZNS_BASE_ZONE_ACTIVE;
			else
				zone->state = ZNS_BASE_ZONE_FULL;
		}
		c->metadata.wal.stream.active_zone_idx = latest;
		c->metadata.wal.stream.generation = latest_generation;
		c->metadata.wal.header_written = true;
	}
out:
	kvfree(buffer);
	return ret;
}

static int zns_base_wal_write_header_locked(struct zns_base_c *c, u64 first_seq)
{
  	u8 *buffer;
  	struct zns_base_wal_zone_header_disk *header;
  	int ret;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

  	header = (struct zns_base_wal_zone_header_disk *)buffer;

  	header->magic = cpu_to_le32(ZNS_BASE_WAL_MAGIC);
  	header->version = cpu_to_le16(ZNS_BASE_FORMAT_VERSION);
  	header->header_bytes = cpu_to_le16(sizeof(*header));
  	header->generation = cpu_to_le64(
  		c->metadata.wal.stream.generation);
  	header->first_seq = cpu_to_le64(first_seq);
  	header->record_bytes = cpu_to_le32(
  		ZNS_BASE_WAL_RECORD_SIZE);

  	/* crc field는 0으로 둔 상태에서 계산 */
  	header->header_crc32c = cpu_to_le32(
  		crc32c(~0, buffer, sizeof(*header)));

  	ret = zns_base_metadata_write_block_locked(c,
  		&c->metadata.wal.stream, buffer, NULL);
  	if (!ret)
  		c->metadata.wal.header_written = true;
	
	kvfree(buffer);
  	return ret;
}

static bool zns_base_metadata_has_space(
  	struct zns_base_c *c,
  	struct zns_base_metadata_stream *stream,
  	unsigned int blocks)
{
  	struct zns_base_zone *zone;
  	sector_t zone_end;

  	zone = &c->zone_state.zones[stream->active_zone_idx];
  	zone_end = zone->start_sector + zone->capacity_sectors;

  	return zone->write_pointer +
  		((sector_t)blocks * SECTORS_PER_BLOCK) <= zone_end;
}

static int zns_base_wal_rotate(struct zns_base_c *c)
{
  	struct zns_base_metadata_stream *stream;
  	struct zns_base_zone *old_zone;
  	struct zns_base_zone *new_zone;
  	unsigned int i;
  	unsigned int idx;

  	stream = &c->metadata.wal.stream;
  	old_zone = &c->zone_state.zones[stream->active_zone_idx];

  	if (old_zone->role != ZNS_BASE_ZONE_WAL)
  		return -EIO;

  	old_zone->state = ZNS_BASE_ZONE_FULL;

  	for (i = 0; i < stream->zone_count; i++) {
  		idx = stream->first_zone_idx + i;
  		new_zone = &c->zone_state.zones[idx];

  		if (new_zone->role != ZNS_BASE_ZONE_WAL)
  			return -EIO;

  		if (new_zone->state != ZNS_BASE_ZONE_FREE)
  			continue;

  		if (new_zone->write_pointer != new_zone->start_sector)
  			continue;

  		new_zone->state = ZNS_BASE_ZONE_ACTIVE;
  		stream->active_zone_idx = idx;
  		stream->generation++;
  		c->metadata.wal.header_written = false;
  		return 0;
  	}

  	return -ENOSPC;
}

static void zns_base_wal_finalize_page_locked(struct zns_base_c *c)
{
  	struct zns_base_wal_state *wal = &c->metadata.wal;
  	struct zns_base_wal_page_header_disk *header;
  	u8 *payload;
  	size_t payload_bytes;

  	/* 호출자는 c->metadata.lock을 잡은 상태여야 한다. */
  	header = (struct zns_base_wal_page_header_disk *)
  		wal->page_buffer;

  	payload = wal->page_buffer + sizeof(*header);
  	payload_bytes = wal->record_count *
  		ZNS_BASE_WAL_RECORD_SIZE;

  	memset(header, 0, sizeof(*header));

  	header->magic = cpu_to_le32(ZNS_BASE_WAL_PAGE_MAGIC);
  	header->version = cpu_to_le16(ZNS_BASE_FORMAT_VERSION);
  	header->header_bytes = cpu_to_le16(sizeof(*header));
  	header->generation = cpu_to_le64(wal->stream.generation);
  	header->first_seq = cpu_to_le64(wal->first_seq);
  	header->record_count = cpu_to_le16(wal->record_count);
  	header->record_bytes = cpu_to_le16(
  		ZNS_BASE_WAL_RECORD_SIZE);

  	header->payload_crc32c = cpu_to_le32(
  		crc32c(~0, payload, payload_bytes));

  	/* header_crc32c는 0인 상태에서 header 전체를 계산한다. */
  	header->header_crc32c = 0;
  	header->header_crc32c = cpu_to_le32(
  		crc32c(~0, header, sizeof(*header)));
}

static void zns_base_wal_prepare_page_locked(struct zns_base_c *c)
{
  	struct zns_base_wal_state *wal = &c->metadata.wal;
  	struct zns_base_wal_pending_commit *commit;
  	struct zns_base_wal_record_disk *record;
  	unsigned int index = 0;

  	/* 호출자는 mapping_wal_lock과 metadata.lock을 잡은 상태여야 한다. */
  	if (wal->record_count == 0)
  		return;

  	spin_lock(&c->lock);

  	if (wal->first_seq == 0)
  		wal->first_seq = c->mapping.next_seq;

  	list_for_each_entry(commit, &wal->pending_commits, node) {
  		if (commit->seq == 0)
  			commit->seq = c->mapping.next_seq++;
  	}

  	spin_unlock(&c->lock);

  	list_for_each_entry(commit, &wal->pending_commits, node) {
  		record = zns_base_wal_record_at(wal, index++);

  		memset(record, 0, sizeof(*record));

  		record->logical_block =
  			cpu_to_le64(commit->logical_block);
  		record->physical_sector =
  			cpu_to_le64(commit->new_physical_sector);
  		record->seq = cpu_to_le64(commit->seq);
  		record->op_flags = cpu_to_le32(ZNS_BASE_WAL_OP_PUT);

  		record->crc32c = 0;
  		record->crc32c = cpu_to_le32(
  			crc32c(~0, record, sizeof(*record)));
  	}

  	zns_base_wal_finalize_page_locked(c);
}

static int zns_base_wal_write_page_locked(struct zns_base_c *c)
{
  	struct zns_base_wal_state *wal = &c->metadata.wal;
  	unsigned int needed_blocks;
  	int ret;

	/* 호출자는 c->metadata.lock을 잡은 상태여야 한다. */
	if (wal->record_count == 0)
		return 0;

	/* first_seq와 records를 확정한 뒤 zone header를 기록해야 한다. */
	zns_base_wal_prepare_page_locked(c);
	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_CORRUPT_WAL_PAGE_CRC)) {
		struct zns_base_wal_page_header_disk *header;

		header = (struct zns_base_wal_page_header_disk *)
			wal->page_buffer;
		header->payload_crc32c = cpu_to_le32(
			le32_to_cpu(header->payload_crc32c) ^ 1U);
	}

  	/*
  	 * 현재 WAL zone에 zone header와 WAL page를 함께 기록할
  	 * 공간이 있는지 확인한다.
  	 */
  	needed_blocks = wal->header_written ? 1 : 2;

	if (!zns_base_metadata_has_space(c, &wal->stream,
					 needed_blocks)) {
		ret = zns_base_wal_rotate(c);
		if (ret == -ENOSPC) {
			/*
			 * Both WAL zones are full. The caller holds mapping_wal_lock and
			 * metadata.lock, so checkpointing captures a stable published map.
			 */
			ret = zns_base_checkpoint_locked(c);
			if (!ret)
				ret = zns_base_metadata_has_space(c, &wal->stream,
								       needed_blocks) ? 0 : -ENOSPC;
		}
		if (ret)
			return ret;
	}

	if (!wal->header_written) {
		ret = zns_base_wal_write_header_locked(c,
						       wal->first_seq);
		if (ret)
			return ret;
	}

	/* Data writes in this batch used no FUA; persist them before WAL publish. */
	ret = zns_base_submit_flush(c);
	if (ret)
		return ret;

	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_BEFORE_WAL_WRITE))
		return -EIO;

	return zns_base_metadata_write_block_locked(
  		c, &wal->stream, wal->page_buffer, NULL);
}

static void zns_base_wal_finish_commits(
  	struct zns_base_c *c,
  	struct list_head *done_commits)
{
  	struct zns_base_wal_pending_commit *commit;
  	struct zns_base_wal_pending_commit *next;

	list_for_each_entry_safe(commit, next, done_commits, node) {
		list_del_init(&commit->node);

  		if (commit->io)
  			zns_base_io_finish_commit(c, commit->io,
  						  commit->result);

  		kfree(commit);
  	}
}

static void zns_base_wal_abort_pending(
  	struct zns_base_c *c,
  	int error)
{
  	struct zns_base_wal_state *wal = &c->metadata.wal;
  	struct zns_base_wal_pending_commit *commit;
  	struct zns_base_wal_pending_commit *next;
  	LIST_HEAD(done_commits);

  	mutex_lock(&c->metadata.lock);

	list_for_each_entry_safe(commit, next,
				 &wal->pending_commits, node) {
		commit->result = error;
		zns_base_release_pending_slot(c, commit->new_zone,
					      commit->new_slot,
					      commit->logical_block);
		list_move_tail(&commit->node, &done_commits);
  	}

  	zns_base_wal_reset_page_locked(wal);
  	wal->flush_scheduled = false;

  	mutex_unlock(&c->metadata.lock);

  	zns_base_wal_finish_commits(c, &done_commits);
}

static void zns_base_wal_flush_work(struct work_struct *work)
{
  	struct delayed_work *delayed_work = to_delayed_work(work);
  	struct zns_base_wal_state *wal;
  	struct zns_base_metadata_state *metadata;
  	struct zns_base_c *c;
  	struct zns_base_wal_pending_commit *commit;
	struct zns_base_wal_pending_commit *next;
	LIST_HEAD(done_commits);
	bool published = false;
	int ret = 0;

  	wal = container_of(delayed_work, struct zns_base_wal_state,
  			   flush_work);
  	metadata = container_of(wal, struct zns_base_metadata_state, wal);
  	c = container_of(metadata, struct zns_base_c, metadata);

  	/*
  	 * WAL 기록 순서와 mapping publish 순서를 하나로 직렬화한다.
  	 * mapping_wal_lock -> metadata.lock 순서를 항상 유지한다.
  	 */
  	mutex_lock(&c->mapping_wal_lock);
  	mutex_lock(&c->metadata.lock);

  	if (wal->record_count == 0)
  		goto out_unlock;

	ret = zns_base_wal_write_page_locked(c);
	if (ret) {
		DMERR("WAL page flush failed: %d", ret);
		wal->flush_error = ret;

	list_for_each_entry_safe(commit, next,
				 &wal->pending_commits, node) {
		commit->result = ret;
		zns_base_release_pending_slot(c, commit->new_zone,
					      commit->new_slot,
					      commit->logical_block);
		list_move_tail(&commit->node, &done_commits);
  		}

  		zns_base_wal_reset_page_locked(wal);
  		goto out_unlock;
  	}

  	/*
  	 * WAL page가 FUA로 durable하게 기록된 뒤에만
  	 * RAM mapping과 reverse map을 publish한다.
  	 */
	list_for_each_entry_safe(commit, next,
				 &wal->pending_commits, node) {
		if (commit->type == ZNS_BASE_WAL_COMMIT_FOREGROUND)
			commit->result =
				zns_base_wal_publish_foreground_locked(c,
								       commit);
		else if (commit->type == ZNS_BASE_WAL_COMMIT_GC)
			commit->result =
				zns_base_wal_publish_gc_locked(c, commit);
		else
			commit->result = -EIO;

		if (commit->result && !wal->flush_error) {
			DMERR("WAL mapping publish failed: %d", commit->result);
			wal->flush_error = commit->result;
		}

		if (commit->result)
			zns_base_release_pending_slot(c, commit->new_zone,
						      commit->new_slot,
						      commit->logical_block);
		else
			published = true;

  		list_move_tail(&commit->node, &done_commits);
  	}

  	zns_base_wal_reset_page_locked(wal);

  out_unlock:
  	wal->flush_scheduled = false;

  	mutex_unlock(&c->metadata.lock);
  	mutex_unlock(&c->mapping_wal_lock);

	zns_base_wal_finish_commits(c, &done_commits);
	if (published)
		zns_base_schedule_gc(c);
}

static int zns_base_wal_flush_sync(struct zns_base_c *c)
{
	struct zns_base_wal_state *wal = &c->metadata.wal;
	bool has_records;
	int ret;

	mutex_lock(&c->metadata.lock);
	has_records = wal->record_count != 0;
	ret = wal->flush_error;
	mutex_unlock(&c->metadata.lock);

	if (ret)
		return ret;

	if (has_records) {
		mod_delayed_work(c->wal_wq, &wal->flush_work, 0);
		flush_delayed_work(&wal->flush_work);
	}

	mutex_lock(&c->metadata.lock);
	ret = wal->flush_error;
	mutex_unlock(&c->metadata.lock);
	return ret;
}

static int zns_base_wal_publish_gc_locked(
	struct zns_base_c *c,
	struct zns_base_wal_pending_commit *commit)
{
	struct mapping_entry current_entry;
	int ret;

	/* Caller holds mapping_wal_lock. */
	ret = mapping_wait_for_write_slot(c, commit->logical_block);
	if (ret)
		return ret;

	spin_lock(&c->lock);

	if (!commit->new_zone->slots[commit->new_slot].pending ||
	    commit->new_zone->slots[commit->new_slot].logical_block !=
	    commit->logical_block) {
		ret = -EIO;
		goto out_unlock;
	}

	ret = mapping_lookup(c, commit->logical_block, &current_entry);
	if (ret == -ENOENT ||
	    (!ret && (current_entry.physical_sector !=
		      commit->expected_physical_sector ||
		      current_entry.seq != commit->expected_seq))) {
		/* A newer foreground write won while this block was being copied. */
		commit->new_zone->slots[commit->new_slot].pending = false;
		commit->new_zone->pending_blocks--;

		if (commit->old_zone->slots[commit->old_slot].valid &&
		    commit->old_zone->slots[commit->old_slot].logical_block ==
		    commit->logical_block) {
			commit->old_zone->slots[commit->old_slot].valid = false;
			commit->old_zone->valid_blocks--;
		}

		ret = 0;
		goto out_unlock;
	}

	if (ret)
		goto out_unlock;

	ret = mapping_update_if_match(c, commit->logical_block,
				      commit->expected_physical_sector,
				      commit->expected_seq,
				      commit->new_physical_sector, commit->seq);
	if (ret == -ESTALE) {
		commit->new_zone->slots[commit->new_slot].pending = false;
		commit->new_zone->pending_blocks--;
		ret = 0;
		goto out_unlock;
	}
	if (ret)
		goto out_unlock;

	commit->new_zone->slots[commit->new_slot].pending = false;
	commit->new_zone->pending_blocks--;
	commit->new_zone->slots[commit->new_slot].valid = true;
	commit->new_zone->valid_blocks++;
	c->gc_moved_blocks++;

	if (commit->old_zone->slots[commit->old_slot].valid &&
	    commit->old_zone->slots[commit->old_slot].logical_block ==
	    commit->logical_block) {
		commit->old_zone->slots[commit->old_slot].valid = false;
		commit->old_zone->valid_blocks--;
	}

out_unlock:
	spin_unlock(&c->lock);
	return ret;
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
	mutex_init(&c -> mapping_wal_lock);
	/*
	* 먼저 zone role과 usable capacity를 알아야 한다.
	* 아직 mapping_init()을 하지 않았으므로 여기 실패 경로에는
	* mapping_destroy()가 필요 없다.
	*/
	ret = zns_base_zone_init(c);
	if (ret) {
		ti->error = "failed to initialize zone metadata";
		dm_put_device(ti, c->dev);
		kfree(c);
		return ret;
	}

	if (ti->len > zns_base_usable_logical_sectors(c)) {
		ti->error = "target length exceeds usable data capacity";
		zns_base_zone_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return -EINVAL;
	}

	ret = zns_base_metadata_init(c);
	if (ret) {
		ti->error = "failed to initialize metadata state";
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return ret;
	}

	ret = mapping_init(c, ti->len);
	if (ret) {
		ti->error = "failed to allocate mapping state";
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return ret;
	}

	/* WAL/SSTable replay can wait for MemTable flush work during recovery. */
	init_waitqueue_head(&c->spare_waitq);

	ret = zns_base_manifest_recover(c);
	if (ret) {
		ti->error = "failed to recover manifest checkpoint";
		mapping_destroy(c);
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return ret;
	}

	ret = zns_base_wal_recover(c);
	if (ret) {
		ti->error = "failed to replay WAL";
		mapping_destroy(c);
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
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
	c->gc_last_error = 0;
	c -> stopping = false;

	c->metadata.wal.flush_scheduled = false;

	c->io_wq = alloc_workqueue("zns-base-io",
  			    WQ_MEM_RECLAIM, 1);

	if (!c->io_wq) {
		ti->error = "failed to allocate foreground I/O workqueue";
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
		mapping_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	c->wal_wq = alloc_workqueue("zns-base-wal",
  			    WQ_MEM_RECLAIM, 1);
	if (!c->wal_wq) {
		ti->error = "failed to allocate WAL workqueue";
		destroy_workqueue(c->io_wq);
		c->io_wq = NULL;
		zns_base_metadata_destroy(c);
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
		destroy_workqueue(c->wal_wq);
  		c->wal_wq = NULL;
		zns_base_metadata_destroy(c);
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
		destroy_workqueue(c->wal_wq);
 		c->wal_wq = NULL;
		destroy_workqueue(c->io_wq);
		c->io_wq = NULL;

		zns_base_metadata_destroy(c);
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

	cancel_delayed_work_sync(&c->metadata.wal.flush_work);
  	zns_base_wal_abort_pending(c, -EIO);

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

	destroy_workqueue(c->wal_wq);
 	c->wal_wq = NULL;
	zns_base_metadata_destroy(c);

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

static void zns_base_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flags, char *result,
			    unsigned int maxlen)
{
	struct zns_base_c *c = ti->private;
	struct zns_base_zone *wal_zone;
	unsigned int i;
	unsigned int data_active = 0;
	unsigned int data_free = 0;
	unsigned int data_full = 0;
	unsigned int data_gc_dest = 0;
	unsigned int data_gc_victim = 0;
	unsigned int wal_zone_idx;
	unsigned int wal_records;
	unsigned int manifest_active;
	unsigned int sstable_active;
	unsigned int checkpoint_manifest;
	unsigned int checkpoint_sstable;
	u64 wal_generation;
	u64 checkpoint_seq;
	u64 checkpoint_generation;
	u64 gc_runs;
	u64 gc_reset_count;
	u64 gc_moved_blocks;
	size_t mapping_runs;
	sector_t wal_used_sectors;
	sector_t wal_capacity_sectors;
	int gc_error;
	int gc_last_error;
	int wal_error;
	unsigned int sz = 0;

	(void)status_flags;

	if (type == STATUSTYPE_TABLE) {
		DMEMIT("%s", c->dev->name);
		return;
	}

	mutex_lock(&c->metadata.lock);
	wal_zone_idx = c->metadata.wal.stream.active_zone_idx;
	wal_records = c->metadata.wal.record_count;
	wal_generation = c->metadata.wal.stream.generation;
	wal_error = c->metadata.wal.flush_error;
	checkpoint_seq = c->metadata.checkpoint_seq;
	checkpoint_generation = c->metadata.checkpoint_generation;
	checkpoint_manifest = c->metadata.checkpoint_manifest_zone_idx;
	checkpoint_sstable = c->metadata.checkpoint_sstable_zone_idx;
	manifest_active = c->metadata.manifest.active_zone_idx;
	sstable_active = c->metadata.sstable.active_zone_idx;
	mutex_unlock(&c->metadata.lock);

	spin_lock(&c->lock);
	for (i = 0; i < c->zone_state.nr_zones; i++) {
		struct zns_base_zone *zone = &c->zone_state.zones[i];

		if (zone->role != ZNS_BASE_ZONE_DATA)
			continue;

		switch (zone->state) {
		case ZNS_BASE_ZONE_ACTIVE:
			data_active++;
			break;
		case ZNS_BASE_ZONE_FREE:
			data_free++;
			break;
		case ZNS_BASE_ZONE_FULL:
			data_full++;
			break;
		case ZNS_BASE_ZONE_GC_DEST:
			data_gc_dest++;
			break;
		case ZNS_BASE_ZONE_GC_VICTIM:
			data_gc_victim++;
			break;
		}
	}

	wal_zone = &c->zone_state.zones[wal_zone_idx];
	wal_used_sectors = wal_zone->write_pointer - wal_zone->start_sector;
	wal_capacity_sectors = wal_zone->capacity_sectors;
	gc_runs = c->gc_runs;
	gc_reset_count = c->gc_reset_count;
	gc_moved_blocks = c->gc_moved_blocks;
	mapping_runs = c->mapping.run_count;
	gc_error = c->gc_error;
	gc_last_error = c->gc_last_error;
	spin_unlock(&c->lock);

	DMEMIT("data_active=%u data_free=%u data_full=%u gc_dest=%u gc_victim=%u ",
		data_active, data_free, data_full, data_gc_dest, data_gc_victim);
	DMEMIT("gc_runs=%llu gc_resets=%llu gc_moved_blocks=%llu gc_error=%d gc_last_error=%d ",
		(unsigned long long)gc_runs,
		(unsigned long long)gc_reset_count,
		(unsigned long long)gc_moved_blocks, gc_error, gc_last_error);
	DMEMIT("wal_zone=%u wal_generation=%llu wal_used_blocks=%llu wal_capacity_blocks=%llu wal_staged_records=%u wal_error=%d ",
		wal_zone_idx, (unsigned long long)wal_generation,
		(unsigned long long)(wal_used_sectors / SECTORS_PER_BLOCK),
		(unsigned long long)(wal_capacity_sectors / SECTORS_PER_BLOCK),
		wal_records, wal_error);
	DMEMIT("mapping_runs=%zu checkpoint_seq=%llu checkpoint_generation=%llu manifest_active_zone=%u manifest_checkpoint_zone=%u sstable_active_zone=%u sstable_checkpoint_zone=%u",
		mapping_runs,
		(unsigned long long)checkpoint_seq,
		(unsigned long long)checkpoint_generation, manifest_active,
		checkpoint_manifest, sstable_active, checkpoint_sstable);
}

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 1, 0},
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	.status          = zns_base_status,
};

static int __init zns_base_init(void)
{
	int ret;

	zns_base_check_disk_format();

	ret = dm_register_target(&zns_base_target);

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
