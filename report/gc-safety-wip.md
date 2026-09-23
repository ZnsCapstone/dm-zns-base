# WIP: GC / flush safety fixes (2026-09-23)

This is an experimental correctness change, not a claim that the EXT4 incident
or all crash-recovery paths are fixed. Do not use it on valuable data.

## Changes

- Merge **new keys**, not just overwrites, from the active memtable before every
  victim. Snapshot allocation happens outside the spinlock, with growth slack;
  an incomplete copy or allocation error aborts without reset. Publish any hash
  resize back to the cycle owner. Check remaining memtable references before reset.
- Propagate conditional mapping insertion errors from relocation. Log failing
  batch/errno and distinguish data allocation from WAL allocation failure.
- Serialize memtable flushes. Reads search active then frozen memtable (including
  tombstones). Keep frozen ownership until its SSTable is published and the
  chain ends. Failure before publication retains memory and fails I/O/GC closed
  until detach/recovery; failure in any checkpoint prevents WAL durable-frontier
  advancement for the rest of this target lifetime.
- Revalidate asynchronous SSTable reads after a publication, including reads
  whose old snapshot missed. A changed publication epoch restarts the snapshot
  rather than returning an obsolete location after the frozen table is released.
- Remove foreground borrowing of GC reserves, require reserve >= 2, check
  victim workspace, and protect the remaining per-victim GC WAL budget from
  foreground WAL appends. GC_DATA tail capacity is reusable; a full WAL zone is
  conservatively budgeted because foreground shares the active WAL stream.

These changes can increase CPU/memory use and cause earlier explicit ENOSPC in
fully-live workloads. Reserve protection does not create reclaimable data or
guarantee the workload can run indefinitely. Failed checkpoints conservatively
retain WAL and can ultimately exhaust space. Performance needs fresh measurement.

## Local non-destructive validation

```
python3 scripts/test-safety-unit.py
make -C src KDIR=/lib/modules/5.15.0-191-generic/build -j4
```

The Python suite extracts production C helpers and compiles them with mock
kernel APIs and UBSan. Cases cover new-key refresh, hash growth, incomplete
snapshots, allocation failure, failed mapping commit, CAS conflict, frozen
visibility/tombstones, failed flush retention, asynchronous read restart/OOM,
and reserve/WAL-budget decisions. Source wiring assertions cover reset guards.
It does **not** simulate real concurrency, storage I/O, zone reset or crash replay.
The local environment lacks 5.15.0-186 headers; rebuild on that guest kernel.

## Guest revalidation (destructive experiment device only)

Preserve the previous results and recovered journal first. Stop other benchmark
runs. Build the module in the guest; do not copy a host-built .ko into the guest.

```
uname -r  # expected 5.15.0-186-generic
cd ~/dm-zns-base
python3 scripts/test-safety-unit.py
make -C src clean
make -C src
cd ~/kafka_python_result
DIAG_DURATION_SECONDS=600 bash run-gc-diagnostic.sh ext4 75 0
# Only after inspecting the short run:
DIAG_DURATION_SECONDS=3600 bash run-gc-diagnostic.sh ext4 75 0
```

The existing runner resets the configured FEMU experiment device. Check JSON
validity, full IntegrityDiag summary, GC audit, relocation errors, workspace
failures and all three collection statuses. An exit code of zero is not a pass.
If any overlap, read replay, failed reset guard or I/O error appears, preserve
the entire logs; do not treat a throughput result as successful validation.

Separate tiny-zone null_blk/FEMU integration tests, fault injection and crash
tests are still required before considering these fixes production-safe.
