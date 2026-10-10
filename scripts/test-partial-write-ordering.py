#!/usr/bin/env python3
"""Verify partial writes are ordering boundaries for the async DATA path."""
import pathlib
import subprocess
import tempfile
import unittest


SOURCE = (pathlib.Path(__file__).resolve().parents[1] /
          "src/dm-zns-base.c").read_text()


def function_body(signature: str) -> str:
    start = SOURCE.rindex(signature)
    brace = SOURCE.index("{", start)
    depth = 0
    for index in range(brace, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:index + 1]
    raise AssertionError(f"unclosed function: {signature}")


class PartialWriteOrderingTests(unittest.TestCase):
    def test_rmw_classifier(self):
        helper = function_body(
            "static bool zns_base_write_needs_rmw_ordering(")
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define REQ_OP_READ 0
#define REQ_OP_WRITE 1
#define SECTORS_PER_BLOCK 8
#define ZNS_BASE_BLOCK_SIZE 4096
struct bio { struct { uint64_t bi_sector; unsigned int bi_size; } bi_iter;
             int op; };
#define bio_op(bio) ((bio)->op)
'''
        cases = r'''
int main(void) {
    struct bio bio = { .bi_iter = { .bi_sector = 0, .bi_size = 4096 },
                       .op = REQ_OP_WRITE };
    assert(!zns_base_write_needs_rmw_ordering(&bio));
    bio.bi_iter.bi_sector = 1;
    assert(zns_base_write_needs_rmw_ordering(&bio));
    bio.bi_iter.bi_sector = 0;
    bio.bi_iter.bi_size = 512;
    assert(zns_base_write_needs_rmw_ordering(&bio));
    bio.op = REQ_OP_READ;
    assert(!zns_base_write_needs_rmw_ordering(&bio));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "test.c"
            exe = pathlib.Path(tmp) / "test"
            src.write_text(prelude + helper + cases)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_dispatcher_waits_before_rmw(self):
        dispatcher = function_body("static void zns_base_io_work(")
        self.assertIn("zns_base_write_needs_rmw_ordering(io->bio)",
                      dispatcher)
        self.assertIn("c->foreground_data_inflight", dispatcher)


if __name__ == "__main__":
    unittest.main()
