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
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/seqlock.h>
#include <linux/srcu.h>

#define DM_MSG_PREFIX "zns-base"
#define ZNS_BASE_BLOCK_SIZE 4096
#define ZNS_BASE_SECTOR_SIZE 512
#define SECTORS_PER_BLOCK 8
#define ZNS_BASE_SECTORS_PER_MIB (1024 * 1024 / ZNS_BASE_SECTOR_SIZE)
#define MEMTABLE_POOL_SIZE 8
#define IO_POOL_SIZE 128
#define ZNS_BASE_DATA_BATCH_BLOCKS 32 /* 128 KiB per lower DATA command */
#define GC_RESERVE_ZONES 2
#define GC_DEFAULT_LOW_WATERMARK 3
#define GC_DEFAULT_TARGET_FREE_ZONES 4
#define ZNS_BASE_NO_ZONE ((unsigned int)-1)
#define ZNS_BASE_MANIFEST_ZONES 2
#define ZNS_BASE_WAL_ZONES      2
#define ZNS_BASE_SSTABLE_ZONES  2
#define ZNS_BASE_METADATA_ZONES (ZNS_BASE_MANIFEST_ZONES + ZNS_BASE_WAL_ZONES + ZNS_BASE_SSTABLE_ZONES)
#define ZNS_BASE_FORMAT_VERSION 3
#define ZNS_BASE_WAL_MAGIC      0x4c41575aU // ZWAL
#define ZNS_BASE_SSTABLE_MAGIC  0x4254535aU // ZSTB
#define ZNS_BASE_MANIFEST_MAGIC 0x4e414d5aU	// ZMAN
#define ZNS_BASE_WAL_RECORD_SIZE 32
#define ZNS_BASE_SSTABLE_ENTRY_SIZE 24
#define ZNS_BASE_WAL_OP_PUT 1
#define ZNS_BASE_WAL_PAGE_MAGIC 0x4750575aU /* ZWPG */
#define ZNS_BASE_WAL_PAGE_HEADER_SIZE 64
#define ZNS_BASE_WAL_RECORDS_PER_PAGE ((ZNS_BASE_BLOCK_SIZE - ZNS_BASE_WAL_PAGE_HEADER_SIZE) / ZNS_BASE_WAL_RECORD_SIZE)
#define ZNS_BASE_MAX_MANIFEST_SSTABLES \
	((ZNS_BASE_BLOCK_SIZE - sizeof(struct zns_base_manifest_header_disk)) / \
	 sizeof(struct zns_base_sstable_descriptor_disk))

/* Keep the production default at the device-reported capacity.  Large-zone
 * emulators can set this read-only parameter to exercise rollover and GC over
 * a smaller prefix of every physical zone.  Zone reset still covers the full
 * hardware zone. */
static unsigned int data_zone_capacity_mib;
module_param(data_zone_capacity_mib, uint, 0444);
MODULE_PARM_DESC(data_zone_capacity_mib,
	"optional usable MiB per data zone for bounded GC tests (0=device capacity)");

/* Production defaults preserve the original policy.  Read-only parameters let
 * a bounded functional test trigger one deterministic GC round without writing
 * most of a multi-GiB FEMU namespace. */
static unsigned int gc_low_watermark = GC_DEFAULT_LOW_WATERMARK;
module_param(gc_low_watermark, uint, 0444);
MODULE_PARM_DESC(gc_low_watermark,
	"free data-zone count at or below which background GC is scheduled");

static unsigned int gc_target_free_zones = GC_DEFAULT_TARGET_FREE_ZONES;
module_param(gc_target_free_zones, uint, 0444);
MODULE_PARM_DESC(gc_target_free_zones,
	"free data-zone count at which a background GC run stops");

/* Production generations must amortize SSTable and Manifest traffic.  Eight
 * 64K-entry tables also leave enough foreground headroom for compaction. */
static unsigned int memtable_capacity_entries = 65536;
module_param(memtable_capacity_entries, uint, 0444);
MODULE_PARM_DESC(memtable_capacity_entries,
	"mapping entries per MemTable generation");

/* Compact less often in production so foreground writes amortize metadata
 * traffic. Tests may lower this value to exercise compaction quickly. */
static unsigned int sstable_compaction_threshold = 16;
module_param(sstable_compaction_threshold, uint, 0444);
MODULE_PARM_DESC(sstable_compaction_threshold,
	"published SSTable count that triggers compaction");

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

	u64 next_seq;

	size_t spare_count;
	size_t reserved_slots;
	struct work_struct flush_work;
	bool flush_pending;
	int flush_error;
};

struct zns_base_zone_slot {
	size_t logical_block;
	u64 seq;
	bool valid;
	bool pending;
};

struct zns_base_zone {
  	sector_t start_sector;
  	sector_t capacity_sectors;
	sector_t reset_sectors;
  	sector_t write_pointer;
   	unsigned int nr_blocks;
   	unsigned int valid_blocks;
	unsigned int pending_blocks;
	struct zns_base_zone_slot *slots;
  	enum zns_base_zone_state state;
	enum zns_base_zone_role role;

	atomic_t inflight_reads;
	wait_queue_head_t read_waitq;
	/* This zone was fully checked and found to have no reclaimable blocks in
	 * this GC run.  Lazy invalidation makes valid_blocks conservative, so a
	 * per-run skip marker prevents repeatedly selecting the same clean zone. */
	u64 gc_skip_run;
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
	/* Data bios are submitted by the single foreground dispatcher and finish
	 * asynchronously.  The original bio cannot complete until both this count
	 * and pending_commits reach zero. */
	unsigned int pending_data_writes;
	/* Normal writes use writeback semantics: they complete once data is staged
	 * into the in-memory WAL overlay.  FUA/PREFLUSH writes wait for the WAL
	 * page's durable publish. */
	bool requires_durable_commit;
  	bool write_staging_done;
  	int write_error;
  	bool completed;
};

struct zns_base_data_mapping {
	struct zns_base_zone *zone;
	size_t logical_block;
	sector_t physical_sector;
	unsigned int slot;
	bool mapping_slot_reserved;
};

/* One completion-ordered physical DATA extent.  Full aligned upper writes are
 * coalesced to 128 KiB; partial RMW remains a one-block extent. */
struct zns_base_data_write {
	struct work_struct complete_work;
	struct list_head node;
	struct zns_base_c *c;
	struct zns_base_io *io;
	struct bio *lower_bio;
	struct zns_base_data_mapping *mappings;
	unsigned int mapping_count;
	struct page *scratch_page; /* non-NULL for partial-block RMW */
	blk_status_t status;
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
	__le64 seq;
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
	bool mapping_slot_reserved;

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
	/* WAL staging must not wait behind SSTable compaction. */
	struct mutex lock;
  	struct zns_base_metadata_stream stream;
  	bool header_written;

	u8 *page_buffer;
  	unsigned int record_count;
  	u64 first_seq;

  	struct list_head pending_commits;

	struct work_struct flush_work;
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
	struct zns_base_sstable_descriptor_disk
		sstables[ZNS_BASE_MAX_MANIFEST_SSTABLES];
	unsigned int sstable_count;
	bool compaction_running;
	u64 compaction_count;
	u64 compaction_last_ns;
	u64 compaction_max_ns;
	seqcount_t catalog_seq;
	struct srcu_struct catalog_srcu;
	bool catalog_srcu_initialized;
	struct mutex lock;
};

struct zns_base_c {
	struct dm_dev *dev;

	struct zns_base_zone_state_table zone_state;

	struct mapping_state mapping;
	size_t nr_logical_blocks;

	struct list_head pending_bios;
	
	struct workqueue_struct *io_wq;
	struct workqueue_struct *data_wq;
	struct workqueue_struct *gc_wq;
	struct workqueue_struct *wal_wq;
	
	struct work_struct io_work;
	struct work_struct gc_work;
	

	struct zns_base_metadata_state metadata;

	bool io_work_scheduled;
	unsigned int foreground_data_inflight;
	/* Conventional writes to the single active DATA stream are released only
	 * after the previous lower bio completes.  Serializing the dispatcher work
	 * alone is insufficient because submit_bio() returns before device
	 * completion. */
	struct list_head data_write_queue;
	bool data_write_inflight;
	int data_write_error;
	bool gc_scheduled;
	bool gc_running;
	int gc_error;
	int gc_last_error;
	u64 gc_runs;
	u64 gc_reset_count;
	u64 gc_moved_blocks;

	mempool_t *io_pool;
	/* quiescing rejects new upper bios while already-issued data completions
	 * and their WAL publication are still allowed to drain.  stopping is the
	 * later, hard-stop state used only after the WAL is empty. */
	bool quiescing;
	bool stopping;

	wait_queue_head_t spare_waitq;
	wait_queue_head_t gc_waitq;
	wait_queue_head_t data_waitq;

	// 나중에 io_lock이랑 mapping_lock이랑 나누기.
	spinlock_t lock;
	struct mutex mapping_wal_lock;
};



static int mapping_update(struct zns_base_c *c, size_t logical_block, sector_t physical_sector, u64 seq);
static int mapping_lookup(struct zns_base_c *c, size_t logical_block, struct mapping_entry *entry);
static int mapping_lookup_visible(struct zns_base_c *c, size_t logical_block,
				  struct mapping_entry *entry);
static int mapping_reserve_write_slot(struct zns_base_c *c,
				      size_t logical_block);
static void mapping_release_write_slot(struct zns_base_c *c);
static struct mapping_memtable_entry *
mapping_memtable_find(struct mapping_memtable *memtable,
			      size_t logical_block);
static int zns_base_allocate_block(struct zns_base_c *c, sector_t *physical_sector);
static int zns_base_write_full_blocks(struct zns_base_c *c,
		struct zns_base_io *io, size_t first_logical_block,
		unsigned int bio_offset_bytes, unsigned int requested_blocks,
		unsigned int *submitted_blocks);
static int zns_base_get_zone_slot(struct zns_base_c *c,sector_t physical_sector,
  				  struct zns_base_zone **zone_out, unsigned int *slot_out);
static void zns_base_gc_work(struct work_struct *work);
static void zns_base_schedule_gc(struct zns_base_c *c);
static int zns_base_gc_move_block(struct zns_base_c *c, struct zns_base_zone *victim,
  				  unsigned int victim_slot);
static int zns_base_reset_victim(struct zns_base_c *c, struct zns_base_zone *victim);
static unsigned int zns_base_count_free_zones(struct zns_base_c *c);
static int zns_base_select_victim(struct zns_base_c *c, struct zns_base_zone **victim_out);
static int zns_base_gc_validate_victim(struct zns_base_c *c,
		struct zns_base_zone *victim, unsigned int *stale_blocks);
static int zns_base_gc_verify_reset_safe(struct zns_base_c *c,
		struct zns_base_zone *victim);
static void zns_base_release_victim(struct zns_base_c *c, struct zns_base_zone *victim);
static bool zns_base_gc_space_ready(struct zns_base_c *c);
static int zns_base_wait_for_gc_space(struct zns_base_c *c);
static bool zns_base_metadata_has_space(
  	struct zns_base_c *c,
  	struct zns_base_metadata_stream *stream,
  	unsigned int blocks);
static int zns_base_wal_rotate(struct zns_base_c *c);
static int zns_base_checkpoint_locked(struct zns_base_c *c);
static int zns_base_persist_memtable_locked(struct zns_base_c *c,
		const struct mapping_entry *entries, size_t entry_count);
static int zns_base_sstable_lookup(struct zns_base_c *c,
		size_t logical_block, struct mapping_entry *entry);
static int zns_base_manifest_rotate_and_write_locked(struct zns_base_c *c,
	const struct zns_base_sstable_descriptor_disk *descriptors,
	unsigned int descriptor_count, u64 checkpoint_seq,
	unsigned int *written_zone_idx);
static bool zns_base_sstable_header_valid(struct zns_base_sstable_header_disk *header);
static int zns_base_sstable_apply_to_snapshot_locked(struct zns_base_c *c,
	const struct zns_base_sstable_descriptor_disk *descriptor,
	struct mapping_entry *snapshot);
static int zns_base_sstable_invalidate_obsolete_locked(struct zns_base_c *c,
	const struct zns_base_sstable_descriptor_disk *descriptor,
	const struct mapping_entry *latest);
static int zns_base_replay_wal_put(struct zns_base_c *c,
				   size_t logical_block, sector_t physical_sector, u64 seq);
static void zns_base_wal_flush_work(struct work_struct *work);
static void zns_base_wal_abort_pending(struct zns_base_c *c, int error);
static int zns_base_wal_flush_sync(struct zns_base_c *c);
static void zns_base_wal_schedule_flush(struct zns_base_c *c, bool immediate);
static int zns_base_wal_stage_foreground(struct zns_base_c *c,
		struct zns_base_io *io, size_t logical_block,
		sector_t new_physical_sector, struct zns_base_zone *new_zone,
		unsigned int new_slot, bool durable, bool *page_full);
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
static bool zns_base_invalidate_entry_slot_locked(struct zns_base_c *c,
		const struct mapping_entry *entry);

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
	BUILD_BUG_ON(sizeof(struct zns_base_sstable_entry_disk) != 24);
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
	    io->pending_data_writes == 0 &&
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

static void zns_base_io_add_pending_data(struct zns_base_c *c,
					  struct zns_base_io *io)
{
	spin_lock(&c->lock);
	io->pending_data_writes++;
	c->foreground_data_inflight++;
	spin_unlock(&c->lock);
}

/* Called only after data completion work has either staged the corresponding
 * WAL record or turned that write into an error. */
static void zns_base_io_finish_data(struct zns_base_c *c,
				    struct zns_base_io *io, int error)
{
	bool restart_dispatcher = false;

	spin_lock(&c->lock);
	if (error && !io->write_error)
		io->write_error = error;
	if (WARN_ON_ONCE(!io->pending_data_writes ||
			 !c->foreground_data_inflight)) {
		spin_unlock(&c->lock);
		return;
	}
	io->pending_data_writes--;
	c->foreground_data_inflight--;
	if (!c->foreground_data_inflight && !c->io_work_scheduled &&
	    !list_empty(&c->pending_bios)) {
		c->io_work_scheduled = true;
		restart_dispatcher = true;
	}
	spin_unlock(&c->lock);

