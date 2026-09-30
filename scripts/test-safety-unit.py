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
    def test_gc_preserves_foreground_active_tail(self):
        worker = function("gc_work_fn")
        self.assertNotRegex(worker,
                            r"active_zone\[ZONE_TAG_USER_DATA\]\s*=\s*ZONE_NONE")

    def test_reserve_boundary_snapshot_is_one_shot_and_outside_lock(self):
        worker = function("gc_work_fn")
        self.assertIn("no_progress == 3", worker)
        snapshot_call = worker.index("gc_log_no_progress_state(c, no_progress)")
        final_unlock = worker.rindex("spin_unlock_irq(&c->lock)", 0, snapshot_call)
        self.assertLess(final_unlock, snapshot_call)
        snapshot = function("gc_log_no_progress_state")
        self.assertIn("active_tail[ZONE_TAG_COUNT]", snapshot)
        self.assertIn("rows[z].dispatch_wp", snapshot)
        self.assertIn("rows[z].invalid_count", snapshot)

    def test_actual_c_helpers(self):
        names = ["mapping_get", "release_frozen_memtable", "gc_live_hash",
                 "gc_live_map_find", "gc_live_map_resize", "gc_live_map_update",
                 "gc_refresh_live_map", "gc_count_free_zones",
                 "gc_wal_sectors_for_data",
                 "gc_wal_tail_needs_roll", "gc_workspace_required",
                 "gc_reclaim_gain_allowed",
                 "gc_seed_wal_required", "gc_seed_borrow_allowed",
                 "gc_wal_rotation_needed",
                 "sstable_flush_can_borrow_reserve",
                 "sstable_compaction_reserve_reclaimable",
                 "sstable_compaction_can_borrow_reserve",
                 "zone_pool_acquire_free", "zone_pool_alloc",
                 "zone_pool_alloc_fresh",
                 "zone_pool_handoff_gc_tail", "zone_pool_zone_is_active",
                 "gc_commit_relocation",
                 "foreground_wal_allowed", "sstable_read_finish",
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

    def test_gc_groups_relocation_wal_before_mapping_publication(self):
        relocate = function("gc_relocate_batch")
        reclaim = function("gc_reclaim_one_victim")
        self.assertIn("GC_RELOC_WAL_PAGES 16", SOURCE)
        self.assertIn("nr_wal_pages = DIV_ROUND_UP", relocate)
        self.assertIn("blkdev_issue_flush(c->dev->bdev)", relocate)
        self.assertLess(relocate.index("blkdev_issue_flush(c->dev->bdev)"),
                        relocate.index("gc_commit_relocation"))
        self.assertIn("WAL_PAGE_MAX_RECORDS * GC_RELOC_WAL_PAGES", reclaim)

    def test_reserve_bootstrap_requires_real_data_gain(self):
        reclaim = function("gc_reclaim_one_victim")
        guard = function("gc_reclaim_gain_allowed")
        self.assertIn("live_sectors >= used_sectors", guard)
        self.assertIn("free_zones <= gc_reserved_zones", guard)
        self.assertIn("gc_reclaim_gain_allowed(used_sectors, live_sectors",
                      reclaim)
        self.assertIn("reserve-boundary bootstrap victim", reclaim)

    def test_gc_preserves_wal_floor_for_next_victim(self):
        reclaim = function("gc_reclaim_one_victim")
        workspace = function("gc_workspace_required")
        roll = function("gc_wal_tail_needs_roll")
        self.assertIn("wal_sectors + c->gc_wal_reserve", workspace)
        self.assertIn("wal_sectors + c->gc_wal_reserve", roll)
        self.assertIn("active_zone[ZONE_TAG_WAL] = ZONE_NONE", reclaim)
        self.assertIn("rolled WAL tail to preserve next-victim floor", reclaim)

    def test_foreground_reserve_borrow_is_workspace_guarded(self):
        retry = function("zone_pool_alloc_with_gc_retry")
        self.assertIn("gc_seed_borrow_allowed(c)", retry)
        self.assertRegex(retry, r"zone_pool_alloc\([^;]*true\)")
        guard = function("gc_seed_borrow_allowed")
        self.assertIn("gc_reserved_zones < 2", guard)
        self.assertIn("wal_tail >= gc_seed_wal_required(c)", guard)
        self.assertIn("c->gc_wal_reserve = gc_wal_sectors_for_data", retry)
        self.assertIn("max(c->gc_wal_budget, c->gc_wal_reserve)",
                      function("foreground_wal_allowed"))
        self.assertIn("gc_reserved_zones < 2", function("zns_base_ctr"))

    def test_reserve_boundary_rotates_wal_before_next_seed(self):
        retry = function("zone_pool_alloc_with_gc_retry")
        needed = function("gc_wal_rotation_needed")
        rotate = function("wal_rotate_work_fn")
        reclaim = function("wal_reclaim_work_fn")
        schedule = function("schedule_memtable_flush_locked")
        self.assertIn("gc_wal_rotation_needed(c)", retry)
        self.assertIn("c->wal_rotation_pending = true", retry)
        self.assertIn("free_zones > gc_reserved_zones", needed)
        self.assertIn("gc_wal_tail_needs_roll(c, c->gc_wal_reserve)", needed)
        self.assertIn("wal_tail < gc_seed_wal_required(c)", needed)
        self.assertIn("if (rotation_credit_used)", retry)
        self.assertGreater(retry.index("c->wal_rotation_credit = false"),
                           retry.index("if (!ret)"))
        self.assertIn("atomic_inc(&c->foreground_writes)", retry)
        self.assertIn("schedule_memtable_flush_locked(c, true)", rotate)
        self.assertIn("zone_pool_acquire_free(c->zp, ZONE_TAG_WAL, true)", rotate)
        self.assertIn("(!force && c->wal_rotation_pending)", schedule)
        self.assertIn("victim == c->wal_rotation_old_zone", reclaim)
        self.assertLess(reclaim.index("zone_pool_mark_free"),
                        reclaim.index("c->wal_rotation_pending = false"))

    def test_gc_requests_wal_rotation_before_workspace_stall(self):
        reclaim = function("gc_reclaim_one_victim")
        request = "gc: requested reserve-boundary WAL generation rotation"
        reject = "gc: insufficient relocation workspace"
        self.assertIn("else if (!rotation_pending && gc_wal_rotation_needed(c))",
                      reclaim)
        self.assertIn("c->wal_rotation_pending = true", reclaim)
        self.assertIn("mod_delayed_work(zns_gc_wq, &c->wal_rotate_work, 0)",
                      reclaim)
        self.assertLess(reclaim.index(request), reclaim.index(reject))

    def test_gc_yields_to_foreground_rotation_without_workspace_error(self):
        reclaim = function("gc_reclaim_one_victim")
        snapshot = "rotation_pending = c->wal_rotation_pending"
        waiting = "else if (rotation_pending)"
        info = "gc: yielding victim %u for already pending WAL generation rotation"
        reject = "gc: insufficient relocation workspace"
        self.assertIn(snapshot, reclaim)
        self.assertIn("else if (!rotation_pending && gc_wal_rotation_needed(c))",
                      reclaim)
        self.assertIn(waiting, reclaim)
        self.assertIn(info, reclaim)
        self.assertLess(reclaim.index(snapshot), reclaim.index(waiting))
        self.assertLess(reclaim.index(info), reclaim.index(reject))

    def test_gc_yields_reclaimed_space_to_waiting_foreground(self):
        worker = function("gc_work_fn")
        self.assertIn("!list_empty(&c->pending_write_bios)", worker)
        self.assertIn("gc: yielding after reclaim for pending foreground I/O", worker)
        self.assertLess(worker.index("foreground_waiting"),
                        worker.index("if (!still_low)"))

    def test_gc_drains_admitted_foreground_before_reserving_victim_wal(self):
        reclaim = function("gc_reclaim_one_victim")
        retry = function("zone_pool_alloc_with_gc_retry")
        done = function("foreground_write_done")
        mapper = function("zns_base_map")
        self.assertIn("c->gc_foreground_pause = true", reclaim)
        self.assertIn("wait_event(c->flush_waitq", reclaim)
        self.assertLess(reclaim.index("c->gc_foreground_pause = true"),
                        reclaim.index("c->gc_wal_budget = victim_wal"))
        self.assertIn("c->gc_foreground_pause = false", reclaim)
        self.assertIn("c->gc_foreground_pause", retry)
        self.assertLess(retry.index("c->gc_foreground_pause"),
                        retry.index("zone_pool_alloc(c->zp"))
        self.assertIn("wake_up_all(&c->flush_waitq)", done)
        self.assertIn("c->wal_rotation_pending || c->gc_foreground_pause", mapper)

    def test_sstable_reserve_borrow_requires_guaranteed_old_zone_reclaim(self):
        compact = function("compaction_work_fn")
        guard = function("sstable_compaction_can_borrow_reserve")
        structural = function("sstable_compaction_reserve_reclaimable")
        flush = function("flush_memtable_async")
        flush_guard = function("sstable_flush_can_borrow_reserve")
        self.assertIn("sstable_compaction_can_borrow_reserve", compact)
        self.assertRegex(compact, r"zone_pool_alloc\([^;]*true\)")
        self.assertIn("c->zp->sstable_live_count[old_zone]", structural)
        self.assertIn("c->zp->dispatch_wp[old_zone]", structural)
        self.assertIn("c->wal_ckpt_inflight", guard)
        self.assertIn("c->gc_active", guard)
        self.assertIn("c->wal_rotation_pending", guard)
        self.assertIn("proactive_rotation = !ret", compact)
        self.assertIn("zone_pool_alloc_fresh", compact)
        self.assertLess(compact.index("sstable_compaction_can_borrow_reserve"),
                        compact.index("&new_phys, &new_zone, false"))
        self.assertIn("(proactive=%u)", compact)
        self.assertIn("flush_workqueue(zns_compaction_wq)", flush)
        self.assertIn("sstable_flush_can_borrow_reserve", flush)
        self.assertIn("c->nr_sstables != compaction_k - 1", flush_guard)
        self.assertIn("c->zp->sstable_live_count[old_zone]", flush_guard)
        self.assertIn("merged_records += c->sstables[i].record_count", flush_guard)
        self.assertIn("flush_sectors + merged_sectors", flush_guard)

    def test_gc_yields_to_pending_sstable_rotation(self):
        compact = function("compaction_work_fn")
        worker = function("gc_work_fn")
        schedule = function("schedule_memtable_flush_locked")
        dtr = function("zns_base_dtr")
        self.assertIn("c->sstable_rotation_pending = true", compact)
        self.assertIn("wait_event(c->flush_waitq", compact)
        self.assertIn("c->sstable_rotation_pending = false", compact)
        self.assertIn("READ_ONCE(c->sstable_rotation_pending)", worker)
        self.assertIn("gc: yielding for reserve-boundary SSTable rotation", worker)
        self.assertIn("gc: stopping at victim boundary", worker)
        self.assertIn("c->sstable_rotation_pending", schedule)
        self.assertIn("wake_up_all(&c->flush_waitq)", dtr)

    def test_foreground_can_take_gc_tail_only_after_worker_finishes(self):
        retry = function("zone_pool_alloc_with_gc_retry")
        self.assertIn("zone_pool_handoff_gc_tail", retry)
        self.assertIn("c->gc_wal_reserve", retry)
        self.assertIn("!c->gc_active", retry)
        handoff = function("zone_pool_handoff_gc_tail")
        self.assertIn("active_zone[ZONE_TAG_GC_DATA] = ZONE_NONE", handoff)
        self.assertIn("active_zone[ZONE_TAG_USER_DATA] = z", handoff)
        self.assertIn("zone_pool_zone_is_active(c->zp, z)",
                      function("gc_select_victim"))

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
