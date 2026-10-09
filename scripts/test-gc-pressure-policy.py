#!/usr/bin/env python3
"""Source-level regression checks for bounded GC under foreground pressure."""
import pathlib
import unittest

SOURCE = (pathlib.Path(__file__).resolve().parents[1] / 'src/dm-zns-base.c').read_text()


def function_body(signature: str) -> str:
    start = SOURCE.rindex(signature)
    brace = SOURCE.index('{', start)
    depth = 0
    for index in range(brace, len(SOURCE)):
        if SOURCE[index] == '{':
            depth += 1
        elif SOURCE[index] == '}':
            depth -= 1
            if depth == 0:
                return SOURCE[start:index + 1]
    raise AssertionError(f'unclosed function: {signature}')


class GCPressurePolicyTests(unittest.TestCase):
    def test_clean_zone_cooldown_is_cross_run_and_invalidated_early(self):
        self.assertIn('static unsigned int gc_clean_zone_cooldown_ms = 30000;', SOURCE)
        select = function_body('static int zns_base_select_victim(')
        self.assertIn('time_before(jiffies, zone->gc_skip_until)', select)
        self.assertIn('!c->foreground_waiters', select)
        validate = function_body('static void zns_base_gc_work(')
        self.assertIn('msecs_to_jiffies(gc_clean_zone_cooldown_ms)', validate)
        invalidate = function_body('static bool zns_base_invalidate_entry_slot_locked(')
        self.assertIn('zone->gc_skip_until = 0;', invalidate)

    def test_low_reclaim_scans_for_best_emergency_fallback(self):
        worker = function_body('static void zns_base_gc_work(')
        self.assertIn('gc_min_reclaim_percent', worker)
        self.assertIn('fallback_reclaimable', worker)
        self.assertIn('fallback_victim->gc_skip_run = 0;', worker)
        self.assertIn('foreground_waiting', worker)

    def test_waiter_can_take_first_reset_zone_and_gc_yields(self):
        reserve = function_body('static unsigned int zns_base_foreground_reserve_locked(')
        self.assertIn('if (c->foreground_waiters)', reserve)
        self.assertIn('return 0;', reserve)
        wait = function_body('static int zns_base_wait_for_gc_space(')
        self.assertIn('c->foreground_waiters++;', wait)
        self.assertIn('c->foreground_waiters--;', wait)
        worker = function_body('static void zns_base_gc_work(')
        self.assertIn('c->foreground_waiters &&', worker)
        self.assertIn('c->stopping || c->quiescing ||', worker)

    def test_gc_publish_uses_recovered_latest_sequence_index(self):
        update = function_body('static int mapping_update(')
        self.assertIn('c->mapping.latest_seq[logical_block] = seq;', update)
        recover = function_body('static int zns_base_manifest_recover(')
        self.assertIn('c->mapping.latest_seq[i] = snapshot[i].seq;', recover)
        publish = function_body('static int zns_base_wal_publish_gc_locked(')
        self.assertIn('c->mapping.latest_seq[commit->logical_block]', publish)
        self.assertNotIn('mapping_lookup(', publish)


if __name__ == '__main__':
    unittest.main()