	wake_up_all(&c->data_waitq);
	if (restart_dispatcher)
		queue_work(c->io_wq, &c->io_work);
	zns_base_io_try_complete(c, io);
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
	io->pending_data_writes = 0;
	io->requires_durable_commit = bio->bi_opf & (REQ_FUA | REQ_PREFLUSH);
	io->write_staging_done = false;
	io->write_error = 0;
	io->completed = false;

	spin_lock(&c -> lock);

	if (c->quiescing || c->stopping) {
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
		    zone->slots[slot].logical_block == chunk->logical_block &&
		    zone->slots[slot].seq == entry.seq) {
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

static void zns_base_submit_data_write(struct zns_base_data_write *write)
{
	struct bio *lower = write->lower_bio;

	WARN_ON_ONCE(!lower);
	submit_bio(lower);
}

/* Complete the current conventional DATA dispatch and select the next one.
 * PBA allocation is ordered by the single foreground worker, while this gate
 * makes device submission completion-ordered as required by a sequential
 * write zone.  If one write fails, later reservations cannot be reused safely
 * and are returned through failed_writes without reaching the device. */
static struct zns_base_data_write *
zns_base_finish_data_dispatch(struct zns_base_c *c,
			      struct list_head *failed_writes)
{
	struct zns_base_data_write *next = NULL;

	spin_lock(&c->lock);
	if (WARN_ON_ONCE(!c->data_write_inflight)) {
		spin_unlock(&c->lock);
		return NULL;
	}

	c->data_write_inflight = false;
	if (c->data_write_error) {
		list_splice_init(&c->data_write_queue, failed_writes);
	} else if (!list_empty(&c->data_write_queue)) {
		next = list_first_entry(&c->data_write_queue,
					struct zns_base_data_write, node);
		list_del_init(&next->node);
		c->data_write_inflight = true;
	}
	spin_unlock(&c->lock);

	return next;
}

static void zns_base_fail_queued_data_writes(struct zns_base_c *c,
					      struct list_head *failed_writes,
					      int error)
{
	struct zns_base_data_write *write;
	struct zns_base_data_write *next;
	unsigned int i;

	list_for_each_entry_safe(write, next, failed_writes, node) {
		list_del_init(&write->node);
		bio_put(write->lower_bio);
		write->lower_bio = NULL;
		for (i = 0; i < write->mapping_count; i++) {
			struct zns_base_data_mapping *mapping = &write->mappings[i];

			zns_base_release_pending_slot(c, mapping->zone,
				mapping->slot, mapping->logical_block);
			if (mapping->mapping_slot_reserved)
				mapping_release_write_slot(c);
		}
		if (write->scratch_page)
			__free_page(write->scratch_page);
		zns_base_io_finish_data(c, write->io, error);
		kfree(write->mappings);
		kfree(write);
	}
}

static void zns_base_data_write_complete_work(struct work_struct *work)
{
	struct zns_base_data_write *write = container_of(work,
		struct zns_base_data_write, complete_work);
	struct zns_base_c *c = write->c;
	struct zns_base_data_write *next;
	LIST_HEAD(failed_writes);
	bool wal_page_full = false;
	bool wal_flush_needed = false;
	bool first_data_error = false;
	unsigned int i;
	int ret = 0;

	if (write->status) {
		ret = blk_status_to_errno(write->status);
		if (!ret)
			ret = -EIO;
	} else if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_DATA_WRITE)) {
		ret = -EIO;
	}

	if (ret) {
		/* write_pointer was reserved before submission.  A failed zoned write
		 * leaves a hole that cannot safely be reused, so fail subsequent data
		 * allocations instead of silently corrupting the zone stream. */
		spin_lock(&c->lock);
		if (!c->data_write_error) {
			c->data_write_error = ret;
			first_data_error = true;
		}
		spin_unlock(&c->lock);
		wake_up_all(&c->spare_waitq);
		if (first_data_error)
			DMERR("DATA write pipeline stopped: zone=%u sector=%llu status=%d",
			      (unsigned int)(write->mappings[0].zone -
					     c->zone_state.zones),
			      (unsigned long long)write->mappings[0].physical_sector,
			      ret);
		for (i = 0; i < write->mapping_count; i++) {
			struct zns_base_data_mapping *mapping = &write->mappings[i];

			zns_base_release_pending_slot(c, mapping->zone,
				mapping->slot, mapping->logical_block);
			if (mapping->mapping_slot_reserved) {
				mapping_release_write_slot(c);
				mapping->mapping_slot_reserved = false;
			}
		}
	} else {
		for (i = 0; i < write->mapping_count; i++) {
			struct zns_base_data_mapping *mapping = &write->mappings[i];

			ret = zns_base_wal_stage_foreground(c, write->io,
				mapping->logical_block, mapping->physical_sector,
				mapping->zone, mapping->slot,
				write->io->requires_durable_commit, &wal_page_full);
			/* The WAL staging call owns this reservation on entry. */
			mapping->mapping_slot_reserved = false;
			if (ret) {
				zns_base_release_pending_slot(c, mapping->zone,
					mapping->slot, mapping->logical_block);
				i++;
				break;
			}
			wal_flush_needed |= wal_page_full ||
				write->io->requires_durable_commit;
		}
		if (wal_flush_needed)
			zns_base_wal_schedule_flush(c, true);
		/* A failed record leaves the remainder of this already-written extent
		 * unreachable; release its reservations without publishing mappings. */
		for (; i < write->mapping_count; i++) {
			struct zns_base_data_mapping *mapping = &write->mappings[i];

			zns_base_release_pending_slot(c, mapping->zone,
				mapping->slot, mapping->logical_block);
			if (mapping->mapping_slot_reserved) {
				mapping_release_write_slot(c);
				mapping->mapping_slot_reserved = false;
			}
		}
	}

	if (write->scratch_page)
		__free_page(write->scratch_page);
	zns_base_io_finish_data(c, write->io, ret);
	kfree(write->mappings);
	kfree(write);

	next = zns_base_finish_data_dispatch(c, &failed_writes);
	if (!list_empty(&failed_writes))
		zns_base_fail_queued_data_writes(c, &failed_writes,
						 c->data_write_error ?: -EIO);
	if (next)
		zns_base_submit_data_write(next);
}

static void zns_base_data_write_endio(struct bio *bio)
{
	struct zns_base_data_write *write = bio->bi_private;

	write->status = bio->bi_status;
	write->lower_bio = NULL;
	bio_put(bio);
	queue_work(write->c->data_wq, &write->complete_work);
}

static void zns_base_queue_data_write(struct zns_base_c *c,
				      struct zns_base_data_write *write,
				      struct bio *lower)
{
	lower->bi_end_io = zns_base_data_write_endio;
	lower->bi_private = write;
	write->lower_bio = lower;
	zns_base_io_add_pending_data(c, write->io);

	spin_lock(&c->lock);
	if (!c->data_write_inflight && list_empty(&c->data_write_queue)) {
		c->data_write_inflight = true;
		spin_unlock(&c->lock);
		zns_base_submit_data_write(write);
	} else {
		list_add_tail(&write->node, &c->data_write_queue);
		spin_unlock(&c->lock);
	}
}

/* The caller already reserved both the data PBA and its pending reverse-map
 * slot.  The lower bio is queued in PBA order and is submitted only when the
 * previous conventional DATA write has completed. */
static int zns_base_submit_data_write_async(struct zns_base_c *c,
		struct zns_base_io *io, const struct zns_base_chunk *chunk,
		sector_t physical_sector, struct zns_base_zone *new_zone,
		unsigned int new_slot, struct page *scratch_page)
{
	struct zns_base_data_write *write;
	struct bio *lower;
	int added;

	write = kzalloc(sizeof(*write), GFP_KERNEL);
	if (!write)
		return -ENOMEM;
	write->mappings = kcalloc(1, sizeof(*write->mappings), GFP_KERNEL);
	if (!write->mappings) {
		kfree(write);
		return -ENOMEM;
	}
	write->c = c;
	write->io = io;
	write->mapping_count = 1;
	write->mappings[0].zone = new_zone;
	write->mappings[0].logical_block = chunk->logical_block;
	write->mappings[0].physical_sector = physical_sector;
	write->mappings[0].slot = new_slot;
	write->mappings[0].mapping_slot_reserved = true;
	write->scratch_page = scratch_page;
	INIT_LIST_HEAD(&write->node);
	INIT_WORK(&write->complete_work, zns_base_data_write_complete_work);

	if (scratch_page) {
		lower = bio_alloc(GFP_KERNEL, 1);
		if (!lower)
			goto out_free_write;
		bio_set_dev(lower, c->dev->bdev);
		lower->bi_opf = REQ_OP_WRITE;
		lower->bi_iter.bi_sector = physical_sector;
		added = bio_add_page(lower, scratch_page, ZNS_BASE_BLOCK_SIZE, 0);
		if (added != ZNS_BASE_BLOCK_SIZE) {
			bio_put(lower);
			goto out_free_write;
		}
	} else {
		lower = bio_clone_fast(io->bio, GFP_KERNEL, &fs_bio_set);
		if (!lower)
			goto out_free_write;
		bio_trim(lower, chunk->bio_offset_bytes / ZNS_BASE_SECTOR_SIZE,
			 chunk->length_bytes / ZNS_BASE_SECTOR_SIZE);
		/* Durability is established once, immediately before the WAL FUA.
		 * Replaying upper flush flags on every split DATA clone is redundant. */
		lower->bi_opf &= ~(REQ_PREFLUSH | REQ_FUA);
		bio_set_dev(lower, c->dev->bdev);
		lower->bi_iter.bi_sector = physical_sector;
	}

	zns_base_queue_data_write(c, write, lower);
	return 0;

out_free_write:
	kfree(write->mappings);
	kfree(write);
	return -ENOMEM;
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
	bool mapping_slot_reserved;
	int ret;

  	full_block = chunk->block_offset_bytes == 0 &&
  		     chunk->length_bytes == ZNS_BASE_BLOCK_SIZE;
	scratch_page = NULL;
	old_zone_pinned = false;
	mapping_slot_reserved = false;

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
				 chunk->logical_block ||
				 old_zone->slots[old_slot].seq != old_entry.seq))
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

	ret = mapping_reserve_write_slot(c, chunk->logical_block);
  	if (ret)
  		goto out_free_page;
	mapping_slot_reserved = true;

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
	if (!ret)
		ret = zns_base_reserve_pending_slot_locked(new_zone, new_slot,
						  chunk->logical_block);
	spin_unlock(&c->lock);

  	if (ret)
  		goto out_free_page;

	/* Do not wait here.  The foreground dispatcher continues issuing later
	 * PBA-ordered writes, while completion work appends this mapping to WAL. */
	ret = zns_base_submit_data_write_async(c, io, chunk,
			new_physical_sector, new_zone, new_slot, scratch_page);
	if (ret) {
		spin_lock(&c->lock);
		if (!c->data_write_error)
			c->data_write_error = ret;
		spin_unlock(&c->lock);
		wake_up_all(&c->spare_waitq);
		zns_base_release_pending_slot(c, new_zone, new_slot,
					      chunk->logical_block);
		goto out_free_page;
	}
	/* Ownership moved to zns_base_data_write for partial RMW. */
	scratch_page = NULL;
	mapping_slot_reserved = false;
	
  out_free_page:
	if (mapping_slot_reserved)
		mapping_release_write_slot(c);
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
	unsigned int max_batch_blocks;
  	int ret;

  	if (bio->bi_iter.bi_size % ZNS_BASE_SECTOR_SIZE) {
  		zns_base_io_finish_staging(c, io, -EOPNOTSUPP);
  		return;
  	}

	current_sector = bio->bi_iter.bi_sector;
  	remaining_bytes = bio->bi_iter.bi_size;
	bio_offset_bytes = 0;
	max_batch_blocks = min_t(unsigned int, ZNS_BASE_DATA_BATCH_BLOCKS,
		queue_max_sectors(bdev_get_queue(c->dev->bdev)) /
		SECTORS_PER_BLOCK);
	if (!max_batch_blocks)
		max_batch_blocks = 1;

	if (current_sector % SECTORS_PER_BLOCK == 0 &&
	    remaining_bytes % ZNS_BASE_BLOCK_SIZE == 0) {
		while (remaining_bytes) {
			unsigned int requested = min_t(unsigned int,
				remaining_bytes / ZNS_BASE_BLOCK_SIZE,
				max_batch_blocks);
			unsigned int submitted = 0;

			ret = zns_base_write_full_blocks(c, io,
				current_sector / SECTORS_PER_BLOCK,
				bio_offset_bytes, requested, &submitted);
			if (ret || !submitted) {
				zns_base_io_finish_staging(c, io, ret ?: -EIO);
				return;
			}
			current_sector += (sector_t)submitted * SECTORS_PER_BLOCK;
			bio_offset_bytes += submitted * ZNS_BASE_BLOCK_SIZE;
			remaining_bytes -= submitted * ZNS_BASE_BLOCK_SIZE;
		}
		zns_base_io_finish_staging(c, io, 0);
		return;
	}

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
	size_t available;

	(void)logical_block;

	spin_lock(&c -> lock);

	available = c->mapping.active_memtable->entry_capacity -
		c->mapping.active_memtable->entry_count +
		c->mapping.spare_count *
		c->mapping.active_memtable->entry_capacity;
	ready = c -> stopping ||
			c -> mapping.flush_error ||
			c->data_write_error ||
			available > c->mapping.reserved_slots;
		
	spin_unlock(&c -> lock);

	return ready;
}

/* Reserve MemTable capacity before issuing a physical DATA write.  Every
 * successful caller must release exactly once after mapping_update() or on an
 * error path.  This turns WAL publication into a non-blocking operation. */
static int mapping_reserve_write_slot(struct zns_base_c *c,
				      size_t logical_block){
	(void)logical_block;

	for(;;){
		size_t available;

		spin_lock(&c -> lock);

		if(c -> stopping){
			spin_unlock(&c -> lock);
			return -EIO;
		}

		if(c -> mapping.flush_error){
			spin_unlock(&c -> lock);
			return c -> mapping.flush_error;
		}
		if (c->data_write_error) {
			int error = c->data_write_error;

			spin_unlock(&c->lock);
			return error;
		}

		available = c->mapping.active_memtable->entry_capacity -
			c->mapping.active_memtable->entry_count +
			c->mapping.spare_count *
			c->mapping.active_memtable->entry_capacity;
		if (available > c->mapping.reserved_slots) {
			c->mapping.reserved_slots++;
				spin_unlock(&c -> lock);
				return 0;
		}

		spin_unlock(&c -> lock);

		wait_event(c -> spare_waitq,
			   mapping_write_ready(c, logical_block));
	}
}

