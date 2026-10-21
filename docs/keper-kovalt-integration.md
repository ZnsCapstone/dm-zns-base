# keper1212 + Kovalt03 integration

This branch keeps `zns_dm/keper1212` as the architectural base and integrates
the remaining useful controls from `zns_dm/Kovalt03` without replacing the
RB-tree/Manifest recovery design or adding the old fixed 2 ms WAL delay.

## Combined behavior

- Keper's timer-free WAL grouping, recovery/checkpoint format, GC batching,
  lookup caches, failure injection, and foreground-progress handoff remain the
  default behavior.
- GC reserve and hysteresis are configurable with
  `gc_reserved_zones`, `gc_low_watermark`, and `gc_high_watermark`.  They must
  satisfy `reserved <= low < high`.  `gc_target_free_zones` is retained as a
  deprecated alias for `gc_high_watermark`.
- `strict_wal_durability=0` preserves Keper's writeback completion semantics.
  With `strict_wal_durability=1`, every foreground write waits for the existing
  DATA-flush + WAL-FUA durable-commit path.  This provides the stronger
  completion boundary for durability evaluation without a fixed batching
  timer.
- `wal_batch_max_records` can force a smaller durability group without adding
  a delayed worker.  Zero preserves Keper's full `wal_group_pages` capacity.
- `wal_batch_alloc_failures` deterministically fails the requested number of
  foreground WAL commit allocations for error-path testing.  GC WAL commits
  are deliberately not affected.
- `gc_diag_budget` bounds GC diagnostic lines over a module load; zero keeps
  the existing unlimited behavior.
- `metrics_enabled=0` disables optional cache, WAL-group, SSTable-flush, and
  compaction evaluation-counter updates while leaving the algorithms enabled.
- Kovalt names `flush_threshold` and `compaction_k` are accepted as aliases for
  `memtable_capacity_entries` and `sstable_compaction_threshold`.
- `dmsetup status` reports these controls so a benchmark artifact can prove
  which mode was loaded.

## Deliberate compaction difference

Kovalt's `compaction_k` merges the oldest K skiplist SSTables.  Keper stores a
catalog in a two-zone Manifest/SSTable recovery layout and safely resets the
old metadata zone only after every live input has been packed into the other
zone.  A literal oldest-K port would leave live inputs in the zone being reset
or require a new multi-zone lifetime protocol.  On this branch `compaction_k`
therefore controls when Keper's full-catalog compaction runs; it does not select
a partial input set.  This distinction is exposed in the parameter description
and must not be reported as Kovalt's K-way compaction.

## Recommended evaluation settings

For performance comparison, use the same mode on every filesystem.  The
existing benchmark configuration maps to:

```sh
sudo insmod src/dm-zns-base.ko \
  gc_reserved_zones=2 gc_low_watermark=4 gc_high_watermark=5 \
  wal_batch_max_records=0 strict_wal_durability=0 metrics_enabled=1
```

Run a separate durability experiment with `strict_wal_durability=1`; do not
mix those results with writeback-mode throughput results.  After loading,
verify the selected values with both `cat /sys/module/dm_zns_base/parameters/*`
and `dmsetup status`.
