#!/usr/bin/env python3
"""Check GC bulk validation's safety ordering and point-lookup fallback."""
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


class GCBulkValidationTests(unittest.TestCase):
    def test_bulk_is_default_and_point_path_is_retained(self):
        self.assertIn('static bool gc_bulk_validation = true;', SOURCE)
        self.assertIn('module_param(gc_bulk_validation, bool, 0444);', SOURCE)
        wrapper = function_body('static int zns_base_gc_validate_victim(')
        self.assertIn('zns_base_gc_validate_victim_bulk(', wrapper)
        self.assertIn('if (ret != -ENOMEM)', wrapper)
        self.assertIn('zns_base_gc_validate_victim_point(', wrapper)

    def test_catalog_is_pinned_and_ram_brackets_scan(self):
        body = function_body('static int zns_base_gc_build_bulk_snapshot(')
        first_ram = body.index('zns_base_snapshot_ram(c, snapshot);')
        srcu_lock = body.index('srcu_read_lock(')
        scan = body.index('zns_base_sstable_apply_to_snapshot_locked(')
        srcu_unlock = body.index('srcu_read_unlock(')
        second_ram = body.rindex('zns_base_snapshot_ram(c, snapshot);')
        self.assertLess(first_ram, srcu_lock)
        self.assertLess(srcu_lock, scan)
        self.assertLess(scan, srcu_unlock)
        self.assertLess(srcu_unlock, second_ram)
        self.assertIn('read_seqcount_begin(&c->metadata.catalog_seq)', body)
        self.assertIn('read_seqcount_retry(&c->metadata.catalog_seq', body)

    def test_sstable_snapshot_scan_batches_reads(self):
        body = function_body('static int zns_base_sstable_apply_to_snapshot_locked(')
        self.assertIn('ZNS_BASE_GC_READAHEAD_BLOCKS * ZNS_BASE_BLOCK_SIZE', body)
        self.assertIn('zns_base_submit_read_buffer_blocks(', body)
        # The single-block helper remains only for the SSTable header.
        self.assertEqual(body.count('zns_base_metadata_read_block('), 1)

    def test_bulk_loop_has_no_point_lookup_and_keeps_exact_match(self):
        body = function_body('static int zns_base_gc_validate_victim_bulk(')
        self.assertNotIn('mapping_lookup(', body)
        self.assertIn('current_entry.physical_sector == physical_sector', body)
        self.assertIn('current_entry.seq == seq', body)
        self.assertIn('current_entry.physical_sector == ZNS_BASE_DISCARDED_PBA', body)
        self.assertIn('victim->valid_blocks--;', body)

    def test_conditional_wal_publish_and_reset_guard_remain(self):
        publish = function_body('static int zns_base_wal_publish_gc_locked(')
        self.assertIn('mapping_lookup(c, commit->logical_block', publish)
        self.assertIn('current_entry.seq != commit->expected_seq', publish)
        reset = function_body('static int zns_base_gc_verify_reset_safe(')
        self.assertIn('pending_blocks', reset)
        self.assertIn('valid_blocks', reset)


if __name__ == '__main__':
    unittest.main()