static void mapping_release_write_slot(struct zns_base_c *c)
{
	spin_lock(&c->lock);
	if (WARN_ON_ONCE(!c->mapping.reserved_slots)) {
		spin_unlock(&c->lock);
		return;
	}
	c->mapping.reserved_slots--;
	spin_unlock(&c->lock);
	wake_up_all(&c->spare_waitq);
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
	bool wait_for_data;

	c = container_of(work, struct zns_base_c, io_work);

	for(;;){
		spin_lock(&c -> lock);

		if(list_empty(&c -> pending_bios)){
			c -> io_work_scheduled = false;
			spin_unlock(&c -> lock);
			/* A partial WAL page deliberately remains staged here.  Normal
			 * writes have already completed under writeback semantics; page-full,
			 * explicit FLUSH/FUA, or graceful teardown is the durability boundary. */
			return;
		}

		io = list_first_entry(&c -> pending_bios, struct zns_base_io, node);
		list_del_init(&io -> node);
		/* A read or FLUSH is an ordering boundary.  Let already-issued data
		 * writes reach completion work (and hence WAL staging) before this
		 * single dispatcher handles the boundary request. */
		wait_for_data = bio_op(io->bio) != REQ_OP_WRITE &&
			c->foreground_data_inflight;
		if (wait_for_data) {
			list_add(&io->node, &c->pending_bios);
			c->io_work_scheduled = false;
		}

		spin_unlock(&c -> lock);
		if (wait_for_data)
			return;

		if (!zns_base_process_bio(c, io))
			mempool_free(io, c -> io_pool);
	}
}

