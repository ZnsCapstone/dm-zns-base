#!/usr/bin/env python3
"""실제 enqueue C 함수 + 가상 timer. 실제 커널 지연/IOPS 측정은 아님."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def driver_function():
    source = (ROOT / 'src/dm-zns-base-main.c').read_text()
    start = source.index('static void submit_wal_async(struct zns_io_ctx *ctx)\n{')
    end = source.index('\n}', start) + 2
    return source[start:end]


class WalUserspaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix='wal-userspace-')
        cls.addClassCleanup(cls.folder.cleanup)
        path = Path(cls.folder.name)
        harness = (ROOT / 'scripts/wal-userspace-harness.c').read_text()
        (path / 'test.c').write_text(harness.replace('/* DRIVER_FUNCTION */', driver_function()))
        cls.binary = path / 'test'
        subprocess.run(
            ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
             str(path / 'test.c'), '-o', str(cls.binary)],
            check=True,
        )

    def run_case(self, delay, count, step_ns):
        result = json.loads(subprocess.check_output(
            [str(self.binary), str(delay), str(count), str(step_ns)], text=True,
        ))
        print(json.dumps(
            dict(delay_ms=delay, count=count, arrival_step_ns=step_ns, **result),
            ensure_ascii=False,
        ), flush=True)
        self.assertEqual(result['records'], count)
        return result

    def test_qd32_like_staggered_arrivals(self):
        delayed = self.run_case(2, 32, 100000)
        immediate = self.run_case(0, 32, 100000)
        self.assertAlmostEqual(delayed['first_fire_ms'], 2)
        self.assertLessEqual(delayed['max_queue_ms'], 2)
        self.assertEqual(delayed['max_batch'], 20)
        self.assertEqual(immediate['max_queue_ms'], 0)
        self.assertEqual(immediate['batches'], 32)

    def test_single_request(self):
        self.assertEqual(self.run_case(2, 1, 0)['max_queue_ms'], 2)

    def test_full_batch_fires_immediately(self):
        result = self.run_case(2, 127, 1000)
        self.assertAlmostEqual(result['first_fire_ms'], 0.126)
        self.assertEqual(result['max_batch'], 127)

    def test_continuous_arrivals_do_not_extend_deadline(self):
        result = self.run_case(2, 255, 100000)
        self.assertLessEqual(result['max_queue_ms'], 2)
        self.assertGreater(result['batches'], 1)

    def test_arrivals_slower_than_delay(self):
        result = self.run_case(2, 10, 3000000)
        self.assertEqual(result['batches'], 10)
        self.assertEqual(result['max_queue_ms'], 2)


if __name__ == '__main__':
    unittest.main()
