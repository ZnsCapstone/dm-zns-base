#!/usr/bin/env python3
"""Compile the production cache helper with fake I/O. No device access.

Tests cache decisions, not real kernel concurrency, SRCU, or crash recovery.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/dm-zns-base.c").read_text()


class LookupCacheTests(unittest.TestCase):
    def test_production_helper(self):
        start = SOURCE.index("static int zns_base_lookup_read_block(")
        end = SOURCE.index("\n}\n", start) + 3
        prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint64_t u64;
typedef uint64_t sector_t;
#define ZNS_BASE_BLOCK_SIZE 4096
#define SECTORS_PER_BLOCK 8
#define ZNS_BASE_LOOKUP_CACHE_BITS 0
/* Force collisions to exercise replacement. */
#define hash_64(value, bits) 0
static int lock_depth, reads, fail_read;
static unsigned char disk_value;
#define spin_lock(lock) do { assert(!lock_depth); lock_depth++; } while (0)
#define spin_unlock(lock) do { assert(lock_depth == 1); lock_depth--; } while (0)
#define atomic64_inc(value) (++*(value))
struct zns_base_lookup_cache_entry {
    u64 epoch;
    sector_t sector;
    bool valid;
    unsigned char data[4096];
};
struct zns_base_c {
    struct {
        struct zns_base_lookup_cache_entry *lookup_cache;
        int lookup_cache_lock;
        unsigned long lookup_cache_hits, lookup_cache_misses;
    } metadata;
};
static int zns_base_metadata_read_block(struct zns_base_c *c,
                                      sector_t sector, void *buffer) {
    (void)c; (void)sector;
    assert(lock_depth == 0);
    reads++;
    if (fail_read) return -5;
    memset(buffer, disk_value, 4096);
    return 0;
}
'''
        cases = r'''
int main(void) {
    struct zns_base_c c = {0};
    struct zns_base_lookup_cache_entry cache = {0};
    unsigned char buffer[4096];
    c.metadata.lookup_cache = &cache;
    disk_value = 17;
    assert(!zns_base_lookup_read_block(&c, 1, 8, buffer));
    assert(reads == 1 && buffer[0] == 17);
    buffer[0] = 99; /* Caller mutating header must not mutate cache. */
    assert(!zns_base_lookup_read_block(&c, 1, 8, buffer));
    assert(reads == 1 && buffer[0] == 17 && buffer[4095] == 17);
    disk_value = 23; /* Same sector reused after catalog publication. */
    assert(!zns_base_lookup_read_block(&c, 2, 8, buffer));
    assert(reads == 2 && buffer[0] == 23);
    disk_value = 31;
    assert(!zns_base_lookup_read_block(&c, 2, 16, buffer));
    assert(reads == 3 && buffer[0] == 31); /* Hash collision. */
    fail_read = 1;
    assert(zns_base_lookup_read_block(&c, 3, 16, buffer) == -5);
    assert(cache.epoch == 2); /* Errors must not be cached. */
    fail_read = 0;
    disk_value = 41;
    assert(!zns_base_lookup_read_block(&c, 3, 16, buffer));
    assert(buffer[0] == 41);
    /* Late old-epoch fill cannot hit for a newer epoch. */
    disk_value = 31;
    assert(!zns_base_lookup_read_block(&c, 2, 16, buffer));
    disk_value = 41;
    assert(!zns_base_lookup_read_block(&c, 3, 16, buffer));
    assert(buffer[0] == 41 && reads == 7);
    c.metadata.lookup_cache = 0; /* Allocation failure fallback. */
    assert(!zns_base_lookup_read_block(&c, 3, 16, buffer));
    assert(reads == 8);
    assert(c.metadata.lookup_cache_hits == 1);
    assert(c.metadata.lookup_cache_misses == 8);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="zns-cache-test-") as folder:
            source = pathlib.Path(folder) / "cache.c"
            binary = pathlib.Path(folder) / "cache-test"
            source.write_text(prelude + SOURCE[start:end] + cases)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(source), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True)

    def test_catalog_epoch_wiring(self):
        self.assertEqual(SOURCE.count("write_seqcount_begin(&c->metadata.catalog_seq);"), 2)
        self.assertEqual(SOURCE.count(
            "write_seqcount_begin(&c->metadata.catalog_seq);\n\tc->metadata.lookup_epoch++;"), 2)
        start = SOURCE.index("static int zns_base_sstable_lookup(",
                             SOURCE.index("static int zns_base_lookup_read_block("))
        lookup = SOURCE[start:SOURCE.index("\n}\n", start)]
        self.assertEqual(lookup.count("zns_base_lookup_read_block(c, lookup_epoch,"), 3)
        self.assertNotIn("zns_base_metadata_read_block(", lookup)
        self.assertLess(lookup.index("srcu_read_lock("), lookup.index("lookup_epoch ="))
        self.assertGreater(lookup.index("srcu_read_unlock("),
                           lookup.rindex("zns_base_lookup_read_block("))


if __name__ == "__main__":
    unittest.main()