static void zns_base_gc_work(struct work_struct *work)
{
  	struct zns_base_c *c;
	struct zns_base_zone *victim;
  	unsigned int slot;
	unsigned int reclaimable_blocks;
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
		    (c->quiescing && !c->io_work_scheduled &&
		     !c->foreground_data_inflight &&
		     list_empty(&c->pending_bios)) ||
		    zns_base_count_free_zones(c) >=
		    gc_target_free_zones) {
  			spin_unlock(&c->lock);
  			break;
  		}

  		spin_unlock(&c->lock);

		ret = zns_base_select_victim(c, &victim);
		if (ret == -ENOENT) {
			/* No useful FULL victim is not corruption.  A proactive GC can
			 * legitimately run while the current ACTIVE zone still has room.
			 * End this round without poisoning every later foreground write;
			 * a writer that actually needs another zone performs its own
			 * one-shot ENOSPC decision in zns_base_wait_for_gc_space(). */
			ret = 0;
			break;
		}

		/* valid_blocks is deliberately conservative after a foreground write
		 * misses in RAM.  Resolve the candidate exactly before spending a GC
		 * destination zone on it. */
		ret = zns_base_gc_validate_victim(c, victim,
						  &reclaimable_blocks);
		if (ret) {
			zns_base_release_victim(c, victim);
			break;
		}
		if (!reclaimable_blocks) {
			spin_lock(&c->lock);
			victim->gc_skip_run = c->gc_runs;
			spin_unlock(&c->lock);
			zns_base_release_victim(c, victim);
			continue;
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

		/* Selection statistics are only a heuristic.  Audit the exact,
		 * versioned reverse map immediately before the destructive zone reset. */
		ret = zns_base_gc_verify_reset_safe(c, victim);
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

	if (ret && ret != -ENOSPC && !c->stopping) {
		c->gc_error = ret;
		c->gc_last_error = ret;
		DMERR("GC stopped with error %d", ret);
	} else if (ret == -ENOSPC && !c->stopping) {
		/* Space pressure is recoverable after later overwrites create a
		 * better victim.  Report it, but do not turn the whole target into
		 * a permanent EIO state. */
		c->gc_last_error = ret;
		DMWARN("GC paused without relocation space (%d)", ret);
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

static void mapping_flush_work(struct work_struct *work) {
	struct mapping_state *mapping;
	struct zns_base_c *c;
	struct mapping_memtable *memtable;
	struct mapping_entry *entries;
	size_t entry_count;
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

		entry_count = memtable->entry_count;
		entries = kvcalloc(entry_count, sizeof(*entries), GFP_KERNEL);
		if (!entries) {
			spin_lock(&c -> lock);
			c -> mapping.flush_error = -ENOMEM;
			c -> mapping.flush_pending = false;
			spin_unlock(&c -> lock);

			wake_up_all(&c -> spare_waitq);
			return;
		}

		/* The frozen rbtree is immutable, unique, and LBA-sorted. */
		mapping_memtable_copy_sorted(memtable, entries);

		mutex_lock(&c->metadata.lock);
		ret = zns_base_persist_memtable_locked(c, entries, entry_count);
		mutex_unlock(&c->metadata.lock);
		kvfree(entries);
		if (ret) {
			DMERR("persistent MemTable flush failed: entries=%zu ret=%d sstables=%u",
			      entry_count, ret, c->metadata.sstable_count);
			spin_lock(&c->lock);
			c->mapping.flush_error = ret;
			c->mapping.flush_pending = false;
			spin_unlock(&c->lock);
			wake_up_all(&c->spare_waitq);
			return;
		}

		spin_lock(&c -> lock);
		list_del_init(&memtable -> node);
		mapping_memtable_reset(memtable);
		list_add_tail(&memtable->node, &c->mapping.spare_memtables);
		c -> mapping.spare_count++;

		spin_unlock(&c -> lock);

		wake_up_all(&c -> spare_waitq);
	}
}

static int mapping_init(struct zns_base_c *c, sector_t target_sectors)
{
	size_t nr_logical_blocks;
	struct mapping_memtable *memtable;
	struct mapping_memtable *next;
	size_t mem_capacity = memtable_capacity_entries;
	size_t i;
	int ret;

	if(target_sectors % SECTORS_PER_BLOCK != 0){
		return -EINVAL;
	}
	if (!mem_capacity)
		return -EINVAL;
	nr_logical_blocks = target_sectors / SECTORS_PER_BLOCK;
	
	INIT_LIST_HEAD(&c -> mapping.spare_memtables);
	INIT_LIST_HEAD(&c -> mapping.frozen_memtables);

	c -> mapping.spare_count = 0;
	c->mapping.reserved_slots = 0;

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
	c->mapping.reserved_slots = 0;

	return ret;
}

static void mapping_destroy(struct zns_base_c *c)
{
	struct mapping_memtable *memtable;
	struct mapping_memtable *next;

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

	c -> nr_logical_blocks = 0;
	c->mapping.spare_count = 0;
	c->mapping.reserved_slots = 0;
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

/* Caller holds c->lock. This is the non-sleeping MemTable portion only. */
static int mapping_lookup_ram_locked(struct zns_base_c *c,
				     size_t logical_block,
				     struct mapping_entry *entry)
{
	struct mapping_memtable *active_memtable;
	struct mapping_memtable *memtable;
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

	return -ENOENT;
}

/* May sleep while binary-searching immutable on-media SSTables. */
static int mapping_lookup(struct zns_base_c *c, size_t logical_block,
			  struct mapping_entry *entry)
{
	int ret;

	spin_lock(&c->lock);
	ret = mapping_lookup_ram_locked(c, logical_block, entry);
	spin_unlock(&c->lock);
	if (ret != -ENOENT)
		return ret;

	ret = zns_base_sstable_lookup(c, logical_block, entry);
	return ret;
}

/* Includes writes that reached the data zone but are waiting for WAL FUA. */
static int mapping_lookup_visible(struct zns_base_c *c, size_t logical_block,
				  struct mapping_entry *entry)
{
	struct zns_base_wal_pending_commit *commit;

	mutex_lock(&c->metadata.wal.lock);
	list_for_each_entry_reverse(commit, &c->metadata.wal.pending_commits,
				    node) {
		if (commit->logical_block != logical_block)
			continue;

		entry->logical_block = logical_block;
		entry->physical_sector = commit->new_physical_sector;
		entry->seq = commit->seq;
		mutex_unlock(&c->metadata.wal.lock);
		return 0;
	}
	mutex_unlock(&c->metadata.wal.lock);

	return mapping_lookup(c, logical_block, entry);
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

/* Mark one reverse-map slot stale only when it still describes exactly the
 * mapping version supplied by the caller.  This exact match is important for
 * lazy invalidation: a newer write may already have reused the same LBA, but a
 * data PBA is never treated as obsolete merely because its LBA matches.  The
 * caller holds c->lock. */
static bool zns_base_invalidate_entry_slot_locked(
	struct zns_base_c *c, const struct mapping_entry *entry)
{
	struct zns_base_zone *zone;
	struct zns_base_zone_slot *slot;
	unsigned int slot_idx;

	if (zns_base_get_zone_slot(c, entry->physical_sector,
				       &zone, &slot_idx))
		return false;

	slot = &zone->slots[slot_idx];
	if (!slot->valid || slot->pending ||
	    slot->logical_block != entry->logical_block ||
	    slot->seq != entry->seq)
		return false;

	slot->valid = false;
	zone->valid_blocks--;
	return true;
}

static int zns_base_report_zone(struct blk_zone *zone,
  				unsigned int idx, void *data)
{
  	struct zns_base_c *c = data;
  	struct zns_base_zone *z;
	sector_t requested_capacity;

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
	z->reset_sectors = zone->len;
	z->role = zns_base_zone_role_from_index(idx);
	if (z->role == ZNS_BASE_ZONE_DATA && data_zone_capacity_mib) {
		requested_capacity =
			(sector_t)data_zone_capacity_mib *
			ZNS_BASE_SECTORS_PER_MIB;
		if (!requested_capacity ||
		    requested_capacity > zone->capacity ||
		    requested_capacity % SECTORS_PER_BLOCK != 0 ||
		    zone->wp > zone->start + requested_capacity)
			return -EINVAL;
		z->capacity_sectors = requested_capacity;
	}
	/* Recovery uses the device-reported write pointer as the source of truth. */
	z->write_pointer = zone->wp;
	z->nr_blocks = z->capacity_sectors / SECTORS_PER_BLOCK;
	z->valid_blocks = 0;
	z->pending_blocks = 0;
	z->gc_skip_run = 0;
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

/* Caller holds c->lock.  Once GC owns a destination, that zone itself is the
 * first unit of relocation reserve.  Keeping the full two additional FREE
 * zones at that point prevents a freshly reset victim from returning to
 * foreground use and can deadlock exactly at the low watermark. */
static unsigned int zns_base_foreground_reserve_locked(struct zns_base_c *c)
{
	unsigned int reserve = GC_RESERVE_ZONES;

	if (c->zone_state.gc_dest_zone_idx != ZNS_BASE_NO_ZONE && reserve)
		reserve--;
	return reserve;
}

static bool zns_base_gc_space_ready(struct zns_base_c *c)
{
  	bool ready;

  	spin_lock(&c->lock);

	ready = c->stopping ||
		c->gc_error ||
		zns_base_count_free_zones(c) >
			zns_base_foreground_reserve_locked(c) ||
		(!c->gc_running && !c->gc_scheduled);

  	spin_unlock(&c->lock);

  	return ready;
}

static int zns_base_wait_for_gc_space(struct zns_base_c *c)
{
	bool attempted_gc = false;

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

		if (zns_base_count_free_zones(c) >
		    zns_base_foreground_reserve_locked(c)) {
			spin_unlock(&c->lock);
			return 0;
		}

		/* A GC round completed but could not create an admissible FREE
		 * zone.  Fail this allocation as NOSPC without setting gc_error;
		 * the target remains usable instead of becoming permanently EIO. */
		if (attempted_gc && !c->gc_running && !c->gc_scheduled) {
			c->gc_last_error = -ENOSPC;
			spin_unlock(&c->lock);
			return -ENOSPC;
		}

  		spin_unlock(&c->lock);

		/* lock 밖에서 GC를 예약해야 한다. */
		zns_base_schedule_gc(c);
		attempted_gc = true;

  		/*
  		 * reset 성공, GC 실패, dtr 종료 중 하나가 발생하면
  		 * 깨어나서 위 조건을 다시 검사한다.
  		 */
  		wait_event(c->gc_waitq, zns_base_gc_space_ready(c));
  	}
}

static bool zns_base_gc_needed(struct zns_base_c *c)
{
	return zns_base_count_free_zones(c) <= gc_low_watermark;
}

static void zns_base_schedule_gc(struct zns_base_c *c)
{
	bool queue_gc = false;
	bool draining_foreground;

	spin_lock(&c->lock);

	draining_foreground = c->io_work_scheduled ||
		c->foreground_data_inflight ||
		!list_empty(&c->pending_bios);

	if (!c->stopping &&
	    (!c->quiescing || draining_foreground) &&
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

		/* Lazy invalidation can leave a physically stale slot counted as valid.
		 * A zone that was checked and found clean is skipped only for this GC
		 * run; other FULL zones, including conservatively all-valid ones, remain
		 * candidates for the exact validation pass. */
		if (zone->gc_skip_run == c->gc_runs)
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

/* Resolve one candidate zone without relocating data.  Foreground writes only
 * invalidate mappings found in RAM, so valid_blocks is an upper bound until
 * this pass compares every reverse-map version with the newest visible map.
 * The victim remains isolated in GC_VICTIM state throughout the scan. */
static int zns_base_gc_validate_victim(struct zns_base_c *c,
	struct zns_base_zone *victim, unsigned int *reclaimable_blocks)
{
	struct mapping_entry current_entry;
	sector_t physical_sector;
	unsigned int slot_idx;
	int ret;

	for (slot_idx = 0; slot_idx < victim->nr_blocks; slot_idx++) {
		size_t logical_block;
		u64 seq;

		spin_lock(&c->lock);
		if (victim->state != ZNS_BASE_ZONE_GC_VICTIM) {
			spin_unlock(&c->lock);
			return -EIO;
		}
		if (!victim->slots[slot_idx].valid) {
			spin_unlock(&c->lock);
			continue;
		}

		logical_block = victim->slots[slot_idx].logical_block;
		seq = victim->slots[slot_idx].seq;
		physical_sector = victim->start_sector +
			((sector_t)slot_idx * SECTORS_PER_BLOCK);
		spin_unlock(&c->lock);

		ret = mapping_lookup(c, logical_block, &current_entry);
		if (ret && ret != -ENOENT)
			return ret;

		spin_lock(&c->lock);
		if (victim->state != ZNS_BASE_ZONE_GC_VICTIM) {
			spin_unlock(&c->lock);
			return -EIO;
		}
		if (victim->slots[slot_idx].valid &&
		    victim->slots[slot_idx].logical_block == logical_block &&
		    victim->slots[slot_idx].seq == seq &&
		    (ret == -ENOENT ||
		     current_entry.physical_sector != physical_sector ||
		     current_entry.seq != seq)) {
			victim->slots[slot_idx].valid = false;
			victim->valid_blocks--;
		}
		spin_unlock(&c->lock);
	}

	spin_lock(&c->lock);
	if (victim->state != ZNS_BASE_ZONE_GC_VICTIM) {
		spin_unlock(&c->lock);
		return -EIO;
	}
	*reclaimable_blocks = victim->nr_blocks - victim->valid_blocks;
	spin_unlock(&c->lock);
	return 0;
}

/* Destructive reset guard.  Every path is required to invalidate a slot only
 * after an exact {LBA, PBA, seq} comparison; uncertain SSTable-only mappings
 * deliberately remain valid.  GC_VICTIM is isolated from allocation, so a
 * mapping cannot newly point into it.  Consequently an exhaustive reverse-map
 * audit with no valid/pending slots is the exact reset condition and needs no
 * second round of per-slot SSTable I/O. */
static int zns_base_gc_verify_reset_safe(struct zns_base_c *c,
	struct zns_base_zone *victim)
{
	unsigned int valid_blocks = 0;
	unsigned int pending_blocks = 0;
	unsigned int slot_idx;

	spin_lock(&c->lock);
	if (victim->state != ZNS_BASE_ZONE_GC_VICTIM) {
		spin_unlock(&c->lock);
		return -EIO;
	}
	for (slot_idx = 0; slot_idx < victim->nr_blocks; slot_idx++) {
		valid_blocks += victim->slots[slot_idx].valid;
		pending_blocks += victim->slots[slot_idx].pending;
	}
	if (valid_blocks != victim->valid_blocks ||
	    pending_blocks != victim->pending_blocks) {
		DMERR("GC reset guard found reverse-map counter mismatch zone=%u valid=%u/%u pending=%u/%u",
		      (unsigned int)(victim - c->zone_state.zones),
		      valid_blocks, victim->valid_blocks,
		      pending_blocks, victim->pending_blocks);
		spin_unlock(&c->lock);
		return -EIO;
	}
	spin_unlock(&c->lock);

	return valid_blocks == 0 && pending_blocks == 0 ? 0 : -EBUSY;
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
	u64 victim_seq;
	unsigned int new_slot;
	bool wal_page_full;
	bool mapping_slot_reserved = false;
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
	victim_seq = victim->slots[victim_slot].seq;
	old_physical_sector = victim->start_sector +
  		((sector_t)victim_slot * SECTORS_PER_BLOCK);

	spin_unlock(&c->lock);
	ret = mapping_lookup(c, logical_block, &expected_entry);
	spin_lock(&c->lock);
	if (victim->state != ZNS_BASE_ZONE_GC_VICTIM ||
	    !victim->slots[victim_slot].valid ||
	    victim->slots[victim_slot].logical_block != logical_block ||
	    victim->slots[victim_slot].seq != victim_seq) {
		spin_unlock(&c->lock);
		return 0;
	}

  	/*
  	 * reverse map slot은 valid지만 mapping이 이미 다른 PBA를
  	 * 가리키면, 이 slot은 stale data다. 복사할 필요가 없다.
  	 */
	if (ret == -ENOENT ||
	    (!ret &&
	     (expected_entry.physical_sector != old_physical_sector ||
	      expected_entry.seq != victim_seq))) {
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

	ret = mapping_reserve_write_slot(c, logical_block);
	if (ret)
		return ret;
	mapping_slot_reserved = true;

	ret = zns_base_allocate_gc_block(c, &new_physical_sector,
					 &new_zone, &new_slot);
	if (ret) {
		mapping_release_write_slot(c);
  		return ret;
	}

  	page = alloc_page(GFP_KERNEL);
	if (!page) {
		mapping_release_write_slot(c);
  		return -ENOMEM;
	}

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

	wal_page_full = false;
	ret = zns_base_wal_stage_gc(c, logical_block, new_physical_sector,
					    new_zone, new_slot, victim, victim_slot,
					    &expected_entry, &wal_page_full);
	/* zns_base_wal_stage_gc() owns the reservation on entry. */
	mapping_slot_reserved = false;
	if (ret) {
		zns_base_release_pending_slot(c, new_zone, new_slot,
					      logical_block);
		goto out_free_page;
	}

	/* GC moves also batch; a full page commits now and victim reset calls
	 * zns_base_wal_flush_sync() for a partial final page. */
	if (wal_page_full)
		zns_base_wal_schedule_flush(c, true);
	ret = 0;
	
  out_free_page:
	if (mapping_slot_reserved)
		mapping_release_write_slot(c);
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
			       victim->reset_sectors,
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
	victim->gc_skip_run = 0;
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

	if (zns_base_count_free_zones(c) <=
	    zns_base_foreground_reserve_locked(c))
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

	if (c->data_write_error)
		return c->data_write_error;

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
		zone_end = zone->start_sector + zone->capacity_sectors;
	}

	/* Reserve now, before the asynchronous lower write is submitted.  The
	 * single foreground dispatcher therefore gives every in-flight bio a
	 * distinct, monotonically increasing PBA. */
	*physical_sector = zone->write_pointer;
	zone->write_pointer += SECTORS_PER_BLOCK;
	if (zone->write_pointer == zone_end)
		zone->state = ZNS_BASE_ZONE_FULL;
	return 0;
}

static int zns_base_write_full_blocks(struct zns_base_c *c,
		struct zns_base_io *io, size_t first_logical_block,
		unsigned int bio_offset_bytes, unsigned int requested_blocks,
		unsigned int *submitted_blocks)
{
	struct zns_base_data_write *write;
	struct zns_base_zone *zone;
	struct bio *lower;
	sector_t zone_end;
	sector_t first_physical = 0;
	unsigned int count;
	unsigned int reserved = 0;
	unsigned int allocated = 0;
	unsigned int i;
	int ret = 0;

	*submitted_blocks = 0;
	if (!requested_blocks)
		return -EINVAL;
	if (first_logical_block >= c->nr_logical_blocks ||
	    requested_blocks > c->nr_logical_blocks - first_logical_block)
		return -EIO;

	/* The foreground worker is the sole DATA allocator.  Select one extent
	 * that cannot cross the current physical zone. */
retry_zone:
	spin_lock(&c->lock);
	zone = &c->zone_state.zones[c->zone_state.active_zone_idx];
	zone_end = zone->start_sector + zone->capacity_sectors;
	if (zone->state != ZNS_BASE_ZONE_ACTIVE ||
	    zone->write_pointer + SECTORS_PER_BLOCK > zone_end) {
		zone->state = ZNS_BASE_ZONE_FULL;
		ret = zns_base_activate_next_zone(c);
		if (!ret) {
			zone = &c->zone_state.zones[c->zone_state.active_zone_idx];
			zone_end = zone->start_sector + zone->capacity_sectors;
		}
	}
	if (!ret && c->data_write_error)
		ret = c->data_write_error;
	if (!ret)
		count = min_t(unsigned int, requested_blocks,
			(zone_end - zone->write_pointer) / SECTORS_PER_BLOCK);
	else
		count = 0;
	spin_unlock(&c->lock);
	if (ret == -EAGAIN) {
		ret = zns_base_wait_for_gc_space(c);
		if (!ret)
			goto retry_zone;
	}
	if (ret)
		return ret;
	if (!count)
		return -ENOSPC;

	write = kzalloc(sizeof(*write), GFP_KERNEL);
	if (!write)
		return -ENOMEM;
	write->mappings = kcalloc(count, sizeof(*write->mappings), GFP_KERNEL);
	if (!write->mappings) {
		kfree(write);
		return -ENOMEM;
	}
	lower = bio_clone_fast(io->bio, GFP_KERNEL, &fs_bio_set);
	if (!lower) {
		kfree(write->mappings);
		kfree(write);
		return -ENOMEM;
	}
	bio_trim(lower, bio_offset_bytes / ZNS_BASE_SECTOR_SIZE,
		 count * SECTORS_PER_BLOCK);
	/* The WAL worker flushes all completed DATA before publishing mappings. */
	lower->bi_opf &= ~(REQ_PREFLUSH | REQ_FUA);
	bio_set_dev(lower, c->dev->bdev);

	write->c = c;
	write->io = io;
	write->mapping_count = count;
	write->scratch_page = NULL;
	INIT_LIST_HEAD(&write->node);
	INIT_WORK(&write->complete_work, zns_base_data_write_complete_work);

	for (i = 0; i < count; i++) {
		ret = mapping_reserve_write_slot(c, first_logical_block + i);
		if (ret)
			goto out_release;
		write->mappings[i].logical_block = first_logical_block + i;
		write->mappings[i].mapping_slot_reserved = true;
		reserved++;
	}

	for (i = 0; i < count; i++) {
		struct zns_base_data_mapping *mapping = &write->mappings[i];
		sector_t physical;

		spin_lock(&c->lock);
		ret = zns_base_allocate_block(c, &physical);
		if (!ret)
			ret = zns_base_get_zone_slot(c, physical,
				&mapping->zone, &mapping->slot);
		if (!ret && (mapping->zone != zone ||
			(mapping->zone->slots[mapping->slot].valid ||
			 mapping->zone->slots[mapping->slot].pending)))
			ret = -EIO;
		if (!ret)
			ret = zns_base_reserve_pending_slot_locked(mapping->zone,
				mapping->slot, mapping->logical_block);
		spin_unlock(&c->lock);
		if (ret)
			goto out_pipeline_error;

		mapping->physical_sector = physical;
		allocated++;
		if (!i)
			first_physical = physical;
		else if (physical != first_physical +
				 (sector_t)i * SECTORS_PER_BLOCK) {
			ret = -EIO;
			goto out_pipeline_error;
		}
	}

	lower->bi_iter.bi_sector = first_physical;
	zns_base_queue_data_write(c, write, lower);
	*submitted_blocks = count;
	return 0;

out_pipeline_error:
	/* At least one physical write pointer may already be reserved.  Continuing
	 * would create an illegal hole, so latch a permanent pipeline error. */
	spin_lock(&c->lock);
	if (!c->data_write_error)
		c->data_write_error = ret ?: -EIO;
	spin_unlock(&c->lock);
	wake_up_all(&c->spare_waitq);
out_release:
	for (i = 0; i < allocated; i++)
		zns_base_release_pending_slot(c, write->mappings[i].zone,
			write->mappings[i].slot,
			write->mappings[i].logical_block);
	for (i = 0; i < reserved; i++)
		if (write->mappings[i].mapping_slot_reserved)
			mapping_release_write_slot(c);
	bio_put(lower);
	kfree(write->mappings);
	kfree(write);
	return ret;
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
	int ret;

  	mutex_init(&c->metadata.lock);
	mutex_init(&c->metadata.wal.lock);
	seqcount_init(&c->metadata.catalog_seq);
	ret = init_srcu_struct(&c->metadata.catalog_srcu);
	if (ret)
		return ret;
	c->metadata.catalog_srcu_initialized = true;

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

	INIT_WORK(&c->metadata.wal.flush_work, zns_base_wal_flush_work);

	c->metadata.wal.flush_scheduled = false;
  	c->metadata.wal.flush_error = 0;
	c->metadata.checkpoint_seq = 0;
	c->metadata.checkpoint_generation = 0;
	c->metadata.checkpoint_sstable_zone_idx = ZNS_BASE_NO_ZONE;
	c->metadata.checkpoint_manifest_zone_idx = ZNS_BASE_NO_ZONE;
	c->metadata.sstable_count = 0;
	memset(c->metadata.sstables, 0, sizeof(c->metadata.sstables));

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
	if (c->metadata.catalog_srcu_initialized) {
		cleanup_srcu_struct(&c->metadata.catalog_srcu);
		c->metadata.catalog_srcu_initialized = false;
	}
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

	/* Caller holds wal.lock. */
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
	bool durable,
	bool *page_full)
{
  	struct zns_base_wal_pending_commit *commit;
  	bool full = false;
  	int ret;

  	if (page_full)
  		*page_full = false;

	commit = kzalloc(sizeof(*commit), GFP_KERNEL);
	if (!commit) {
		mapping_release_write_slot(c);
  		return -ENOMEM;
	}

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
	commit->mapping_slot_reserved = true;

  	commit->had_old_mapping = false;
	commit->old_zone = NULL;
	commit->old_slot = 0;

	commit->io = durable ? io : NULL;

	/* A normal write has writeback completion semantics.  Its mapping remains
	 * visible in wal.pending_commits, but its original bio is not held until
	 * the next page-full or explicit flush durability boundary. */
	if (durable)
		zns_base_io_add_pending_commit(c, io);

	for (;;) {
		mutex_lock(&c->metadata.wal.lock);

		ret = zns_base_wal_stage_commit_locked(c, commit, &full);

		mutex_unlock(&c->metadata.wal.lock);

		if (ret != -EAGAIN)
			break;

		ret = zns_base_wal_flush_sync(c);
		if (ret)
			break;
	}

  	if (ret) {
		if (durable)
			zns_base_io_finish_commit(c, io, ret);
		mapping_release_write_slot(c);
		commit->mapping_slot_reserved = false;
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
	if (!commit) {
		mapping_release_write_slot(c);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&commit->node);
	commit->type = ZNS_BASE_WAL_COMMIT_GC;
	commit->logical_block = logical_block;
	commit->new_physical_sector = new_physical_sector;
	commit->new_zone = new_zone;
	commit->new_slot = new_slot;
	commit->mapping_slot_reserved = true;
	commit->old_zone = old_zone;
	commit->old_slot = old_slot;
	commit->expected_physical_sector = expected_entry->physical_sector;
	commit->expected_seq = expected_entry->seq;
	commit->io = NULL;

	for (;;) {
		mutex_lock(&c->metadata.wal.lock);

		ret = zns_base_wal_stage_commit_locked(c, commit, &full);

		mutex_unlock(&c->metadata.wal.lock);

		if (ret != -EAGAIN)
			break;

		ret = zns_base_wal_flush_sync(c);
		if (ret)
			break;
	}

	if (ret) {
		mapping_release_write_slot(c);
		commit->mapping_slot_reserved = false;
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
	zone->slots[slot].seq = 0;
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
		zone->slots[slot].seq = 0;
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

	spin_lock(&c->lock);
	if (!commit->new_zone->slots[commit->new_slot].pending ||
	    commit->new_zone->slots[commit->new_slot].logical_block !=
	    commit->logical_block) {
  		ret = -EIO;
  		goto out_unlock;
  	}

	/* Foreground publication must never perform persistent SSTable I/O.  An
	 * older mapping that is still in Active/Frozen RAM can be invalidated
	 * eagerly; a mapping that exists only in an SSTable remains conservatively
	 * valid until compaction or GC proves it stale. */
	ret = mapping_lookup_ram_locked(c, commit->logical_block, &old_entry);
	if (ret == -ENOENT) {
		had_old_mapping = false;
		ret = 0;
	} else if (ret) {
		goto out_unlock;
	} else {
		had_old_mapping = true;
	}

	if (had_old_mapping) {
  		ret = zns_base_get_zone_slot(c,
  					     old_entry.physical_sector,
  					     &old_zone,
  					     &old_slot);
  		if (ret)
  			goto out_unlock;

		if (!old_zone->slots[old_slot].valid ||
		    old_zone->slots[old_slot].pending ||
  		    old_zone->slots[old_slot].logical_block !=
		    commit->logical_block ||
		    old_zone->slots[old_slot].seq != old_entry.seq) {
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
	commit->new_zone->slots[commit->new_slot].seq = commit->seq;
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
	if (commit->mapping_slot_reserved) {
		mapping_release_write_slot(c);
		commit->mapping_slot_reserved = false;
	}
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

static int zns_base_metadata_write_block_flags_locked(
  	struct zns_base_c *c,
  	struct zns_base_metadata_stream *stream,
  	const void *buffer,
	sector_t *written_sector,
	unsigned int op_flags)
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
		ret = zns_base_submit_page(c, page, REQ_OP_WRITE, op_flags,
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

static int zns_base_metadata_write_block_locked(
	struct zns_base_c *c,
	struct zns_base_metadata_stream *stream,
	const void *buffer,
	sector_t *written_sector)
{
	return zns_base_metadata_write_block_flags_locked(c, stream, buffer,
		written_sector, REQ_FUA);
}

/* An SSTable is unreachable until its descriptor is published by a durable
 * Manifest.  Its body therefore needs one durability barrier after all pages,
 * rather than an expensive FUA on every 4 KiB page. */
static int zns_base_sstable_write_block_locked(
	struct zns_base_c *c,
	const void *buffer,
	sector_t *written_sector)
{
	return zns_base_metadata_write_block_flags_locked(c,
		&c->metadata.sstable, buffer, written_sector, 0);
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
	struct rb_node *node;
	size_t i, out = 0;

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

	spin_unlock(&c->lock);

	/* metadata.lock is held by the checkpoint caller. */
	for (i = 0; i < c->metadata.sstable_count; i++) {
		if (zns_base_sstable_apply_to_snapshot_locked(c,
			&c->metadata.sstables[i], snapshot)) {
			kvfree(snapshot);
			return NULL;
		}
	}

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
				      zone->start_sector, zone->reset_sectors,
				      GFP_KERNEL);
	if (ret)
		return ret;

	zone->write_pointer = zone->start_sector;
	zone->state = ZNS_BASE_ZONE_FREE;
	return 0;
}

static size_t zns_base_sstable_blocks(size_t entry_count)
{
	return 1 + DIV_ROUND_UP(entry_count,
		ZNS_BASE_BLOCK_SIZE / sizeof(struct zns_base_sstable_entry_disk));
}

static bool zns_base_sstable_zone_has_space(struct zns_base_zone *zone,
					     size_t blocks)
{
	sector_t remaining;

	if (zone->write_pointer < zone->start_sector ||
	    zone->write_pointer > zone->start_sector + zone->capacity_sectors)
		return false;
	remaining = zone->start_sector + zone->capacity_sectors -
		zone->write_pointer;
	return blocks <= remaining / SECTORS_PER_BLOCK;
}

/*
 * Normal MemTable flushes append to the current SSTable zone.  Compaction and
 * checkpoint consolidation set rotate=true and write their replacement table
 * to the other, empty zone.  Only after its Manifest is durable may the caller
 * reset the old zone.  This two-zone ping-pong is the metadata equivalent of
 * copy-on-write and is what makes packing multiple SSTables per zone safe.
 */
static int zns_base_choose_sstable_zone_locked(struct zns_base_c *c,
		 size_t blocks, bool rotate, unsigned int *zone_idx)
{
	struct zns_base_metadata_stream *stream = &c->metadata.sstable;
	struct zns_base_zone *active;
	unsigned int i;

	active = &c->zone_state.zones[stream->active_zone_idx];
	if (active->role != ZNS_BASE_ZONE_SSTABLE)
		return -EIO;

	if (!rotate) {
		if (!zns_base_sstable_zone_has_space(active, blocks))
			return -ENOSPC;
		active->state = ZNS_BASE_ZONE_ACTIVE;
		*zone_idx = stream->active_zone_idx;
		return 0;
	}

	for (i = 0; i < stream->zone_count; i++) {
		unsigned int idx = stream->first_zone_idx + i;
		struct zns_base_zone *zone = &c->zone_state.zones[idx];

		if (zone->role != ZNS_BASE_ZONE_SSTABLE)
			return -EIO;
		if (idx == stream->active_zone_idx)
			continue;
		if (zone->write_pointer != zone->start_sector)
			continue;
		if (!zns_base_sstable_zone_has_space(zone, blocks))
			return -ENOSPC;

		active->state = ZNS_BASE_ZONE_FULL;
		zone->state = ZNS_BASE_ZONE_ACTIVE;
		stream->active_zone_idx = idx;
		*zone_idx = idx;
		return 0;
	}

	return -ENOSPC;
}

static int zns_base_write_sstable_locked(struct zns_base_c *c,
					 const struct mapping_entry *entries,
					 size_t entry_count, u64 table_generation,
					 bool rotate,
					 struct zns_base_sstable_descriptor_disk *descriptor)
{
	struct zns_base_sstable_header_disk *header;
	struct zns_base_sstable_entry_disk entry;
	u8 *buffer;
	u32 entries_crc = ~0;
	sector_t start_sector;
	size_t i, page_entry, blocks;
	unsigned int previous_active;
	unsigned int zone_idx;
	int ret;

	blocks = zns_base_sstable_blocks(entry_count);
	previous_active = c->metadata.sstable.active_zone_idx;
	ret = zns_base_choose_sstable_zone_locked(c, blocks, rotate, &zone_idx);
	if (ret)
		return ret;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer) {
		if (rotate) {
			c->metadata.sstable.active_zone_idx = previous_active;
			c->zone_state.zones[previous_active].state =
				ZNS_BASE_ZONE_ACTIVE;
			c->zone_state.zones[zone_idx].state = ZNS_BASE_ZONE_FREE;
		}
		return -ENOMEM;
	}

	for (i = 0; i < entry_count; i++) {
		entry.logical_block = cpu_to_le64(entries[i].logical_block);
		entry.physical_sector = cpu_to_le64(entries[i].physical_sector);
		entry.seq = cpu_to_le64(entries[i].seq);
		entries_crc = crc32c(entries_crc, &entry, sizeof(entry));
	}

	header = (struct zns_base_sstable_header_disk *)buffer;
	header->magic = cpu_to_le32(ZNS_BASE_SSTABLE_MAGIC);
	header->version = cpu_to_le16(ZNS_BASE_FORMAT_VERSION);
	header->header_bytes = cpu_to_le16(sizeof(*header));
	header->generation = cpu_to_le64(table_generation);
	header->entry_count = cpu_to_le64(entry_count);
	header->min_logical_block = cpu_to_le64(entry_count ? entries[0].logical_block : 0);
	header->max_logical_block = cpu_to_le64(entry_count ? entries[entry_count - 1].logical_block : 0);
	header->max_seq = cpu_to_le64(table_generation);
	header->entries_bytes = cpu_to_le64(entry_count * sizeof(entry));
	header->entries_crc32c = cpu_to_le32(entries_crc);
	header->header_crc32c = 0;
	header->header_crc32c = cpu_to_le32(crc32c(~0, header, sizeof(*header)));

	ret = zns_base_sstable_write_block_locked(c, buffer, &start_sector);
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
			disk_entry->seq = cpu_to_le64(entries[i].seq);
		}
		ret = zns_base_sstable_write_block_locked(c, buffer, NULL);
		if (ret)
			goto out;
	}

	/* Publish through the Manifest only after every non-FUA SSTable page is
	 * durable.  A crash before this flush leaves only an unreachable tail. */
	ret = zns_base_submit_flush(c);
	if (ret)
		goto out;

	descriptor->zone_idx = cpu_to_le32(zone_idx);
	descriptor->start_sector = cpu_to_le64(start_sector);
	descriptor->length_bytes = cpu_to_le64(blocks * ZNS_BASE_BLOCK_SIZE);
	descriptor->min_logical_block = cpu_to_le64(entry_count ? entries[0].logical_block : 0);
	descriptor->max_logical_block = cpu_to_le64(entry_count ? entries[entry_count - 1].logical_block : 0);
	descriptor->generation = cpu_to_le64(table_generation);
	descriptor->payload_crc32c = cpu_to_le32(entries_crc);
	if (zns_base_sstable_zone_has_space(&c->zone_state.zones[zone_idx], 1))
		c->zone_state.zones[zone_idx].state = ZNS_BASE_ZONE_ACTIVE;
	else
		c->zone_state.zones[zone_idx].state = ZNS_BASE_ZONE_FULL;
	DMINFO("SSTable: zone=%u start=%llu entries=%zu crc=%08x",
	       zone_idx, (unsigned long long)start_sector, entry_count,
	       entries_crc);
out:
	if (ret && rotate) {
		/* No Manifest can reference this replacement yet.  Keep the old live
		 * zone selected; recovery will reset any written orphan tail. */
		c->metadata.sstable.active_zone_idx = previous_active;
		c->zone_state.zones[previous_active].state =
			ZNS_BASE_ZONE_ACTIVE;
		c->zone_state.zones[zone_idx].state = ZNS_BASE_ZONE_FULL;
	}
	kvfree(buffer);
	return ret;
}

static u64 zns_base_entries_max_seq(const struct mapping_entry *entries,
					 size_t entry_count)
{
	u64 max_seq = 0;
	size_t i;

	for (i = 0; i < entry_count; i++)
		max_seq = max(max_seq, entries[i].seq);
	return max_seq;
}

static int zns_base_publish_catalog_locked(
	struct zns_base_c *c,
	const struct zns_base_sstable_descriptor_disk *descriptors,
	unsigned int descriptor_count, u64 checkpoint_seq)
{
	unsigned int manifest_idx;
	int ret;

	ret = zns_base_manifest_rotate_and_write_locked(c, descriptors,
			descriptor_count, checkpoint_seq, &manifest_idx);
	if (ret)
		return ret;
	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_MANIFEST_WRITE))
		return -EIO;

	write_seqcount_begin(&c->metadata.catalog_seq);
	memcpy(c->metadata.sstables, descriptors,
	       descriptor_count * sizeof(*descriptors));
	c->metadata.sstable_count = descriptor_count;
	write_seqcount_end(&c->metadata.catalog_seq);
	c->metadata.checkpoint_seq = checkpoint_seq;
	c->metadata.checkpoint_generation++;
	c->metadata.checkpoint_manifest_zone_idx = manifest_idx;
	return 0;
}

static int zns_base_sstable_apply_to_snapshot_locked(
	struct zns_base_c *c,
	const struct zns_base_sstable_descriptor_disk *descriptor,
	struct mapping_entry *snapshot)
{
	struct zns_base_sstable_header_disk *header;
	struct zns_base_sstable_entry_disk *entry;
	u8 *buffer;
	u64 count, i = 0;
	sector_t sector;
	u32 crc = ~0;
	int ret;

	if (le32_to_cpu(descriptor->zone_idx) >= c->zone_state.nr_zones ||
	    c->zone_state.zones[le32_to_cpu(descriptor->zone_idx)].role !=
		ZNS_BASE_ZONE_SSTABLE)
		return -EINVAL;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	ret = zns_base_metadata_read_block(c,
		le64_to_cpu(descriptor->start_sector), buffer);
	if (ret)
		goto out;
	header = (struct zns_base_sstable_header_disk *)buffer;
	if (!zns_base_sstable_header_valid(header)) {
		DMERR("SSTable header invalid at %llu", (unsigned long long)
		      le64_to_cpu(descriptor->start_sector));
		ret = -EIO;
		goto out;
	}
	count = le64_to_cpu(header->entry_count);
	if (count > c->nr_logical_blocks) {
		DMERR("SSTable descriptor has invalid entry count %llu",
		      (unsigned long long)count);
		ret = -EIO;
		goto out;
	}

	for (sector = le64_to_cpu(descriptor->start_sector) + SECTORS_PER_BLOCK;
	     i < count; sector += SECTORS_PER_BLOCK) {
		unsigned int page_entries;

		ret = zns_base_metadata_read_block(c, sector, buffer);
		if (ret)
			goto out;
		page_entries = min_t(u64, count - i,
			ZNS_BASE_BLOCK_SIZE / sizeof(*entry));
		entry = (struct zns_base_sstable_entry_disk *)buffer;
		while (page_entries--) {
			struct mapping_entry candidate = {
				.logical_block = le64_to_cpu(entry->logical_block),
				.physical_sector = le64_to_cpu(entry->physical_sector),
				.seq = le64_to_cpu(entry->seq),
			};

			crc = crc32c(crc, entry, sizeof(*entry));
			if (candidate.logical_block >= c->nr_logical_blocks) {
				ret = -EIO;
				goto out;
			}
			zns_base_snapshot_consider(snapshot, &candidate);
			entry++;
			i++;
		}
	}
	if (crc != le32_to_cpu(descriptor->payload_crc32c)) {
		DMERR("SSTable CRC mismatch actual=%08x descriptor=%08x", crc,
		      le32_to_cpu(descriptor->payload_crc32c));
		ret = -EIO;
	}
out:
	kvfree(buffer);
	return ret;
}

/* After a compacted SSTable and its Manifest are durable, retire reverse-map
 * slots for duplicate records that lost to the selected input snapshot.  The
 * exact {LBA, PBA, seq} check keeps this safe even if another path already
 * invalidated the slot.  Caller holds metadata.lock. */
static int zns_base_sstable_invalidate_obsolete_locked(
	struct zns_base_c *c,
	const struct zns_base_sstable_descriptor_disk *descriptor,
	const struct mapping_entry *latest)
{
	struct zns_base_sstable_header_disk *header;
	struct zns_base_sstable_entry_disk *disk_entry;
	struct mapping_entry candidate;
	u8 *buffer;
	u64 count, index = 0;
	sector_t sector;
	int ret;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	ret = zns_base_metadata_read_block(c,
		le64_to_cpu(descriptor->start_sector), buffer);
	if (ret)
		goto out;
	header = (struct zns_base_sstable_header_disk *)buffer;
	if (!zns_base_sstable_header_valid(header)) {
		ret = -EIO;
		goto out;
	}
	count = le64_to_cpu(header->entry_count);
	if (count > c->nr_logical_blocks) {
		ret = -EIO;
		goto out;
	}

	for (sector = le64_to_cpu(descriptor->start_sector) + SECTORS_PER_BLOCK;
	     index < count; sector += SECTORS_PER_BLOCK) {
		unsigned int page_entries;

		ret = zns_base_metadata_read_block(c, sector, buffer);
		if (ret)
			goto out;
		page_entries = min_t(u64, count - index,
			ZNS_BASE_BLOCK_SIZE / sizeof(*disk_entry));
		disk_entry = (struct zns_base_sstable_entry_disk *)buffer;
		while (page_entries--) {
			candidate.logical_block =
				le64_to_cpu(disk_entry->logical_block);
			candidate.physical_sector =
				le64_to_cpu(disk_entry->physical_sector);
			candidate.seq = le64_to_cpu(disk_entry->seq);
			if (candidate.logical_block >= c->nr_logical_blocks) {
				ret = -EIO;
				goto out;
			}

			if (latest[candidate.logical_block].seq != candidate.seq ||
			    latest[candidate.logical_block].physical_sector !=
				candidate.physical_sector) {
				spin_lock(&c->lock);
				zns_base_invalidate_entry_slot_locked(c, &candidate);
				spin_unlock(&c->lock);
			}
			disk_entry++;
			index++;
		}
	}
	ret = 0;
out:
	kvfree(buffer);
	return ret;
}

static int zns_base_compact_sstables_locked(struct zns_base_c *c)
{
	struct mapping_entry *snapshot;
	struct mapping_entry *entries;
	struct zns_base_sstable_descriptor_disk *input_descriptors;
	struct zns_base_sstable_descriptor_disk output;
	unsigned int input_count = c->metadata.sstable_count;
	unsigned int input_zone;
	unsigned int output_zone;
	size_t entry_count = 0;
	u64 max_seq = 0;
	u64 started_ns;
	u64 elapsed_ns;
	size_t i;
	int ret;

	if (!input_count)
		return 0;
	input_descriptors = kvcalloc(input_count, sizeof(*input_descriptors),
		GFP_KERNEL);
	if (!input_descriptors)
		return -ENOMEM;
	started_ns = ktime_get_ns();
	c->metadata.compaction_running = true;
	c->metadata.compaction_count++;
	DMINFO("compaction start: inputs=%u catalog_generation=%llu",
	       input_count,
	       (unsigned long long)c->metadata.checkpoint_generation);
	memcpy(input_descriptors, c->metadata.sstables,
	       input_count * sizeof(*input_descriptors));
	input_zone = le32_to_cpu(input_descriptors[0].zone_idx);
	for (i = 1; i < input_count; i++) {
		if (le32_to_cpu(input_descriptors[i].zone_idx) != input_zone) {
			DMERR("compaction: live SSTables span multiple zones");
			ret = -EIO;
			goto out_descriptors;
		}
	}

	snapshot = kvcalloc(c->nr_logical_blocks, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot) {
		ret = -ENOMEM;
		goto out_descriptors;
	}
	for (i = 0; i < input_count; i++) {
		ret = zns_base_sstable_apply_to_snapshot_locked(c,
			&input_descriptors[i], snapshot);
		if (ret)
			goto out_snapshot;
	}
	for (i = 0; i < c->nr_logical_blocks; i++)
		if (snapshot[i].seq)
			entry_count++;
	if (!entry_count) {
		ret = -EIO;
		goto out_snapshot;
	}
	entries = kvcalloc(entry_count, sizeof(*entries), GFP_KERNEL);
	if (!entries) {
		ret = -ENOMEM;
		goto out_snapshot;
	}
	entry_count = 0;
	for (i = 0; i < c->nr_logical_blocks; i++) {
		if (!snapshot[i].seq)
			continue;
		entries[entry_count++] = snapshot[i];
		max_seq = max(max_seq, snapshot[i].seq);
	}
	ret = zns_base_write_sstable_locked(c, entries, entry_count, max_seq,
					      true, &output);
	if (ret)
		goto out_entries;
	output_zone = le32_to_cpu(output.zone_idx);
	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_SSTABLE_WRITE)) {
		/* The replacement is not referenced by any Manifest yet. */
		zns_base_reset_metadata_zone_locked(c, output_zone);
		c->metadata.sstable.active_zone_idx = input_zone;
		c->zone_state.zones[input_zone].state = ZNS_BASE_ZONE_ACTIVE;
		ret = -EIO;
		goto out_entries;
	}

	ret = zns_base_publish_catalog_locked(c, &output, 1, max_seq);
	if (ret)
		goto out_entries;
	c->metadata.checkpoint_sstable_zone_idx = output_zone;

	/* The output and Manifest are now durable.  Old duplicate data PBAs can
	 * finally be reflected in the reverse map; failures only leave conservative
	 * valid counts and are repaired by GC's exact validation pass. */
	for (i = 0; i < input_count; i++) {
		int invalidate_ret =
			zns_base_sstable_invalidate_obsolete_locked(c,
				&input_descriptors[i], snapshot);

		if (invalidate_ret)
			DMWARN("compaction: obsolete reverse-map scan failed for input %zu: %d",
			       i, invalidate_ret);
	}
	/* Every live input was packed into one zone.  Reset it once, after the
	 * replacement SSTable and its Manifest are durable. */
	if (input_zone != output_zone) {
		synchronize_srcu(&c->metadata.catalog_srcu);
		ret = zns_base_reset_metadata_zone_locked(c, input_zone);
	}
out_entries:
	kvfree(entries);
out_snapshot:
	kvfree(snapshot);
out_descriptors:
	elapsed_ns = ktime_get_ns() - started_ns;
	c->metadata.compaction_running = false;
	c->metadata.compaction_last_ns = elapsed_ns;
	c->metadata.compaction_max_ns =
		max(c->metadata.compaction_max_ns, elapsed_ns);
	DMINFO("compaction end: inputs=%u entries=%zu ret=%d duration_ms=%llu",
	       input_count, entry_count, ret,
	       (unsigned long long)div_u64(elapsed_ns, NSEC_PER_MSEC));
	kvfree(input_descriptors);
	return ret;
}

static int zns_base_persist_memtable_locked(struct zns_base_c *c,
	const struct mapping_entry *entries, size_t entry_count)
{
	struct zns_base_sstable_descriptor_disk *next;
	struct zns_base_sstable_descriptor_disk output;
	u64 max_seq;
	unsigned int next_count;
	int ret;

	if (!entry_count)
		return 0;
	next = kvcalloc(ZNS_BASE_MAX_MANIFEST_SSTABLES, sizeof(*next), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	if (c->metadata.sstable_count >= sstable_compaction_threshold) {
		ret = zns_base_compact_sstables_locked(c);
		if (ret)
			goto out_next;
	}
	if (c->metadata.sstable_count >= ZNS_BASE_MAX_MANIFEST_SSTABLES) {
		ret = -ENOSPC;
		goto out_next;
	}

	max_seq = zns_base_entries_max_seq(entries, entry_count);
	ret = zns_base_write_sstable_locked(c, entries, entry_count, max_seq,
					      false, &output);
	if (ret == -ENOSPC && c->metadata.sstable_count) {
		/* A packed zone can run out before the descriptor threshold.  Fold
		 * every live table into the other zone, then append this MemTable. */
		ret = zns_base_compact_sstables_locked(c);
		if (!ret)
			ret = zns_base_write_sstable_locked(c, entries,
				entry_count, max_seq, false, &output);
	}
	if (ret)
		goto out_next;
	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_SSTABLE_WRITE)) {
		ret = -EIO;
		goto out_next;
	}

	next_count = c->metadata.sstable_count;
	memcpy(next, c->metadata.sstables, next_count * sizeof(*next));
	next[next_count++] = output;
	ret = zns_base_publish_catalog_locked(c, next, next_count,
				       max(c->metadata.checkpoint_seq, max_seq));
	if (ret)
		goto out_next;

	if (c->metadata.sstable_count >= sstable_compaction_threshold)
		ret = zns_base_compact_sstables_locked(c);
out_next:
	kvfree(next);
	return ret;
}

static int zns_base_write_manifest_locked(struct zns_base_c *c,
						  const struct zns_base_sstable_descriptor_disk *descriptors,
						  unsigned int descriptor_count,
						  u64 checkpoint_seq)
{
	struct zns_base_manifest_header_disk *header;
	u8 *buffer;
	u32 descriptor_crc;
	int ret;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	header = (struct zns_base_manifest_header_disk *)buffer;
	if (descriptor_count > ZNS_BASE_MAX_MANIFEST_SSTABLES) {
		kvfree(buffer);
		return -ENOSPC;
	}
	memcpy(buffer + sizeof(*header), descriptors,
	       descriptor_count * sizeof(*descriptors));
	descriptor_crc = crc32c(~0, buffer + sizeof(*header),
			       descriptor_count * sizeof(*descriptors));

	header->magic = cpu_to_le32(ZNS_BASE_MANIFEST_MAGIC);
	header->version = cpu_to_le16(ZNS_BASE_FORMAT_VERSION);
	header->header_bytes = cpu_to_le16(sizeof(*header));
	header->generation = cpu_to_le64(c->metadata.checkpoint_generation + 1);
	header->checkpoint_last_seq = cpu_to_le64(checkpoint_seq);
	header->descriptor_bytes = cpu_to_le64(descriptor_count * sizeof(*descriptors));
	header->sstable_count = cpu_to_le32(descriptor_count);
	header->descriptors_crc32c = cpu_to_le32(descriptor_crc);
	header->header_crc32c = 0;
	header->header_crc32c = cpu_to_le32(crc32c(~0, header, sizeof(*header)));

	ret = zns_base_metadata_write_block_locked(c, &c->metadata.manifest,
						   buffer, NULL);
	kvfree(buffer);
	return ret;
}

static int zns_base_manifest_rotate_and_write_locked(
	struct zns_base_c *c,
	const struct zns_base_sstable_descriptor_disk *descriptors,
	unsigned int descriptor_count, u64 checkpoint_seq,
	unsigned int *written_zone_idx)
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
	ret = zns_base_write_manifest_locked(c, descriptors, descriptor_count,
					       checkpoint_seq);
	if (ret)
		return ret;

	*written_zone_idx = target_idx;
	return 0;
}

static int zns_base_checkpoint_locked(struct zns_base_c *c)
{
	struct mapping_entry *entries;
	struct zns_base_sstable_descriptor_disk descriptor;
	struct zns_base_sstable_descriptor_disk *old;
	u64 checkpoint_seq;
	unsigned int old_count;
	unsigned int i;
	size_t entry_count;
	int ret;

	old = kvcalloc(ZNS_BASE_MAX_MANIFEST_SSTABLES, sizeof(*old), GFP_KERNEL);
	if (!old)
		return -ENOMEM;
	entries = zns_base_build_snapshot(c, &entry_count, &checkpoint_seq);
	if (!entries) {
		ret = -ENOMEM;
		goto out_old;
	}

	/* WAL reclaim is a full on-media SSTable consolidation, not a RAM run. */
	memcpy(old, c->metadata.sstables,
	       c->metadata.sstable_count * sizeof(*old));
	old_count = c->metadata.sstable_count;
	ret = zns_base_write_sstable_locked(c, entries, entry_count,
						checkpoint_seq, old_count > 0,
						&descriptor);
	if (ret)
		goto out;
	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_SSTABLE_WRITE)) {
		unsigned int output_zone = le32_to_cpu(descriptor.zone_idx);

		if (old_count) {
			unsigned int input_zone = le32_to_cpu(old[0].zone_idx);

			zns_base_reset_metadata_zone_locked(c, output_zone);
			c->metadata.sstable.active_zone_idx = input_zone;
			c->zone_state.zones[input_zone].state =
				ZNS_BASE_ZONE_ACTIVE;
		}
		ret = -EIO;
		goto out;
	}

	ret = zns_base_publish_catalog_locked(c, &descriptor, 1,
					       checkpoint_seq);
	if (ret)
		goto out;
	if (zns_base_failpoint_hit(ZNS_BASE_FAIL_AFTER_MANIFEST_WRITE)) {
		ret = -EIO;
		goto out;
	}
	c->metadata.checkpoint_sstable_zone_idx =
		le32_to_cpu(descriptor.zone_idx);
	/* Readers that captured the previous catalog may sleep in block I/O.  SRCU
	 * waits only for that old epoch; readers of the new table do not starve the
	 * reclaim. */
	synchronize_srcu(&c->metadata.catalog_srcu);

	for (i = 0; i < old_count; i++) {
		unsigned int zone_idx = le32_to_cpu(old[i].zone_idx);
		unsigned int j;
		bool already_reset = false;

		if (zone_idx == le32_to_cpu(descriptor.zone_idx))
			continue;
		for (j = 0; j < i; j++) {
			if (le32_to_cpu(old[j].zone_idx) == zone_idx) {
				already_reset = true;
				break;
			}
		}
		if (!already_reset) {
			ret = zns_base_reset_metadata_zone_locked(c, zone_idx);
			if (ret)
				goto out;
		}
	}

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
	out_old:
	kvfree(old);
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

