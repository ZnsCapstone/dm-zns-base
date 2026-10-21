# GC foreground-progress policy

## Failure this change addresses

An ext4/Kafka endurance run at 80% initial filesystem occupancy exhausted all
free DATA zones and aborted after 60 seconds without a producer ACK.  The saved
kernel timeline showed a foreground space wait of 133,412 ms.  GC relocated
303,659 blocks from one victim in 39,424 ms and then 408,252 blocks from the
next victim in 89,420 ms before waking the writer.  The second move spent
68,462 ms in WAL work.  Other runs repeatedly revalidated completely live
zones, and GC sometimes moved almost an entire zone to reclaim only a small
fraction of it.

Bulk victim validation reduced lookup time, but it did not bound foreground
wait because the worker could continue toward the background high watermark
and conditional publication still performed one mapping lookup per relocated
record.

## Policy and publish changes

- `mapping_state.latest_seq` stores the latest published sequence for every
  logical block.  It is updated under `c->lock`, restored from the checkpoint,
  and advanced by WAL replay.  GC conditional publish compares this exact
  version instead of doing a sleeping SSTable lookup per relocated record.
- A successful reset observed by `foreground_waiters` creates a persistent,
  zone-specific `foreground_zone_grant`.  If another FREE zone remains for GC
  destination rotation, the writer receives the completely empty reset victim
  and therefore a full-zone runway.  Under tighter space pressure it promotes
  the still-open GC destination and preserves the reset victim as reserve.
  The grant is consumed only during activation, so admission stays stable.
- `valid_blocks` is maintained as a conservative live-block count, so
  `nr_blocks - valid_blocks` is a continuously available reclaimable hint.
  Foreground emergency GC ranks FULL zones by this hint, exactly validates only
  the best candidate, and relocates it immediately.  It no longer puts a full
  multi-zone validation pass on the blocked writer's critical path.
- `gc_low_watermark` schedules background work only; it does not block a
  foreground zone rollover.  Foreground admission uses the hard relocation
  reserve.  This separation is intentional: forcing emergency collection at
  the low watermark can relocate an almost entirely live zone before ongoing
  overwrites have created enough reclaimable space.  A zone-specific reset
  grant still bypasses the hard reserve for deadlock-free emergency progress.
- `gc_reserved_zones`, `gc_low_watermark`, and `gc_high_watermark` are
  read-only module parameters.  Their required ordering is
  `reserved <= low < high`; the old `gc_target_free_zones` name remains an
  alias for `gc_high_watermark` so existing keper1212 test scripts continue to
  work.
- `gc_min_reclaim_percent` defaults to 20 while the current ACTIVE zone has
  runway.  When free-zone headroom is within one zone of the hard reserve and
  the ACTIVE zone is at least 25% used, `gc_pressure_reclaim_percent` lowers
  the threshold to 10, leaving roughly 75% of that zone as foreground runway
  while relocation completes.  Pressure selection also bypasses the clean-zone
  cooldown so reclamation starts before rollover.  This retains the lower
  write amplification of a 20% quality target without starving the free-zone
  pipeline as the fixed-20% policy did.  Foreground emergency GC still bypasses
  all quality thresholds after validating its single ranked candidate.
- `gc_clean_zone_cooldown_ms` defaults to 30000.  A completely live zone is not
  repeatedly scanned by background GC during this interval.  An exact mapping
  invalidation clears the cooldown, and foreground pressure ignores it.
- Quiescing stops selection of a new victim.  Teardown therefore waits at most
  for the currently active victim instead of starting another long move.

The latest-sequence index costs eight bytes per logical 4 KiB block (about
32 MiB for a 16 GiB logical target).  It does not change the on-disk format.
The reset guard, WAL durability boundary, reverse-map version checks, and
foreground-wins conditional publication remain in place.

## Diagnostics

`dmsetup status` now includes the configured reserve/low/high watermarks,
`foreground_waiters`, `foreground_zone_grant`, and `foreground_grant_zone`. With
`gc_diagnostics=1`, policy decisions also emit `victim deferred` and
`victim emergency` records.  Existing `phase=move`, `move-cost`,
`foreground-space-wait`, WAL, and reset diagnostics remain available.

## Validation

Source-level regression tests:

```sh
python3 scripts/test-gc-pressure-policy.py
python3 scripts/test-gc-bulk-validation.py
python3 scripts/test-gc-write-batch.py
python3 scripts/test-gc-read-ahead.py
python3 scripts/test-gc-validated-lookup.py
python3 scripts/test-lookup-cache.py
python3 scripts/test-lookup-result-cache.py
python3 scripts/test-active-zone-state.py
```

The module must still be compiled against the running guest kernel and tested
on a disposable ZNS device.  A successful build or short test does not prove
the endurance issue fixed.  Compare the same 80% workload and require no
producer send errors, no integrity errors, completion of the intended duration,
and materially shorter foreground space waits.  Save the complete kernel log
before the benchmark tears down the mapper.
