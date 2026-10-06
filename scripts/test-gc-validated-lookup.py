#!/usr/bin/env python3
"""Exercise the production GC reuse helper with deterministic race injection."""
import pathlib
import subprocess
import tempfile
import unittest

SOURCE = (pathlib.Path(__file__).resolve().parents[1] / 'src/dm-zns-base.c').read_text()


class ValidatedLookupTests(unittest.TestCase):
    def test_helper(self):
        start = SOURCE.index('static int zns_base_gc_lookup_validated(')
        end = SOURCE.index('\n}\n', start) + 3
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint64_t u64;
typedef uint64_t sector_t;
#define READ_ONCE(x) (x)
struct mapping_entry { size_t logical_block; sector_t physical_sector; u64 seq; };
struct zns_base_c { int lock; };
static int ram_ret = -ENOENT, fallback_calls;
static struct mapping_entry ram;
static void spin_lock(int *l) { assert(!*l); *l = 1; }
static void spin_unlock(int *l) { assert(*l); *l = 0; }
static int mapping_lookup_ram_locked(struct zns_base_c *c, size_t lba, struct mapping_entry *out) {
    (void)lba; assert(c->lock); *out = ram;
    return ram_ret;
}
static int mapping_lookup(struct zns_base_c *c, size_t lba, struct mapping_entry *out) {
    (void)c; (void)lba; (void)out; fallback_calls++; return -EIO;
}
'''
        cases = r'''
int main(void) {
    struct zns_base_c c = {0};
    struct mapping_entry out;
    bool hit;
    assert(!zns_base_gc_lookup_validated(&c, true, 9, 80, 10, &out, &hit));
    assert(hit && out.logical_block == 9 && out.physical_sector == 80 && out.seq == 10);
    assert(zns_base_gc_lookup_validated(&c, false, 9, 80, 10, &out, &hit) == -EIO && !hit);
    /* Catalog publication no longer invalidates exact validation evidence. */
    assert(!zns_base_gc_lookup_validated(&c, true, 9, 80, 10, &out, &hit));
    assert(hit && out.physical_sector == 80 && out.seq == 10);
    ram_ret = 0; ram = (struct mapping_entry){9, 160, 11};
    assert(!zns_base_gc_lookup_validated(&c, true, 9, 80, 10, &out, &hit));
    assert(!hit && out.physical_sector == 160 && out.seq == 11);
    ram.physical_sector = ZNS_BASE_DISCARDED_PBA;
    assert(zns_base_gc_lookup_validated(&c, true, 9, 80, 10, &out, &hit) == -ENOENT && !hit);
    ram_ret = -EIO;
    assert(zns_base_gc_lookup_validated(&c, true, 9, 80, 10, &out, &hit) == -EIO && !hit);
    assert(fallback_calls == 1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp) / 'test.c'
            exe = pathlib.Path(tmp) / 'test'
            # Use the production definition, not a test-only alias that could
            # hide an undefined identifier in the kernel build.
            definition = next(line for line in SOURCE.splitlines()
                              if line.startswith('#define ZNS_BASE_DISCARDED_PBA '))
            self.assertLess(SOURCE.index(definition), start)
            source.write_text(harness + '\n' + definition + '\n' + SOURCE[start:end] + cases)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_reuse_is_independent_and_final_publish_stays_conditional(self):
        self.assertIn('module_param(gc_validated_reuse, bool, 0444);', SOURCE)
        self.assertIn('if (gc_validated_reuse && victim->nr_blocks', SOURCE)
        start = SOURCE.rindex('static int zns_base_wal_publish_gc_locked(')
        publish = SOURCE[start:SOURCE.index('\n}\n', start)]
        self.assertIn('mapping_lookup(c, commit->logical_block', publish)
        self.assertIn('current_entry.seq != commit->expected_seq', publish)


if __name__ == '__main__':
    unittest.main()
