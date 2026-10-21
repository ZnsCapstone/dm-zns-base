#!/usr/bin/env python3
"""실제 조기 완료 C helper 실행 + 영속화 경계 구조 회귀. 장치/성능 검증 아님."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('safety', ROOT/'scripts/test-safety-unit.py')
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
function = safety.function


class WritebackTests(unittest.TestCase):
    def test_actual_buffer_and_completion_helpers(self):
        harness = (ROOT/'scripts/wal-writeback-harness.c').read_text()
        for name in ['wal_try_buffer_write', 'wal_complete_buffered']:
            harness = harness.replace('/* '+name+' */', function(name))
        with tempfile.TemporaryDirectory(prefix='wal-writeback-') as folder:
            path = Path(folder)
            (path/'test.c').write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g', str(path/'test.c'),
                            '-o', str(path/'test')], check=True)
            # 관리형 실행기의 ptrace 하에서는 LSan이 동작하지 않는다.
            # ASan/UBSan은 유지하고 소유권 해제는 harness에서도 계수한다.
            subprocess.run([str(path/'test')], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))

    def test_data_flush_precedes_durable_wal_and_publication(self):
        worker = function('wal_batch_work_fn')
        start = worker.index('/* FUA는')
        tail = worker[start:]
        self.assertLess(tail.index('blkdev_issue_flush'), tail.index('wal_page_append_sync'))
        self.assertLess(tail.index('wal_page_append_sync'), tail.index('wal_commit_ctx'))

    def test_gc_cannot_snapshot_volatile_overlay(self):
        worker = function('gc_work_fn')
        self.assertLess(worker.index('wal_buffered_count'), worker.index('c->gc_active = true'))
        self.assertIn('c->gc_active', function('wal_try_buffer_write'))

    def test_checkpoint_cannot_skip_buffered_records(self):
        flush = function('schedule_memtable_flush_locked')
        self.assertLess(flush.index('!list_empty(&c->wal_pending)'),
                        flush.index('flushed_split_off ='))
        self.assertIn('REQ_PREFLUSH | REQ_FUA', function('submit_checkpoint_async'))
        compact = function('compaction_work_fn')
        self.assertLess(compact.index('blkdev_issue_flush'), compact.index('zone_reset_hw'))

    def test_barrier_error_and_teardown_drain(self):
        pending = function('pending_flush_work_fn')
        self.assertIn('failed = c->metadata_failed', pending)
        self.assertIn('wal_writeback && list_empty(&c->pending_flush_bios)', pending)
        self.assertIn('pending->bio->bi_status = BLK_STS_IOERR', pending)
        self.assertIn('mod_delayed_work(zns_wal_wq', function('zns_base_dtr'))
        self.assertIn('!READ_ONCE(c->wal_buffered_count)', function('zns_base_dtr'))
        self.assertIn('cancel_delayed_work_sync(&c->wal_batch_work)', function('zns_base_dtr'))
        self.assertIn('mod_delayed_work(zns_wal_wq', function('wal_rotate_work_fn'))

    def test_volatile_cache_advertises_barriers(self):
        self.assertIn('ti->flush_supported = wal_writeback', function('zns_base_ctr'))

    def test_buffered_allocation_failure_cannot_gc_away_old_data(self):
        worker = function('wal_batch_work_fn')
        self.assertIn('c->stopping || (wal_writeback && c->wal_buffered_count)', worker)
        self.assertIn('wal_complete_buffered(ctx, wal_status)', function('wal_commit_ctx'))


if __name__ == '__main__':
    unittest.main()
