/* Minimal mocks for extracted production helpers, not a kernel emulator. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
typedef uint64_t u64;
typedef uint64_t sector_t;
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define MAPPING_TOMBSTONE UINT64_MAX
#define BLK_STS_IOERR 1
#define BLK_STS_RESOURCE 2
#define DMERR(...) ((void)0)
#define BLOCK_SECTORS 8
#define WAL_PAGE_MAX_RECORDS 127
#define WAL_PAGE_SECTORS 8
#define DIV_ROUND_UP_ULL(n, d) (((n) + (d) - 1) / (d))
#define round_up(n, d) ((((n) + (d) - 1) / (d)) * (d))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define ZONE_NONE UINT32_MAX
enum zone_tag { ZONE_TAG_FREE, ZONE_TAG_USER_DATA, ZONE_TAG_GC_DATA,
                ZONE_TAG_WAL, ZONE_TAG_SSTABLE, ZONE_TAG_COUNT };
struct skiplist_node { u64 lba, phys; struct skiplist_node *forward[1]; };
struct skiplist { struct skiplist_node *head; unsigned int count; };
struct sstable_record { uint64_t lba, phys; };
struct zone_pool {
    unsigned int nr_zones, active_zone[ZONE_TAG_COUNT];
    enum zone_tag zone_tag[8];
    sector_t zone_sectors, wp[8], dispatch_wp[8];
    unsigned int sstable_live_count[8];
    u64 wal_gen[8], wal_next_gen;
};
struct zns_base_c {
    int lock;
    int wal_ckpt_inflight;
    struct skiplist *memtable, *frozen_memtable;
    bool frozen_published, checkpoint_failed, metadata_failed;
    bool wal_rotation_pending, wal_rotation_credit;
    bool gc_active, sstable_rotation_pending;
    struct zone_pool *zp;
    sector_t gc_wal_budget, gc_wal_reserve;
    u64 read_view_epoch;
    unsigned int nr_sstables;
    struct sstable_info *sstables;
    struct fake_device { void *bdev; } *dev;
};
struct gc_candidate { u64 lba; sector_t phys; };
struct gc_live_entry { u64 lba; sector_t phys; bool used; };
struct gc_live_map { struct gc_live_entry *entries; unsigned int capacity, count; };
static unsigned int gc_reserved_zones = 2;
static unsigned int compaction_k = 4;
static bool fail_alloc;
static unsigned int destroy_count;
static int put_result;
static void *test_alloc(size_t n, size_t size) {
    if (fail_alloc) return NULL;
    return calloc(n, size);
}
#define kvcalloc(n, s, flags) test_alloc(n, s)
#define kvmalloc_array(n, s, flags) test_alloc(n, s)
#define kmalloc_array(n, s, flags) test_alloc(n, s)
#define kvfree(p) free(p)
#define kfree(p) free(p)
#define spin_lock_irq(p) ((void)(p))
#define spin_unlock_irq(p) ((void)(p))
static int skiplist_lookup(struct skiplist *s, u64 lba, u64 *phys) {
    for (struct skiplist_node *n = s->head->forward[0]; n; n = n->forward[0])
        if (n->lba == lba) { *phys = n->phys; return 1; }
    return 0;
}
static void skiplist_destroy(struct skiplist *s) { (void)s; destroy_count++; }
static int mapping_put_if_match(struct zns_base_c *c, u64 lba, u64 old, u64 phys) {
    (void)c; (void)lba; (void)old; (void)phys;
    return put_result;
}
struct sstable_info { sector_t phys; u64 seq_no, record_count, min_lba, max_lba; };
struct zns_read_pin { unsigned int zone; };
struct bio {
    int bi_status;
    struct { sector_t bi_sector; } bi_iter;
    struct zns_read_pin pin;
};
struct sstable_read_ctx {
    struct zns_base_c *c;
    struct bio *orig_bio;
    u64 lba;
    sector_t offset_in_block;
    struct sstable_info *candidates;
    unsigned int nr_candidates, idx;
    void *sec_buf;
    int best_found;
    u64 best_seq;
    sector_t best_phys;
    u64 view_epoch;
};
static int completed, submitted, zeroed, restarted, pins;
#define dm_per_bio_data(b, size) (&(b)->pin)
static unsigned int zone_of(struct zone_pool *zp, sector_t p) { return p / zp->zone_sectors; }
static void zone_read_get(struct zone_pool *zp, unsigned int z) { (void)zp; (void)z; pins++; }
static void zone_read_put(struct zone_pool *zp, unsigned int z) { (void)zp; (void)z; pins--; }
static void bio_endio(struct bio *b) { (void)b; completed++; }
static void zero_fill_bio(struct bio *b) { (void)b; zeroed++; }
static void bio_set_dev(struct bio *b, void *dev) { (void)b; (void)dev; }
static void submit_bio_deferred(struct bio *b) { (void)b; submitted++; }
static void sstable_read_next_candidate(struct sstable_read_ctx *ctx) { (void)ctx; restarted++; }
