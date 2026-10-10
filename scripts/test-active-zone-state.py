#!/usr/bin/env python3
"""Test production foreground zone selection without touching devices."""
import pathlib
import subprocess
import tempfile
import unittest

SOURCE = (pathlib.Path(__file__).resolve().parents[1] /
          "src/dm-zns-base.c").read_text()


def function(name):
    start = SOURCE.rindex("static int " + name + "(")
    return SOURCE[start:SOURCE.index("\n}\n", start) + 3]


class ActiveZoneTests(unittest.TestCase):
    def test_state_transitions(self):
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
typedef uint64_t sector_t;
#define SECTORS_PER_BLOCK 8
#define ZNS_BASE_METADATA_ZONES 0
#define ZNS_BASE_NO_ZONE UINT32_MAX
enum { ZNS_BASE_ZONE_FREE, ZNS_BASE_ZONE_ACTIVE, ZNS_BASE_ZONE_FULL,
       ZNS_BASE_ZONE_GC_VICTIM, ZNS_BASE_ZONE_GC_DEST };
#define ZNS_BASE_ZONE_DATA 1
struct zns_base_zone {
    int state, role;
    sector_t start_sector, capacity_sectors, write_pointer;
};
struct zns_base_c {
    struct { struct zns_base_zone zones[4];
             unsigned int nr_zones, active_zone_idx, gc_dest_zone_idx; } zone_state;
    unsigned int reserve;
    int data_write_error;
    bool foreground_zone_grant;
};
static unsigned int zns_base_count_free_zones(struct zns_base_c *c) {
    unsigned int n = 0;
    for (unsigned int i = 0; i < c->zone_state.nr_zones; i++)
        n += c->zone_state.zones[i].state == ZNS_BASE_ZONE_FREE;
    return n;
}
static unsigned int zns_base_foreground_reserve_locked(struct zns_base_c *c) {
    if (c->foreground_zone_grant)
        return 0;
    return c->reserve;
}
'''
        cases = r'''
int main(void) {
    struct zns_base_c c = {0};
    sector_t pba = 999;
    c.zone_state.nr_zones = 4;
    c.zone_state.gc_dest_zone_idx = ZNS_BASE_NO_ZONE;
    c.reserve = 1;
    for (int i = 0; i < 4; i++) {
        c.zone_state.zones[i].role = ZNS_BASE_ZONE_DATA;
        c.zone_state.zones[i].capacity_sectors = 16;
        c.zone_state.zones[i].state = ZNS_BASE_ZONE_FULL;
    }
    /* Stale index must preserve FREE/GC ownership even on repeated retries. */
    int states[] = { ZNS_BASE_ZONE_FREE, ZNS_BASE_ZONE_GC_VICTIM,
                     ZNS_BASE_ZONE_GC_DEST, ZNS_BASE_ZONE_FULL };
    for (unsigned int s = 0; s < 4; s++) {
        c.zone_state.active_zone_idx = 0;
        c.zone_state.zones[0].state = states[s];
        for (int retry = 0; retry < 10; retry++) {
            assert(zns_base_allocate_block(&c, &pba) == -EAGAIN);
            assert(c.zone_state.zones[0].state == states[s]);
            assert(c.zone_state.zones[0].write_pointer == 0);
            assert(pba == 999);
        }
    }
    /* Reset stale zone plus another free zone: reuse via reserve admission. */
    c.zone_state.zones[0].state = ZNS_BASE_ZONE_FREE;
    c.zone_state.zones[1].state = ZNS_BASE_ZONE_FREE;
    assert(zns_base_allocate_block(&c, &pba) == 0 && pba == 0);
    assert(c.zone_state.zones[0].state == ZNS_BASE_ZONE_ACTIVE);
    assert(zns_base_allocate_block(&c, &pba) == 0 && pba == 8);
    assert(c.zone_state.zones[0].state == ZNS_BASE_ZONE_FULL);
    assert(zns_base_allocate_block(&c, &pba) == -EAGAIN);
    /* A persistent reset grant promotes GC_DEST and preserves the FREE zone. */
    c.zone_state.zones[1].state = ZNS_BASE_ZONE_GC_DEST;
    c.zone_state.zones[1].write_pointer = 8;
    c.zone_state.zones[2].state = ZNS_BASE_ZONE_FREE;
    c.zone_state.gc_dest_zone_idx = 1;
    c.foreground_zone_grant = true;
    assert(zns_base_allocate_block(&c, &pba) == 0);
    assert(!c.foreground_zone_grant);
    assert(c.zone_state.gc_dest_zone_idx == ZNS_BASE_NO_ZONE);
    assert(c.zone_state.active_zone_idx == 1);
    assert(c.zone_state.zones[2].state == ZNS_BASE_ZONE_FREE);
    c.zone_state.zones[1].write_pointer = 16;
    assert(zns_base_ensure_active_zone(&c) == -EAGAIN);
    /* GC-owned stale zone remains untouched when another zone is selected. */
    c.zone_state.zones[0].state = ZNS_BASE_ZONE_GC_VICTIM;
    c.zone_state.zones[1].state = ZNS_BASE_ZONE_FREE;
    c.zone_state.zones[1].write_pointer = 0;
    c.zone_state.zones[2].state = ZNS_BASE_ZONE_FREE;
    assert(zns_base_allocate_block(&c, &pba) == 0);
    assert(c.zone_state.active_zone_idx == 1);
    assert(c.zone_state.zones[0].state == ZNS_BASE_ZONE_GC_VICTIM);
    /* Exhausted ACTIVE transitions to FULL; non-DATA is rejected. */
    c.zone_state.zones[1].write_pointer = 16;
    assert(zns_base_ensure_active_zone(&c) == -EAGAIN);
    assert(c.zone_state.zones[1].state == ZNS_BASE_ZONE_FULL);
    c.zone_state.zones[1].role = 0;
    assert(zns_base_ensure_active_zone(&c) == -EIO);
    return 0;
}
'''
        code = prelude + "\n".join(function(n) for n in (
            "zns_base_activate_next_zone", "zns_base_ensure_active_zone",
            "zns_base_allocate_block")) + cases
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "test.c"
            exe = pathlib.Path(tmp) / "test"
            src.write_text(code)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(src), "-o", str(exe)],
                           check=True)
            subprocess.run([str(exe)], check=True)

    def test_batch_uses_same_guard_under_lock(self):
        batch = function("zns_base_write_full_blocks")
        selection = batch[batch.index("retry_zone:"):batch.index("if (ret == -EAGAIN)")]
        self.assertIn("spin_lock(&c->lock);\n\tret = zns_base_ensure_active_zone(c);", selection)
        self.assertNotIn("zone->state =", selection)


if __name__ == "__main__":
    unittest.main()
