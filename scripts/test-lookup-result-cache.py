#!/usr/bin/env python3
"""Compile production SSTable-result cache helpers; no device access."""
import pathlib
import subprocess
import tempfile
import unittest

SOURCE = (pathlib.Path(__file__).resolve().parents[1] / "src/dm-zns-base.c").read_text()


class ResultCacheTests(unittest.TestCase):
    def test_helpers(self):
        start = SOURCE.index("static bool zns_base_result_cache_get(")
        end = SOURCE.index("/* Take a lockless catalog snapshot", start)
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint64_t u64;
struct mapping_entry { size_t logical_block; u64 physical_sector, seq; };
struct zns_base_lookup_result {
    u64 epoch; size_t logical_block; struct mapping_entry entry;
    int result; bool valid;
};
struct zns_base_c { struct {
    struct zns_base_lookup_result *result_cache;
    int lookup_cache_lock;
    long result_cache_hits, result_cache_misses;
} metadata; };
#define ZNS_BASE_RESULT_CACHE_BITS 0
#define hash_64(key,bits) 0 /* Deliberate collisions */
static void spin_lock(int *lock) { assert(!*lock); *lock = 1; }
static void spin_unlock(int *lock) { assert(*lock); *lock = 0; }
static void atomic64_inc(long *n) { ++*n; }
'''
        cases = r'''
int main(void) {
    struct zns_base_lookup_result slot = {0};
    struct zns_base_c c = {0};
    struct mapping_entry old = {4, 80, 10}, newer = {4, 160, 20}, out = {0};
    int ret = 99;
    assert(!zns_base_result_cache_get(&c, 1, 4, &out, &ret));
    zns_base_result_cache_put(&c, 1, 4, &old, 0); /* allocation fallback */
    c.metadata.result_cache = &slot;
    assert(!zns_base_result_cache_get(&c, 1, 4, &out, &ret));
    zns_base_result_cache_put(&c, 1, 4, &old, 0);
    assert(zns_base_result_cache_get(&c, 1, 4, &out, &ret));
    assert(!ret && out.seq == 10 && out.physical_sector == 80);
    assert(!zns_base_result_cache_get(&c, 2, 4, &out, &ret));
    assert(!zns_base_result_cache_get(&c, 1, 5, &out, &ret));
    zns_base_result_cache_put(&c, 2, 4, &newer, 0);
    zns_base_result_cache_put(&c, 1, 4, &old, 0); /* delayed old reader */
    assert(!zns_base_result_cache_get(&c, 2, 4, &out, &ret));
    zns_base_result_cache_put(&c, 2, 4, &newer, 0);
    zns_base_result_cache_put(&c, 2, 5, NULL, -EIO); /* errors not cached */
    assert(zns_base_result_cache_get(&c, 2, 4, &out, &ret) && out.seq == 20);
    zns_base_result_cache_put(&c, 2, 5, NULL, -ENOENT);
    assert(zns_base_result_cache_get(&c, 2, 5, &out, &ret) && ret == -ENOENT);
    assert(!zns_base_result_cache_get(&c, 3, 5, &out, &ret));
    /* Tombstones must remain positive SSTable answers for upper lookup. */
    newer.physical_sector = UINT64_MAX;
    zns_base_result_cache_put(&c, 3, 4, &newer, 0);
    assert(zns_base_result_cache_get(&c, 3, 4, &out, &ret));
    assert(!ret && out.physical_sector == UINT64_MAX);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "test.c"
            exe = pathlib.Path(tmp) / "test"
            src.write_text(prelude + SOURCE[start:end] + cases)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_latest_mapping_checks_are_retained(self):
        start = SOURCE.rindex("static int mapping_lookup_latest(")
        lookup = SOURCE[start:SOURCE.index("\n}\n", start)]
        self.assertLess(lookup.index("mapping_lookup_ram_locked"), lookup.index("zns_base_sstable_lookup"))
        start = SOURCE.rindex("static int zns_base_wal_publish_gc_locked(")
        publish = SOURCE[start:SOURCE.index("\n}\n", start)]
        self.assertIn("c->mapping.latest_seq[commit->logical_block]", publish)
        self.assertIn("commit->expected_seq", publish)
        self.assertNotIn("mapping_lookup(c, commit->logical_block", publish)
        self.assertEqual(SOURCE.count("c->metadata.lookup_epoch++;"), 2)


if __name__ == "__main__":
    unittest.main()