/* Take a lockless catalog snapshot, then pin all currently published SSTable
 * zones until the lookup finishes.  Compaction can do body I/O concurrently
 * and waits for this reader epoch only before resetting an old zone. */
static int zns_base_sstable_lookup(struct zns_base_c *c,
					 size_t logical_block,
					 struct mapping_entry *entry)
{
	struct zns_base_sstable_header_disk *header;
	struct zns_base_sstable_entry_disk *disk_entry;
	struct zns_base_sstable_descriptor_disk *descriptors;
	u8 *buffer;
	struct mapping_entry best = {};
	unsigned int catalog_seq;
	unsigned int descriptor_count;
	unsigned int table;
	int srcu_idx;
	int ret = -ENOENT;

	descriptors = kcalloc(ZNS_BASE_MAX_MANIFEST_SSTABLES,
			      sizeof(*descriptors), GFP_KERNEL);
	if (!descriptors)
		return -ENOMEM;
	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer) {
		kfree(descriptors);
		return -ENOMEM;
	}

	srcu_idx = srcu_read_lock(&c->metadata.catalog_srcu);
	do {
		catalog_seq = read_seqcount_begin(&c->metadata.catalog_seq);
		descriptor_count = READ_ONCE(c->metadata.sstable_count);
		if (descriptor_count > ZNS_BASE_MAX_MANIFEST_SSTABLES) {
			ret = -EIO;
			goto out;
		}
		memcpy(descriptors, c->metadata.sstables,
		       descriptor_count * sizeof(*descriptors));
	} while (read_seqcount_retry(&c->metadata.catalog_seq, catalog_seq));

	for (table = 0; table < descriptor_count; table++) {
		const struct zns_base_sstable_descriptor_disk *descriptor =
			&descriptors[table];
		u64 left, right, entry_count;

		if (logical_block < le64_to_cpu(descriptor->min_logical_block) ||
		    logical_block > le64_to_cpu(descriptor->max_logical_block))
			continue;
		ret = zns_base_metadata_read_block(c,
			le64_to_cpu(descriptor->start_sector), buffer);
		if (ret)
			goto out;
		header = (struct zns_base_sstable_header_disk *)buffer;
		if (!zns_base_sstable_header_valid(header)) {
			ret = -EIO;
			goto out;
		}
		entry_count = le64_to_cpu(header->entry_count);
		left = 0;
		right = entry_count;
		while (left < right) {
			u64 middle = left + (right - left) / 2;
			sector_t sector = le64_to_cpu(descriptor->start_sector) +
				SECTORS_PER_BLOCK * (1 + middle /
				 (ZNS_BASE_BLOCK_SIZE / sizeof(*disk_entry)));
			unsigned int offset = middle %
				(ZNS_BASE_BLOCK_SIZE / sizeof(*disk_entry));

			ret = zns_base_metadata_read_block(c, sector, buffer);
			if (ret)
				goto out;
			disk_entry = (struct zns_base_sstable_entry_disk *)buffer + offset;
			if (le64_to_cpu(disk_entry->logical_block) < logical_block)
				left = middle + 1;
			else
				right = middle;
		}
		if (left == entry_count)
			continue;
		ret = zns_base_metadata_read_block(c,
			le64_to_cpu(descriptor->start_sector) + SECTORS_PER_BLOCK *
			(1 + left / (ZNS_BASE_BLOCK_SIZE / sizeof(*disk_entry))), buffer);
		if (ret)
			goto out;
		disk_entry = (struct zns_base_sstable_entry_disk *)buffer +
			(left % (ZNS_BASE_BLOCK_SIZE / sizeof(*disk_entry)));
		if (le64_to_cpu(disk_entry->logical_block) == logical_block &&
		    le64_to_cpu(disk_entry->seq) >= best.seq) {
			best.logical_block = logical_block;
			best.physical_sector = le64_to_cpu(disk_entry->physical_sector);
			best.seq = le64_to_cpu(disk_entry->seq);
		}
	}
	if (best.seq) {
		*entry = best;
		ret = 0;
	} else {
		ret = -ENOENT;
	}
