#!/usr/bin/env python3
"""Source-level checks for the keper/Kovalt integration controls."""
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


class KeperKovaltIntegrationTests(unittest.TestCase):
    def test_kovalt_threshold_names_are_compatible_aliases(self):
        self.assertIn(
            'module_param_named(flush_threshold, memtable_capacity_entries, uint, 0444);',
            SOURCE)
        self.assertIn(
            'module_param_named(compaction_k, sstable_compaction_threshold, uint, 0444);',
            SOURCE)

    def test_strict_wal_mode_is_opt_in(self):
        self.assertIn('static bool strict_wal_durability;', SOURCE)
        self.assertIn('module_param(strict_wal_durability, bool, 0444);', SOURCE)
        queue = function_body('static int zns_base_queue_bio(')
        self.assertIn('strict_wal_durability ||', queue)
        self.assertIn('REQ_FUA | REQ_PREFLUSH', queue)

    def test_strict_wal_uses_existing_durable_commit_path(self):
        stage = function_body('static int zns_base_wal_stage_foreground(')
        self.assertIn('if (durable)', stage)
        self.assertIn('commit->io = durable ? io : NULL;', stage)
        self.assertIn('zns_base_io_add_pending_commit(c, io);', stage)

    def test_gc_parameter_order_is_validated(self):
        ctr = function_body('static int zns_base_ctr(')
        self.assertIn('!gc_reserved_zones', ctr)
        self.assertIn('gc_low_watermark < gc_reserved_zones', ctr)
        self.assertIn('gc_high_watermark <= gc_low_watermark', ctr)

    def test_wal_record_limit_forces_a_timer_free_group_boundary(self):
        self.assertIn('static unsigned int wal_batch_max_records;', SOURCE)
        self.assertNotIn('wal_batch_delay_ms', SOURCE)
        limit = function_body('static unsigned int zns_base_wal_record_limit(')
        self.assertIn('wal_batch_max_records ?', limit)
        stage = function_body('static int zns_base_wal_stage_commit_locked(')
        self.assertEqual(stage.count('zns_base_wal_record_limit()'), 2)

    def test_foreground_wal_allocation_failure_is_deterministic(self):
        foreground = function_body('static int zns_base_wal_stage_foreground(')
        gc = function_body('static int zns_base_wal_stage_gc(')
        self.assertIn('wal_batch_alloc_failures--', foreground)
        self.assertIn('return -ENOMEM;', foreground)
        self.assertNotIn('wal_batch_alloc_failures', gc)

    def test_gc_diagnostic_budget_is_global_and_optional(self):
        helper = function_body('static bool zns_base_gc_diag_enabled(')
        self.assertIn('if (!gc_diag_budget)', helper)
        self.assertIn('atomic_inc_return(&gc_diag_events)', helper)
        worker = function_body('static void zns_base_gc_work(')
        self.assertIn('zns_base_gc_diag_enabled()', worker)

    def test_optional_cache_metrics_do_not_change_cache_operation(self):
        lookup = function_body('static int zns_base_lookup_read_block(')
        result = function_body('static bool zns_base_result_cache_get(')
        self.assertIn('if (metrics_enabled)', lookup)
        self.assertIn('if (metrics_enabled)', result)

    def test_status_exposes_integration_controls(self):
        status = function_body('static void zns_base_status(')
        for field in ('gc_reserved_zones=%u', 'gc_low_watermark=%u',
                      'gc_high_watermark=%u', 'gc_diag_budget=%u',
                      'metrics_enabled=%u', 'wal_batch_max_records=%u',
                      'wal_batch_alloc_failures=%u',
                      'strict_wal_durability=%u'):
            self.assertIn(field, status)


if __name__ == '__main__':
    unittest.main()
