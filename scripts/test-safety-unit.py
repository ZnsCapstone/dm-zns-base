#!/usr/bin/env python3
"""Non-destructive regression tests: compile actual driver helpers with fake kernel APIs.

This tests decisions/error handling, NOT kernel concurrency, DMA, WAL replay or FEMU.
No module is loaded and no block device is opened.
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/dm-zns-base-main.c").read_text()


def function(name):
    match = re.search(r"^static [^;{}]*\b" + name + r"\([^;{}]*\)\n\{", SOURCE, re.M)
    if not match:
        raise AssertionError(f"missing function: {name}")
    start = match.start()
    end = match.end()
    depth = 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


class SafetyTests(unittest.TestCase):
    def test_actual_c_helpers(self):
        names = ["mapping_get", "release_frozen_memtable", "gc_live_hash",
                 "gc_live_map_find", "gc_live_map_resize", "gc_live_map_update",
                 "gc_refresh_live_map", "gc_workspace_required",
                 "zone_pool_acquire_free", "zone_pool_alloc", "gc_commit_relocation",
                 "gc_count_free_zones", "foreground_wal_allowed", "sstable_read_finish",
                 "gc_memtable_references_zone"]
        prelude = (ROOT / "scripts/safety-unit-stubs.h").read_text()
        cases = (ROOT / "scripts/safety-unit-cases.c").read_text()
        with tempfile.TemporaryDirectory(prefix="zns-safety-unit-") as folder:
            code = pathlib.Path(folder) / "test.c"
            binary = pathlib.Path(folder) / "test"
            code.write_text(prelude + "\n" + "\n".join(map(function, names)) + "\n" + cases)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(code), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_gc_uses_guards_before_reset(self):
        reclaim = function("gc_reclaim_one_victim")
        self.assertIn("gc_refresh_live_map(c, &live_map)", reclaim)
        self.assertIn("cycle->latest = live_map", reclaim)
        self.assertIn("!gc_memtable_references_zone(c, vstart, vend)", reclaim)
        self.assertLess(reclaim.index("reset blocked by live memtable"),
                        reclaim.index("if (zone_reset_hw"))
        self.assertIn("gc_workspace_required(c, live_sectors)", reclaim)
        self.assertIn("ret = gc_commit_relocation", function("gc_relocate_batch"))

    def test_no_foreground_reserve_borrow(self):
        retry = function("zone_pool_alloc_with_gc_retry")
        self.assertNotIn("borrowed_reserve", retry)
        self.assertNotRegex(retry, r"zone_pool_alloc\([^;]*true\)")
        self.assertIn("gc_reserved_zones < 2", function("zns_base_ctr"))

    def test_flush_visibility_and_read_restart_are_wired(self):
        schedule = function("schedule_memtable_flush_locked")
        self.assertIn("c->wal_ckpt_inflight", schedule)
        self.assertIn("c->frozen_memtable = flushed_memtable", schedule)
        self.assertIn("!c->checkpoint_failed", function("flush_chain_end"))
        self.assertIn("c->read_view_epoch++", function("sstable_register"))
        finish = function("sstable_read_finish")
        self.assertIn("mapping_get(c, rctx->lba, &cur)", finish)
        self.assertIn("rctx->view_epoch != c->read_view_epoch", finish)
        self.assertIn("sstable_read_next_candidate(rctx)", finish)
        self.assertNotIn("skiplist_destroy(ctx->old_memtable)", SOURCE)
        self.assertNotIn("skiplist_destroy(old_memtable)", SOURCE)


if __name__ == "__main__":
    unittest.main()