out:
	srcu_read_unlock(&c->metadata.catalog_srcu, srcu_idx);
	kvfree(buffer);
	kfree(descriptors);
	return ret;
}

static bool zns_base_manifest_page_valid(
	struct zns_base_manifest_header_disk *header,
	struct zns_base_sstable_descriptor_disk *descriptors)
{
	u32 stored_crc = le32_to_cpu(header->header_crc32c);
	u32 actual_crc;
	unsigned int count = le32_to_cpu(header->sstable_count);

	if (le32_to_cpu(header->magic) != ZNS_BASE_MANIFEST_MAGIC ||
	    le16_to_cpu(header->version) != ZNS_BASE_FORMAT_VERSION ||
	    le16_to_cpu(header->header_bytes) != sizeof(*header) ||
	    count == 0 || count > ZNS_BASE_MAX_MANIFEST_SSTABLES ||
	    le64_to_cpu(header->descriptor_bytes) != count * sizeof(*descriptors) ||
	    le32_to_cpu(header->descriptors_crc32c) !=
		crc32c(~0, descriptors, count * sizeof(*descriptors)))
		return false;

	header->header_crc32c = 0;
	actual_crc = crc32c(~0, header, sizeof(*header));
	header->header_crc32c = cpu_to_le32(stored_crc);
	return actual_crc == stored_crc;
}

