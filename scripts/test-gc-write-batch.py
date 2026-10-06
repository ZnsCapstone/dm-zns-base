#!/usr/bin/env python3
"""Compile the production GC destination commit helper and check wiring."""
import pathlib
import subprocess
import tempfile
import unittest

SOURCE = (pathlib.Path(__file__).resolve().parents[1] / 'src/dm-zns-base.c').read_text()


class GCWriteBatchTests(unittest.TestCase):
    def test_atomic_destination_commit(self):
        start = SOURCE.index('static int zns_base_commit_gc_blocks(')
        end = SOURCE.index('/* Pack up to one readahead window', start)
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint64_t u64; typedef uint64_t sector_t;
#define SECTORS_PER_BLOCK 8
#define ZNS_BASE_NO_ZONE ((unsigned)-1)
enum { ZNS_BASE_ZONE_GC_DEST, ZNS_BASE_ZONE_FULL };
struct mapping_entry { size_t logical_block; sector_t physical_sector; u64 seq; };
struct zns_base_zone_slot { size_t logical_block; u64 seq; bool valid, pending; };
struct zns_base_zone { int state; sector_t start_sector, capacity_sectors, write_pointer;
    unsigned pending_blocks; struct zns_base_zone_slot slots[8]; };
struct zns_base_gc_move_item { struct mapping_entry expected_entry;
    struct zns_base_zone *new_zone; sector_t old_physical_sector, new_physical_sector;
    size_t logical_block; u64 victim_seq; unsigned victim_slot, new_slot;
    bool mapping_slot_reserved, pending_reserved; };
struct zns_base_c { int lock; struct { unsigned gc_dest_zone_idx; } zone_state; };
static void spin_lock(int *l) { assert(!*l); *l = 1; }
static void spin_unlock(int *l) { assert(*l); *l = 0; }
static struct zns_base_zone *test_zone;
static int zns_base_get_zone_slot(struct zns_base_c *c, sector_t p,
    struct zns_base_zone **zone, unsigned *slot) {
    (void)c; if (p < test_zone->start_sector) return -EIO;
    *zone = test_zone; *slot = (p - test_zone->start_sector) / 8;
    return *slot < 8 ? 0 : -EIO;
}
static int zns_base_reserve_pending_slot_locked(struct zns_base_zone *z,
    unsigned slot, size_t lba) {
    if (z->slots[slot].valid || z->slots[slot].pending) return -EIO;
    z->slots[slot].logical_block = lba; z->slots[slot].pending = true;
    z->pending_blocks++; return 0;
}
'''
        cases = r'''
int main(void) {
    struct zns_base_c c = {0};
    struct zns_base_zone z = {.state=ZNS_BASE_ZONE_GC_DEST,
        .start_sector=80, .capacity_sectors=64, .write_pointer=80};
    struct zns_base_gc_move_item items[3] = {
        {.logical_block=1,.new_physical_sector=80},
        {.logical_block=2,.new_physical_sector=88},
        {.logical_block=3,.new_physical_sector=96}};
    test_zone = &z;
    assert(!zns_base_commit_gc_blocks(&c, &z, 80, items, 3));
    assert(z.write_pointer == 104 && z.pending_blocks == 3);
    assert(items[0].pending_reserved && items[2].new_slot == 2);
    assert(zns_base_commit_gc_blocks(&c, &z, 80, items, 1) == -EIO);
    z.write_pointer = 104; z.slots[3].valid = true;
    items[0].new_physical_sector = 104;
    assert(zns_base_commit_gc_blocks(&c, &z, 104, items, 1) == -EIO);
    assert(z.write_pointer == 104 && z.pending_blocks == 3);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.c'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(prelude + SOURCE[start:end] + cases)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_batch_order_and_fallback(self):
        start = SOURCE.rindex('static int zns_base_gc_move_blocks(')
        body = SOURCE[start:SOURCE.index('\n}\n', start) + 3]
        self.assertLess(body.index('zns_base_submit_buffer_blocks'),
                        body.index('zns_base_commit_gc_blocks'))
        self.assertLess(body.index('zns_base_commit_gc_blocks'),
                        body.index('zns_base_wal_stage_gc'))
        worker_start = SOURCE.rindex('static void zns_base_gc_work(')
        worker = SOURCE[worker_start:
                        SOURCE.index('static int zns_base_select_victim(', worker_start)]
        self.assertIn('if (read_buffer.write_data)', worker)
        self.assertIn('zns_base_gc_move_block(c', worker)
        self.assertIn('read_buffer.move_items = kcalloc', worker)
        # `current` is a Linux kernel macro for get_current().
        self.assertNotIn('struct mapping_entry current;', body)
        self.assertNotIn('items[ZNS_BASE_GC_READAHEAD_BLOCKS]', body)


if __name__ == '__main__':
    unittest.main()
