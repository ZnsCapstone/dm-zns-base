# GC relocation read-ahead and failure diagnostics

## Observed failure (not a completed root-cause proof)

The endurance run started around 80% filesystem occupancy and ended around
96.86%. A foreground space wait lasted 390876 ms. Victim 14 validation took
46524 ms and its move phase took 154960 ms; reset eventually succeeded.
The producer aborted after 60 seconds without a successful ACK. A later GC
run reported -5 without a precise failure site. Completion of short occupancy
runs therefore did not establish endurance stability.

The old relocation loop does a mapping lookup, reservation, synchronous 4 KiB
read and synchronous 4 KiB write for each live block, followed by WAL staging.
The move duration includes more than physical copy time. Foreground admission
still requires free zones above its effective reserve; the wake after a reset
does not imply that any one reset always makes a writer admissible.

## Scoped change

- Add `gc_read_ahead` (read-only module parameter, default true).
- Allocate at most 128 KiB per GC worker invocation. On allocation failure or
  when disabled, retain single-page reads.
- On a miss read up to 32 contiguous valid victim slots, bounded by the lower
  transfer limit, victim boundary and committed write pointer.
- Reuse immutable physical bytes only within this victim. Clear the window
  before selecting a new victim; free it when the GC invocation ends.
- Keep current mapping checks, conditional WAL publication, destination writes
  and reset guards. Foreground overwrites can invalidate prefetched slots but
  cannot rewrite bytes in the isolated GC victim. They still win through the
  existing conditional publication check.
- Do not publish a failed read into the buffer. Fall back to the existing
  single-page read when bio allocation or bio construction fails.

Writes remain 4 KiB and synchronous. This patch is not full relocation batching,
does not shorten every GC wait, and does not claim to fix the unexplained -5.
No reserve, capacity, timeout, filesystem, retention or benchmark policy changes.

## Diagnostics

`phase=move end` logs read_ios (submitted read attempts), read_hits (reused
blocks), elapsed time and result. `move-cost` logs cumulative milliseconds in
mapping lookup, mapping-slot reservation, read, write and WAL staging calls.
These are worker wall times, not pure device latency. They exclude other code,
including the final WAL flush and reset; background WAL work can overlap them.
Timing calls run even with gc_diagnostics disabled. Partial progress logs retain
their existing five-second cadence; a blocked individual call cannot emit them.

Error logs identify GC phase/victim, and relocation substage/slot. Validation
lookup errors identify the exact LBA. SSTable catalog/header failures now log
their context. These diagnostics must be used to locate -5, not suppress it.

## Tests and deployment limits

Run:

```
python3 scripts/test-gc-read-ahead.py
python3 scripts/test-lookup-cache.py
git diff --check
```

The new tests compile the production read helper with fake block I/O and UBSan.
They cover reuse and copy isolation, queue/victim/write-pointer boundaries,
invalid holes, allocation and bio construction fallbacks, failed reads and
disabled buffering. Source assertions check retained mapping/reset wiring.
They do not test real kernel concurrency, writes, WAL recovery or performance.

Before endurance testing, build on the guest's 5.15.0-186 kernel and run existing
M2, M3 and WAL recovery tests on explicitly verified disposable devices. Tests
may reset devices; never run them on a mounted benchmark device. Confirm the
loaded module and parameters, save complete logs, then compare the same workload
and initial/actual occupancy with gc_read_ahead=0 versus 1. Benchmark module
reloads must preserve the selected parameter for that comparison.

Success requires successful resets and integrity checks, fewer read I/Os and
lower move/space-wait times, and completion of the full endurance duration.
Hits or a clean local test alone do not establish this. If writes, WAL staging
or reservation dominate, a separate correctness-reviewed change is needed.
