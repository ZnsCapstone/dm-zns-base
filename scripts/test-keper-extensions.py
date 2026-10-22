#!/usr/bin/env python3
"""실제 순수 C 정책 helper + extent/WAL/조회 오류 경계 구조 회귀."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('safety', ROOT/'scripts/test-safety-unit.py')
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
function = safety.function


class ExtensionTests(unittest.TestCase):
    def test_actual_c_limits_pressure_groups_and_failpoint(self):
        prelude = '''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t sector_t;
#define BLOCK_SECTORS 8
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define READ_ONCE(x) (x)
#define cmpxchg(p,o,n) __sync_val_compare_and_swap(p,o,n)
static bool adaptive_gc;
static unsigned int wal_batch_max_records=127,wal_group_pages=1,failpoint;
'''
        cases = '''int main(void) {
assert(wal_group_capacity()==127); wal_group_pages=16;
assert(wal_group_capacity()==2032);
assert(append_extent_sectors(2048,64,512,1024)==512);
assert(append_extent_sectors(2048,64,256,1024)==256);
assert(append_extent_sectors(2048,64,512,128)==128);
assert(append_extent_sectors(8,64,512,1024)==8);
assert(append_extent_sectors(2048,1,512,1024)==8);
assert(append_extent_sectors(2048,64,0,1024)==8);
assert(append_extent_sectors(2048,64,15,1024)==8);
assert(gc_reclaim_threshold(10,8,1,100)==5); adaptive_gc=true;
assert(gc_reclaim_threshold(10,8,99,100)==20);
assert(gc_reclaim_threshold(9,8,24,100)==20);
assert(gc_reclaim_threshold(9,8,25,100)==10);
assert(gc_reclaim_threshold(8,8,90,100)==0);
assert(gc_reclaim_threshold(7,8,90,100)==0);
assert(!failpoint_hit(1)); failpoint=2;
assert(!failpoint_hit(1)); assert(failpoint_hit(2));
assert(failpoint==0 && !failpoint_hit(2)); return 0;
}'''
        source = prelude + '\n'.join(function(name) for name in [
            'wal_group_capacity', 'gc_reclaim_threshold', 'append_extent_sectors', 'failpoint_hit']) + cases
        with tempfile.TemporaryDirectory(prefix='keper-extensions-') as folder:
            p = Path(folder)
            (p/'test.c').write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(p/'test.c'), '-o', str(p/'test')], check=True)
            subprocess.run([str(p/'test')], check=True)

    def test_extent_chain_and_reservation_ownership(self):
        done = function('extent_append_done')
        self.assertNotIn('__bi_remaining', done)
        self.assertIn('bio->bi_end_io = extent_append_done', function('submit_data_append_async'))
        self.assertLess(done.index('bio_chain('), done.index('submit_wal_async(child)'))
        self.assertIn('base + (sector_t)i * BLOCK_SECTORS', done)
        start = function('start_foreground_write')
        self.assertIn('kzalloc(sizeof(*ctx)', start)
        self.assertIn('bio_clone_fast', start)
        self.assertIn('extent_nomem:', start)
        self.assertIn('free_extent_children(ctx)', function('abort_after_header_failure'))

    def test_group_checksums_and_final_prefix_flush(self):
        worker = function('wal_batch_work_fn')
        self.assertIn('count < wal_group_capacity()', worker)
        self.assertIn('pages[p]->crc32 = cpu_to_le32(crc32', worker)
        self.assertIn('pages[p], WAL_PAGE_SECTORS, p == nr_pages - 1', worker)
        self.assertIn('bio->bi_opf |= REQ_PREFLUSH', function('wal_page_append_sync'))
        self.assertIn('(wal_group_pages > 1 && !wal_writeback)', function('zns_base_ctr'))

    def test_probe_error_never_becomes_not_found(self):
        done = function('sstable_probe_done')
        self.assertIn('rctx->error = status', done)
        self.assertIn('sstable_read_finish(rctx)', done)
        self.assertNotIn('rctx->idx++', done)
        probe = function('sstable_read_probe')
        self.assertIn('rctx->error = BLK_STS_RESOURCE', probe)
        self.assertIn('rctx->cached_sector == sec_no', probe)

    def test_faultpoints_precede_publication_or_reset(self):
        for name, point in [('data_append_done',1), ('wal_batch_work_fn',2),
                            ('sstable_flush_complete',3), ('submit_checkpoint_async',4),
                            ('zone_reset_hw',5)]:
            self.assertIn(f'failpoint_hit({point})', function(name))


if __name__ == '__main__':
    unittest.main()
