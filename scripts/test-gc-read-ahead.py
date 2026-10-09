#!/usr/bin/env python3
"""Exercise the production GC read helper with fake I/O, without devices.

Does not validate kernel concurrency, mapping publication or crash recovery.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/dm-zns-base.c").read_text()


class GCReadAheadTests(unittest.TestCase):
    def test_production_reader(self):
        start = SOURCE.index("static int zns_base_gc_read_page(")
        end = SOURCE.index("\n}\n", start) + 3
        prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint64_t u64;
typedef uint64_t sector_t;
typedef unsigned char u8;
#define ZNS_BASE_BLOCK_SIZE 4096
#define SECTORS_PER_BLOCK 8
#define ZNS_BASE_GC_READAHEAD_BLOCKS 32
#define ZNS_BASE_ZONE_GC_VICTIM 1
#define GFP_KERNEL 0
#define REQ_OP_READ 0
#define min(a,b) ((a) < (b) ? (a) : (b))
#define min_t(t,a,b) min((t)(a), (t)(b))
struct page { u8 data[4096]; };
struct slot { bool valid; };
struct zns_base_zone {
    int state;
    unsigned int nr_blocks;
    sector_t start_sector, write_pointer;
    struct slot slots[64];
};
struct dev { void *bdev; };
struct zns_base_c { int lock; struct dev *dev; };
struct zns_base_gc_read_buffer {
    void *data;
    unsigned int first_slot, blocks;
    u64 read_ios, read_hits;
};
struct bio {
    struct { sector_t bi_sector; } bi_iter;
    struct page *pages[32];
    unsigned int n;
};
static unsigned int queue_limit = 32, reads, singles, last_count;
static int lock_depth, alloc_fail, add_fail, io_error;
static sector_t last_sector;
static void spin_lock(int *p) { (void)p; assert(!lock_depth); lock_depth++; }
static void spin_unlock(int *p) { (void)p; assert(lock_depth == 1); lock_depth--; }
static unsigned int zns_base_max_transfer_blocks(struct zns_base_c *c) {
    (void)c; return queue_limit;
}
static struct bio *bio_alloc(int flags, unsigned int n) {
    (void)flags; assert(!lock_depth && n <= 32);
    return alloc_fail ? NULL : calloc(1, sizeof(struct bio));
}
static void bio_set_dev(struct bio *b, void *d) { (void)b; (void)d; }
static void bio_set_op_attrs(struct bio *b, int op, int flags) {
    (void)b; assert(op == REQ_OP_READ && !flags);
}
static struct page *vmalloc_to_page(void *p) { return p; }
static void *page_address(struct page *p) { return p->data; }
static int bio_add_page(struct bio *b, struct page *p, unsigned int n, int off) {
    assert(n == 4096 && !off);
    if (add_fail && b->n == 1) return 0;
    b->pages[b->n++] = p;
    return n;
}
static void bio_put(struct bio *b) { free(b); }
static int submit_bio_wait(struct bio *b) {
    assert(!lock_depth);
    reads++; last_count = b->n; last_sector = b->bi_iter.bi_sector;
    if (io_error) return io_error;
    for (unsigned int i = 0; i < b->n; i++)
        memset(b->pages[i]->data, (b->bi_iter.bi_sector / 8 + i) & 255, 4096);
    return 0;
}
static int zns_base_submit_page(struct zns_base_c *c, struct page *p,
                               int op, int flags, sector_t sector) {
    (void)c; assert(!lock_depth && op == REQ_OP_READ && !flags);
    singles++;
    memset(p->data, (sector / 8) & 255, 4096);
    return io_error;
}
'''
        cases = r'''
int main(void) {
    struct dev dev = {0};
    struct zns_base_c c = {.dev = &dev};
    struct zns_base_zone z = {.state=1, .nr_blocks=64,
                             .start_sector=80, .write_pointer=80+64*8};
    struct zns_base_gc_read_buffer cache = {0};
    struct page out;
    cache.data = calloc(32, 4096);
    assert(cache.data);
    for (unsigned int i=0; i<64; i++) z.slots[i].valid = true;
    assert(!zns_base_gc_read_page(&c, &z, 0, &out, &cache));
    assert(reads==1 && last_count==32 && last_sector==80);
    assert(out.data[0]==10 && out.data[4095]==10);
    out.data[0]=255; /* Output modification cannot corrupt prefetched bytes. */
    assert(!zns_base_gc_read_page(&c, &z, 0, &out, &cache));
    assert(out.data[0]==10);
    assert(!zns_base_gc_read_page(&c, &z, 31, &out, &cache));
    assert(reads==1 && out.data[0]==41 && cache.read_hits==2);
    queue_limit=4;
    assert(!zns_base_gc_read_page(&c, &z, 32, &out, &cache));
    assert(reads==2 && last_count==4 && out.data[0]==42);
    cache.blocks=0; z.slots[34].valid=false;
    assert(!zns_base_gc_read_page(&c, &z, 32, &out, &cache));
    assert(last_count==2); /* Stop at invalid hole. */
    cache.blocks=0; z.write_pointer=80+33*8;
    assert(!zns_base_gc_read_page(&c, &z, 32, &out, &cache));
    assert(last_count==1); /* Stop at committed write pointer. */
    z.write_pointer=80+64*8;
    cache.blocks=0;
    assert(!zns_base_gc_read_page(&c, &z, 63, &out, &cache));
    assert(last_count==1); /* Never cross victim boundary. */
    cache.blocks=0; z.state=0;
    assert(zns_base_gc_read_page(&c, &z, 0, &out, &cache)==-EIO);
    z.state=1;
    assert(zns_base_gc_read_page(&c, &z, 64, &out, &cache)==-EIO);
    alloc_fail=1;
    assert(!zns_base_gc_read_page(&c, &z, 0, &out, &cache));
    assert(singles==1 && cache.blocks==0);
    alloc_fail=0; add_fail=1;
    assert(!zns_base_gc_read_page(&c, &z, 0, &out, &cache));
    assert(singles==2 && cache.blocks==0);
    add_fail=0; io_error=-EIO;
    assert(zns_base_gc_read_page(&c, &z, 0, &out, &cache)==-EIO);
    assert(cache.blocks==0); /* Never cache failed batch. */
    io_error=0; z.slots[0].valid=false;
    assert(!zns_base_gc_read_page(&c, &z, 0, &out, &cache));
    assert(singles==3); /* Concurrent invalidation falls back safely. */
    free(cache.data); cache.data=NULL;
    assert(!zns_base_gc_read_page(&c, &z, 1, &out, &cache));
    assert(singles==4 && out.data[0]==11); /* Disabled/allocation failure. */
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="zns-gc-read-test-") as folder:
            source = pathlib.Path(folder) / "gc-read.c"
            binary = pathlib.Path(folder) / "gc-read-test"
            source.write_text(prelude + SOURCE[start:end] + cases)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_mapping_and_reset_guards_retained(self):
        start = SOURCE.index("static int zns_base_gc_move_block(",
                             SOURCE.index("static int zns_base_gc_read_page("))
        end = SOURCE.index("\n}\n", start) + 3
        move = SOURCE[start:end]
        self.assertLess(move.index("zns_base_gc_lookup_validated(c"), move.index("zns_base_gc_read_page(c"))
        self.assertLess(move.index("zns_base_gc_read_page(c"), move.index("zns_base_wal_stage_gc(c"))
        self.assertIn("zns_base_gc_verify_reset_safe(c, victim)", SOURCE)
        publish = SOURCE[SOURCE.rindex("static int zns_base_wal_publish_gc_locked("):]
        self.assertIn("c->mapping.latest_seq[commit->logical_block]", publish)
        self.assertIn("commit->expected_seq", publish)
        self.assertIn("read_buffer.blocks = 0;", SOURCE)
        self.assertIn("vfree(read_buffer.data);", SOURCE)


if __name__ == "__main__":
    unittest.main()
