# SSTable result cache

The cache stores only the answer from an immutable SSTable catalog snapshot,
keyed by catalog epoch and logical block. It does not cache the combined latest
RAM/disk mapping. `mapping_lookup_latest()` still checks active/frozen RAM first;
GC publication still compares the latest PBA and sequence with the expected
mapping before publishing a relocation.

Both catalog publication paths increment `lookup_epoch`. An old reader can
replace a cache slot but cannot supply a hit for a newer catalog. Negative
answers and tombstones follow the same epoch rule; I/O errors are never cached.
Catalog SRCU remains held across lookup and cache access. No disk format changes.

The direct-mapped cache has 16,384 entries (roughly 0.9 MiB on 64-bit builds),
uses the existing lookup-cache spinlock for short copies, and is controlled by
the existing read-only `sstable_lookup_cache` parameter. Allocation failure
falls back to ordinary SSTable lookup. Hash collisions reduce hits, not validity.
Status exposes `result_cache_enabled`, `result_cache_hits`, and
`result_cache_misses`. Existing block-cache hit counts may fall because an entire
SSTable search can now be avoided.

## Local checks

`python3 scripts/test-lookup-result-cache.py` compiles production cache helpers
with fake locks and counters under UBSan. Cases cover positive/negative entries,
epoch invalidation, delayed old readers, collisions, tombstones, error handling,
and allocation fallback. Source checks retain RAM-first lookup and GC sequence
validation. These tests do not establish real kernel concurrency or crash safety.

## Guest validation still required

Build the Linux module, confirm it contains `result_cache_hits`, and load it only
after the old mapper/module is safely removed. Preserve the benchmark workload.
Compare result-cache hit/miss counts, WAL publish time, GC lookup time, foreground
space waits, ACK failures, and consumer verification results. A passing endurance
run requires the entire requested duration and successful integrity checks;
lower publish time alone is not sufficient. Catalog churn may reduce benefit.
