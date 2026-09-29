# SSTable lookup cache: validation notes

The fixed-reservation implementation now caches immutable SSTable blocks used
by point lookups, including GC validation and relocation lookups. It does not
cache LBA-to-PBA answers or change victim selection, allocation reserves, WAL
commit ordering, reverse-map checks, or reset guards.

Each target optionally allocates 4096 cache entries (16 MiB payload plus tags).
Allocation failure falls back to uncached reads. `sstable_lookup_cache=0` at
module load disables allocation for controlled comparisons. The default is on.

Cache keys contain the physical sector and an in-memory catalog epoch. Both
catalog publication sites increment the epoch inside the existing seqcount
update; lookups capture the epoch with their descriptor snapshot. Existing SRCU
protection spans cache access and disk reads, preventing reset of tables still
being read. Cache locks cover only tag checks and copies, never disk I/O. Late
old-epoch fills can evict useful entries but cannot hit under a newer epoch.
Lookup callers receive copies (header CRC validation mutates its input).

`dmsetup status` reports `lookup_cache_enabled`, `lookup_cache_hits`, and
`lookup_cache_misses`. These count all point-lookup block requests, not only GC
or unique blocks. Misses include failed disk read attempts. Counters are sampled
independently and are not an atomic snapshot of concurrent lookups.

## Local non-destructive tests

Run `python3 scripts/test-lookup-cache.py`. It compiles the production cache
helper with fake I/O and UBSan, checking reuse, collisions, catalog changes,
late old-epoch fills, error propagation, copy isolation and allocation fallback.
Source assertions check lookup and publication wiring. This is NOT a kernel
concurrency, crash recovery, or performance test.

## Guest validation still required

Build under the experiment's 5.15.0-186 kernel. After safely detaching the old
target/module, load the new module and run existing M2, WAL recovery and GC
tests only on verified disposable test devices. The scripts may reset devices.
Then reproduce the same EXT4 occupancy progression with discard off, preserving
the full kernel log and actual filesystem occupancy for every scenario.

Compare GC `phase=validate` scanned/elapsed, then verify transition through
move, WAL flush and successful reset, actual `gc_resets` growth, foreground
space-wait completion, and application integrity/errors. An improved hit rate
alone does not prove the capacity stall is fixed. Catalog churn, cache collisions
or a later relocation bottleneck may still limit progress. Separate occupancy
mislabeling and benchmark shutdown timeout issues are not changed by this patch.
