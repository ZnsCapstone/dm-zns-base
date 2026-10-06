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
struct zns_base_c { int lock; struct { unsigned catalog_seq; u64 lookup_epoch; } metadata; };
static int ram_ret = -ENOENT, fallback_calls, race;
static struct mapping_entry ram;
static unsigned read_seqcount_begin(unsigned *v) { return *v; }
static bool read_seqcount_retry(unsigned *v, unsigned old) { return *v != old; }
static void spin_lock(int *l) { assert(!*l); *l = 1; }
static void spin_unlock(int *l) { assert(*l); *l = 0; }
static int mapping_lookup_ram_locked(struct zns_base_c *c, size_t lba, struct mapping_entry *out) {
    (void)lba; assert(c->lock); *out = ram;
    if (race) c->metadata.catalog_seq += 2;
    return ram_ret;
}
static int mapping_lookup(struct zns_base_c *c, size_t lba, struct mapping_entry *out) {
    (void)c; (void)lba; (void)out; fallback_calls++; return -EIO;
}
'''
        cases = r'''
int main(void) {
    struct zns_base_c c = { .metadata = {2, 7} };
    struct mapping_entry out;
    bool hit;
    assert(!zns_base_gc_lookup_validated(&c, 7, 9, 80, 10, &out, &hit));
    assert(hit && out.logical_block == 9 && out.physical_sector == 80 && out.seq == 10);
    assert(zns_base_gc_lookup_validated(&c, 0, 9, 80, 10, &out, &hit) == -EIO && !hit);
    assert(zns_base_gc_lookup_validated(&c, 6, 9, 80, 10, &out, &hit) == -EIO && !hit);
    race = 1;
    assert(zns_base_gc_lookup_validated(&c, 7, 9, 80, 10, &out, &hit) == -EIO && !hit);
    race = 0; ram_ret = 0; ram = (struct mapping_entry){9, 160, 11};
    assert(!zns_base_gc_lookup_validated(&c, 7, 9, 80, 10, &out, &hit));
    assert(!hit && out.physical_sector == 160 && out.seq == 11);
    ram.physical_sector = ZNS_BASE_DISCARDED_PBA;
    assert(zns_base_gc_lookup_validated(&c, 7, 9, 80, 10, &out, &hit) == -ENOENT && !hit);
    ram_ret = -EIO;
    assert(zns_base_gc_lookup_validated(&c, 7, 9, 80, 10, &out, &hit) == -EIO && !hit);
    assert(fallback_calls == 3);
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


if __name__ == '__main__':
    unittest.main()
