#!/usr/bin/env python3
"""Regression checks for WAL recovery after a torn physical tail."""
import pathlib
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


class WALTailRecoveryTests(unittest.TestCase):
    def test_invalid_tail_seals_latest_zone(self):
        recover = function_body("static int zns_base_wal_recover(")
        self.assertIn("bool sealed[ZNS_BASE_WAL_ZONES]", recover)
        self.assertGreaterEqual(recover.count("sealed[selected] = true;"), 2)
        self.assertIn("!sealed[i] && zone->write_pointer < end", recover)

    def test_sealed_stream_forces_rotation(self):
        has_space = function_body("static bool zns_base_metadata_has_space(")
        self.assertIn("zone->state != ZNS_BASE_ZONE_ACTIVE", has_space)

        checkpoint = function_body("static int zns_base_checkpoint_locked(")
        self.assertIn("c->metadata.wal.stream.active_zone_idx =", checkpoint)
        self.assertIn("ZNS_BASE_ZONE_ACTIVE;", checkpoint)

    def test_recovered_mappings_require_written_unique_slots(self):
        manifest = function_body("static int zns_base_manifest_recover(")
        replay = function_body("static int zns_base_replay_wal_put(")
        for body in (manifest, replay):
            self.assertIn("physical_sector >= zone->write_pointer", body)
            self.assertIn("zone->slots[slot].valid", body)
            self.assertIn("zone->slots[slot].pending", body)


if __name__ == "__main__":
    unittest.main()