/*
 * A valid Manifest owns at most one packed SSTable zone.  Anything in the
 * other zone is an unpublished compaction/checkpoint output and can be reset.
 * With no Manifest, WAL replay is the only source of mappings, so both
 * SSTable zones are orphaned and may be cleared.
 */
static int zns_base_recover_sstable_zones(struct zns_base_c *c,
	const struct zns_base_sstable_descriptor_disk *descriptors,
	unsigned int descriptor_count)
{
	struct zns_base_metadata_stream *stream = &c->metadata.sstable;
	unsigned int live_zone = ZNS_BASE_NO_ZONE;
	unsigned int i;
	int ret;

	if (descriptor_count) {
		live_zone = le32_to_cpu(descriptors[0].zone_idx);
		if (live_zone < stream->first_zone_idx ||
		    live_zone >= stream->first_zone_idx + stream->zone_count)
			return -EIO;
		for (i = 1; i < descriptor_count; i++) {
			if (le32_to_cpu(descriptors[i].zone_idx) != live_zone)
				return -EIO;
		}
	}

	for (i = 0; i < stream->zone_count; i++) {
		unsigned int idx = stream->first_zone_idx + i;
		struct zns_base_zone *zone = &c->zone_state.zones[idx];
		sector_t end = zone->start_sector + zone->capacity_sectors;

		if (idx == live_zone) {
			if (zone->write_pointer == zone->start_sector)
				return -EIO;
			zone->state = zone->write_pointer < end ?
				ZNS_BASE_ZONE_ACTIVE : ZNS_BASE_ZONE_FULL;
			continue;
		}

		if (zone->write_pointer != zone->start_sector) {
			ret = zns_base_reset_metadata_zone_locked(c, idx);
			if (ret)
				return ret;
		} else {
			zone->state = ZNS_BASE_ZONE_FREE;
		}
	}

	if (live_zone == ZNS_BASE_NO_ZONE) {
		live_zone = stream->first_zone_idx;
		c->zone_state.zones[live_zone].state = ZNS_BASE_ZONE_ACTIVE;
	}
	stream->active_zone_idx = live_zone;
	return 0;
}

static int zns_base_manifest_recover(struct zns_base_c *c)
{
	struct zns_base_zone *manifest_zone;
	struct zns_base_manifest_header_disk *manifest;
	struct zns_base_sstable_descriptor_disk *latest;
	struct zns_base_sstable_descriptor_disk *descriptors;
	struct mapping_entry *snapshot;
	u8 *buffer;
	sector_t sector;
	u64 latest_generation = 0;
	u64 max_seq = 0;
	unsigned int manifest_idx;
	unsigned int latest_manifest_idx = ZNS_BASE_NO_ZONE;
	unsigned int latest_count = 0;
	size_t i;
	int ret = 0;

	buffer = kvzalloc(ZNS_BASE_BLOCK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	latest = kvcalloc(ZNS_BASE_MAX_MANIFEST_SSTABLES,
			 sizeof(*latest), GFP_KERNEL);
	if (!latest) {
		kvfree(buffer);
		return -ENOMEM;
	}

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
		descriptors = (struct zns_base_sstable_descriptor_disk *)
			(buffer + sizeof(*manifest));
		if (!zns_base_manifest_page_valid(manifest, descriptors))
			continue;
		if (le64_to_cpu(manifest->generation) >= latest_generation) {
			latest_generation = le64_to_cpu(manifest->generation);
			latest_manifest_idx = manifest_idx;
			latest_count = le32_to_cpu(manifest->sstable_count);
			memcpy(latest, descriptors, latest_count * sizeof(*latest));
			c->metadata.checkpoint_seq =
				le64_to_cpu(manifest->checkpoint_last_seq);
		}
		}
	}

	ret = zns_base_recover_sstable_zones(c, latest, latest_count);
	if (ret)
		goto out;

	if (!latest_generation)
		goto out;

	snapshot = kvcalloc(c->nr_logical_blocks, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < latest_count; i++) {
		ret = zns_base_sstable_apply_to_snapshot_locked(c, &latest[i], snapshot);
		if (ret)
			goto out_snapshot;
	}
	for (i = 0; i < c->nr_logical_blocks; i++) {
		struct zns_base_zone *zone;
		unsigned int slot;

		if (!snapshot[i].seq)
			continue;
		ret = zns_base_get_zone_slot(c, snapshot[i].physical_sector,
					     &zone, &slot);
		if (ret)
			goto out_snapshot;
		zone->slots[slot].logical_block = i;
		zone->slots[slot].seq = snapshot[i].seq;
		zone->slots[slot].valid = true;
		zone->valid_blocks++;
		max_seq = max(max_seq, snapshot[i].seq);
	}
	kvfree(snapshot);

	c->metadata.checkpoint_generation = latest_generation;
	c->metadata.checkpoint_manifest_zone_idx = latest_manifest_idx;
	c->metadata.manifest.active_zone_idx = latest_manifest_idx;
	write_seqcount_begin(&c->metadata.catalog_seq);
	memcpy(c->metadata.sstables, latest, latest_count * sizeof(*latest));
	c->metadata.sstable_count = latest_count;
	write_seqcount_end(&c->metadata.catalog_seq);
	c->metadata.checkpoint_sstable_zone_idx = latest_count == 1 ?
		le32_to_cpu(latest[0].zone_idx) : ZNS_BASE_NO_ZONE;
	c->mapping.next_seq = max_seq + 1;
	goto out;
out_snapshot:
	kvfree(snapshot);
out:
	if (ret)
		DMERR("manifest/SSTable recovery failed: %d", ret);
	kvfree(latest);
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
	ret = mapping_reserve_write_slot(c, logical_block);
	if (ret)
		return ret;
	ret = mapping_lookup(c, logical_block, &old_entry);
	if (!ret && old_entry.seq >= seq) {
		mapping_release_write_slot(c);
		return 0;
	}
	if (ret != 0 && ret != -ENOENT) {
		mapping_release_write_slot(c);
		return ret;
	}

	spin_lock(&c->lock);
	if (ret == 0) {
		ret = zns_base_get_zone_slot(c, old_entry.physical_sector,
						     &zone, &slot);
		if (!ret && zone->slots[slot].valid &&
		    zone->slots[slot].logical_block == logical_block &&
		    zone->slots[slot].seq == old_entry.seq) {
			zone->slots[slot].valid = false;
			zone->valid_blocks--;
		}
	} else
		ret = 0;

	if (!ret)
		ret = zns_base_get_zone_slot(c, physical_sector, &zone, &slot);
	if (!ret && !zone->slots[slot].valid) {
		zone->slots[slot].logical_block = logical_block;
		zone->slots[slot].seq = seq;
		zone->slots[slot].valid = true;
		zone->valid_blocks++;
	}
	if (!ret)
		ret = mapping_update(c, logical_block, physical_sector, seq);
	spin_unlock(&c->lock);
	mapping_release_write_slot(c);

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

				ret = zns_base_replay_wal_put(c,
					le64_to_cpu(record->logical_block),
					le64_to_cpu(record->physical_sector),
					le64_to_cpu(record->seq));
				if (ret)
					goto out;
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

	/* Caller holds wal.lock. */
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

	/* Caller holds mapping_wal_lock and wal.lock. */
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

	/* Caller holds mapping_wal_lock and wal.lock. */
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
			 * Both WAL zones are full.  Catalog serialization is needed only
			 * for this rare checkpoint, not for ordinary WAL page writes.
			 */
			mutex_lock(&c->metadata.lock);
			ret = zns_base_checkpoint_locked(c);
			mutex_unlock(&c->metadata.lock);
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

	mutex_lock(&wal->lock);

	list_for_each_entry_safe(commit, next,
				 &wal->pending_commits, node) {
		commit->result = error;
		zns_base_release_pending_slot(c, commit->new_zone,
					      commit->new_slot,
					      commit->logical_block);
		if (commit->mapping_slot_reserved) {
			mapping_release_write_slot(c);
			commit->mapping_slot_reserved = false;
		}
		list_move_tail(&commit->node, &done_commits);
  	}

  	zns_base_wal_reset_page_locked(wal);
  	wal->flush_scheduled = false;

	mutex_unlock(&wal->lock);

  	zns_base_wal_finish_commits(c, &done_commits);
}

static void zns_base_wal_flush_work(struct work_struct *work)
{
  	struct zns_base_wal_state *wal;
  	struct zns_base_metadata_state *metadata;
  	struct zns_base_c *c;
  	struct zns_base_wal_pending_commit *commit;
	struct zns_base_wal_pending_commit *next;
	LIST_HEAD(done_commits);
	bool published = false;
	int ret = 0;

	wal = container_of(work, struct zns_base_wal_state,
			   flush_work);
  	metadata = container_of(wal, struct zns_base_metadata_state, wal);
  	c = container_of(metadata, struct zns_base_c, metadata);

	/* WAL order and mapping publication share one worker.  SSTable compaction
	 * uses metadata.lock independently and cannot block ordinary WAL pages. */
	mutex_lock(&c->mapping_wal_lock);
	mutex_lock(&wal->lock);

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
		if (commit->mapping_slot_reserved) {
			mapping_release_write_slot(c);
			commit->mapping_slot_reserved = false;
		}
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

	mutex_unlock(&wal->lock);
	mutex_unlock(&c->mapping_wal_lock);

	zns_base_wal_finish_commits(c, &done_commits);
	if (published)
		zns_base_schedule_gc(c);
}

/* Queue one already-built WAL batch immediately.  Grouping comes from the
 * foreground I/O queue drain and the 4 KiB page boundary, never from time. */
static void zns_base_wal_schedule_flush(struct zns_base_c *c, bool immediate)
{
	struct zns_base_wal_state *wal = &c->metadata.wal;
	bool queue = false;

	(void)immediate;

	mutex_lock(&wal->lock);
	if (wal->record_count && !wal->flush_error) {
		if (!wal->flush_scheduled) {
			wal->flush_scheduled = true;
			queue = true;
		}
	}
	mutex_unlock(&wal->lock);

	if (queue)
		queue_work(c->wal_wq, &wal->flush_work);
}

static int zns_base_wal_flush_sync(struct zns_base_c *c)
{
	struct zns_base_wal_state *wal = &c->metadata.wal;
	bool has_records;
	int ret;

	for (;;) {
		mutex_lock(&wal->lock);
		has_records = wal->record_count != 0;
		ret = wal->flush_error;
		if (has_records && !ret)
			wal->flush_scheduled = true;
		mutex_unlock(&wal->lock);

		if (ret || !has_records)
			return ret;

		/* queue_work() may observe an older instance that is still pending or
		 * executing.  flush_work() waits for that instance; the loop then
		 * rechecks record_count and queues again if a newly staged partial page
		 * remains. */
		queue_work(c->wal_wq, &wal->flush_work);
		flush_work(&wal->flush_work);
	}
}

static int zns_base_wal_publish_gc_locked(
	struct zns_base_c *c,
	struct zns_base_wal_pending_commit *commit)
{
	struct mapping_entry current_entry;
	int ret;

	/* Capacity was reserved before the GC copy was issued. */
	ret = mapping_lookup(c, commit->logical_block, &current_entry);

	spin_lock(&c->lock);

	if (!commit->new_zone->slots[commit->new_slot].pending ||
	    commit->new_zone->slots[commit->new_slot].logical_block !=
	    commit->logical_block) {
		ret = -EIO;
		goto out_unlock;
	}

	if (ret == -ENOENT ||
	    (!ret && (current_entry.physical_sector !=
		      commit->expected_physical_sector ||
		      current_entry.seq != commit->expected_seq))) {
		/* A newer foreground write won while this block was being copied. */
		commit->new_zone->slots[commit->new_slot].pending = false;
		commit->new_zone->pending_blocks--;

		if (commit->old_zone->slots[commit->old_slot].valid &&
		    commit->old_zone->slots[commit->old_slot].logical_block ==
		    commit->logical_block &&
		    commit->old_zone->slots[commit->old_slot].seq ==
		    commit->expected_seq) {
			commit->old_zone->slots[commit->old_slot].valid = false;
			commit->old_zone->valid_blocks--;
		}

		ret = 0;
		goto out_unlock;
	}

	if (ret)
		goto out_unlock;

	ret = mapping_update(c, commit->logical_block,
			     commit->new_physical_sector, commit->seq);
	if (ret)
		goto out_unlock;

	commit->new_zone->slots[commit->new_slot].pending = false;
	commit->new_zone->pending_blocks--;
	commit->new_zone->slots[commit->new_slot].seq = commit->seq;
	commit->new_zone->slots[commit->new_slot].valid = true;
	commit->new_zone->valid_blocks++;
	c->gc_moved_blocks++;

	if (commit->old_zone->slots[commit->old_slot].valid &&
	    commit->old_zone->slots[commit->old_slot].logical_block ==
	    commit->logical_block &&
	    commit->old_zone->slots[commit->old_slot].seq ==
	    commit->expected_seq) {
		commit->old_zone->slots[commit->old_slot].valid = false;
		commit->old_zone->valid_blocks--;
	}

out_unlock:
	spin_unlock(&c->lock);
	if (commit->mapping_slot_reserved) {
		mapping_release_write_slot(c);
		commit->mapping_slot_reserved = false;
	}
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
	if (!sstable_compaction_threshold ||
	    sstable_compaction_threshold > ZNS_BASE_MAX_MANIFEST_SSTABLES) {
		ti->error = "invalid sstable_compaction_threshold";
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
	if (zns_base_sstable_blocks(c->nr_logical_blocks) >
	    c->zone_state.zones[c->metadata.sstable.first_zone_idx].nr_blocks) {
		ti->error = "full mapping SSTable does not fit in one metadata zone";
		mapping_destroy(c);
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOSPC;
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
	INIT_LIST_HEAD(&c->data_write_queue);
	init_waitqueue_head(&c -> spare_waitq);
	init_waitqueue_head(&c -> gc_waitq);
	init_waitqueue_head(&c -> data_waitq);

	c -> io_work_scheduled = false;
	c->foreground_data_inflight = 0;
	c->data_write_inflight = false;
	c->data_write_error = 0;
	c -> gc_scheduled = false;
	c -> gc_running = false;
	c->gc_error = 0;
	c->gc_last_error = 0;
	c->quiescing = false;
	c -> stopping = false;

	c->metadata.wal.flush_scheduled = false;

	c->io_wq = alloc_workqueue("zns-base-io",
  			    WQ_MEM_RECLAIM, 1);

	if (!c->io_wq) {
		ti->error = "failed to allocate foreground I/O workqueue";
		mapping_destroy(c);
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	/* DATA completions must run independently from the foreground dispatcher.
	 * The dispatcher may wait for MemTable capacity that only a completion can
	 * release, so sharing a single-threaded workqueue would self-deadlock. */
	c->data_wq = alloc_workqueue("zns-base-data",
				     WQ_MEM_RECLAIM, 1);
	if (!c->data_wq) {
		ti->error = "failed to allocate DATA completion workqueue";
		destroy_workqueue(c->io_wq);
		c->io_wq = NULL;
		mapping_destroy(c);
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	c->wal_wq = alloc_workqueue("zns-base-wal",
  			    WQ_MEM_RECLAIM, 1);
	if (!c->wal_wq) {
		ti->error = "failed to allocate WAL workqueue";
		destroy_workqueue(c->data_wq);
		c->data_wq = NULL;
		destroy_workqueue(c->io_wq);
		c->io_wq = NULL;
		mapping_destroy(c);
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	c->gc_wq = alloc_workqueue("zns-base-gc",
					WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!c->gc_wq) {
		ti->error = "failed to allocate GC workqueue";
		destroy_workqueue(c->data_wq);
		c->data_wq = NULL;
		destroy_workqueue(c->io_wq);
		c->io_wq = NULL;
		destroy_workqueue(c->wal_wq);
  		c->wal_wq = NULL;
		mapping_destroy(c);
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
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
		destroy_workqueue(c->data_wq);
		c->data_wq = NULL;
		destroy_workqueue(c->io_wq);
		c->io_wq = NULL;

		mapping_destroy(c);
		zns_base_metadata_destroy(c);
		zns_base_zone_destroy(c);
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
	int wal_ret;

	/* Stop accepting new upper bios, but keep internal mapping waits and WAL
	 * publication alive until every already-issued data write has staged its
	 * record. */
	spin_lock(&c -> lock);
	c->quiescing = true;
	spin_unlock(&c -> lock);

	wake_up_all(&c -> spare_waitq);
	wake_up_all(&c -> gc_waitq);

	/* Drain the foreground dispatcher and all data completion work.  Completion
	 * work can requeue io_work for an ordering-boundary FLUSH, hence the second
	 * flush after foreground_data_inflight reaches zero. */
	flush_workqueue(c->io_wq);
	wait_event(c->data_waitq, !READ_ONCE(c->foreground_data_inflight));
	flush_workqueue(c->data_wq);
	flush_workqueue(c->io_wq);
	spin_lock(&c->lock);
	WARN_ON_ONCE(c->data_write_inflight ||
		     !list_empty(&c->data_write_queue));
	spin_unlock(&c->lock);

	/* A running GC is allowed to finish its current safe round while foreground
	 * I/O drains.  Once the dispatcher is empty, quiescing prevents it from
	 * being scheduled again. */
	cancel_work_sync(&c -> gc_work);

	/* Graceful target removal is a durability boundary.  Do not discard the
	 * final partial WAL page: write and publish until record_count becomes zero. */
	wal_ret = zns_base_wal_flush_sync(c);
	if (wal_ret)
		DMERR("failed to flush partial WAL during target removal: %d", wal_ret);

	/* No WAL publication may still be using the mapping flush worker after this
	 * point.  A cancelled frozen MemTable remains recoverable from the WAL. */
	cancel_work_sync(&c->mapping.flush_work);

	spin_lock(&c->lock);
	c->stopping = true;
	spin_unlock(&c->lock);
	wake_up_all(&c->spare_waitq);
	wake_up_all(&c->gc_waitq);

	cancel_work_sync(&c->io_work);
	cancel_work_sync(&c->gc_work);
	cancel_work_sync(&c->metadata.wal.flush_work);
	/* On success this is a no-op.  On a real lower-device error it releases
	 * otherwise stranded commit objects after the error has been reported. */
	zns_base_wal_abort_pending(c, wal_ret ? wal_ret : -EIO);

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
	destroy_workqueue(c->data_wq);
	c->data_wq = NULL;
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
	u64 compaction_count;
	u64 compaction_last_ns;
	u64 compaction_max_ns;
	unsigned int persistent_sstables;
	unsigned int compaction_running;
	unsigned int data_write_queued = 0;
	unsigned int data_write_queued_blocks = 0;
	unsigned int data_write_inflight;
	size_t mapping_reserved_slots;
	unsigned int active_data_zone;
	struct zns_base_data_write *queued_write;
	sector_t active_data_wp;
	sector_t wal_used_sectors;
	sector_t wal_capacity_sectors;
	int gc_error;
	int gc_last_error;
	int data_write_error;
	int wal_error;
	unsigned int sz = 0;

	(void)status_flags;

	if (type == STATUSTYPE_TABLE) {
		DMEMIT("%s", c->dev->name);
		return;
	}

	mutex_lock(&c->metadata.lock);
	checkpoint_seq = c->metadata.checkpoint_seq;
	checkpoint_generation = c->metadata.checkpoint_generation;
	checkpoint_manifest = c->metadata.checkpoint_manifest_zone_idx;
	checkpoint_sstable = c->metadata.checkpoint_sstable_zone_idx;
	persistent_sstables = c->metadata.sstable_count;
	compaction_running = c->metadata.compaction_running;
	compaction_count = c->metadata.compaction_count;
	compaction_last_ns = c->metadata.compaction_last_ns;
	compaction_max_ns = c->metadata.compaction_max_ns;
	manifest_active = c->metadata.manifest.active_zone_idx;
	sstable_active = c->metadata.sstable.active_zone_idx;
	mutex_unlock(&c->metadata.lock);

	mutex_lock(&c->metadata.wal.lock);
	wal_zone_idx = c->metadata.wal.stream.active_zone_idx;
	wal_records = c->metadata.wal.record_count;
	wal_generation = c->metadata.wal.stream.generation;
	wal_error = c->metadata.wal.flush_error;
	wal_zone = &c->zone_state.zones[wal_zone_idx];
	wal_used_sectors = wal_zone->write_pointer - wal_zone->start_sector;
	wal_capacity_sectors = wal_zone->capacity_sectors;
	mutex_unlock(&c->metadata.wal.lock);

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

	gc_runs = c->gc_runs;
	gc_reset_count = c->gc_reset_count;
	gc_moved_blocks = c->gc_moved_blocks;
	gc_error = c->gc_error;
	gc_last_error = c->gc_last_error;
	data_write_error = c->data_write_error;
	data_write_inflight = c->data_write_inflight;
	list_for_each_entry(queued_write, &c->data_write_queue, node) {
		data_write_queued++;
		data_write_queued_blocks += queued_write->mapping_count;
	}
	mapping_reserved_slots = c->mapping.reserved_slots;
	active_data_zone = c->zone_state.active_zone_idx;
	active_data_wp = c->zone_state.zones[active_data_zone].write_pointer;
	spin_unlock(&c->lock);

	DMEMIT("data_active=%u data_free=%u data_full=%u gc_dest=%u gc_victim=%u ",
		data_active, data_free, data_full, data_gc_dest, data_gc_victim);
	DMEMIT("gc_runs=%llu gc_resets=%llu gc_moved_blocks=%llu gc_error=%d gc_last_error=%d ",
		(unsigned long long)gc_runs,
		(unsigned long long)gc_reset_count,
		(unsigned long long)gc_moved_blocks, gc_error, gc_last_error);
	DMEMIT("data_write_inflight=%u data_write_queued=%u data_write_queued_blocks=%u data_write_error=%d mapping_reserved_slots=%zu active_data_zone=%u active_data_wp=%llu ",
		data_write_inflight, data_write_queued, data_write_queued_blocks,
		data_write_error, mapping_reserved_slots,
		active_data_zone, (unsigned long long)active_data_wp);
	DMEMIT("wal_zone=%u wal_generation=%llu wal_used_blocks=%llu wal_capacity_blocks=%llu wal_staged_records=%u wal_error=%d ",
		wal_zone_idx, (unsigned long long)wal_generation,
		(unsigned long long)(wal_used_sectors / SECTORS_PER_BLOCK),
		(unsigned long long)(wal_capacity_sectors / SECTORS_PER_BLOCK),
		wal_records, wal_error);
	DMEMIT("persistent_sstables=%u checkpoint_seq=%llu checkpoint_generation=%llu manifest_active_zone=%u manifest_checkpoint_zone=%u sstable_active_zone=%u sstable_checkpoint_zone=%u",
		persistent_sstables,
		(unsigned long long)checkpoint_seq,
		(unsigned long long)checkpoint_generation, manifest_active,
		checkpoint_manifest, sstable_active, checkpoint_sstable);
	DMEMIT(" compaction_running=%u compaction_count=%llu compaction_last_ms=%llu compaction_max_ms=%llu",
		compaction_running, (unsigned long long)compaction_count,
		(unsigned long long)div_u64(compaction_last_ns, NSEC_PER_MSEC),
		(unsigned long long)div_u64(compaction_max_ns, NSEC_PER_MSEC));
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
