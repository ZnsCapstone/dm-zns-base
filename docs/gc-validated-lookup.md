# GC validation reuse

The GC worker optionally retains one catalog epoch per victim slot (4 MiB for
524288 slots, capped at 8 MiB). Only exact successful validation matches are
retained, and only if the catalog seqcount remained unchanged during lookup.
Epoch zero, disabled SSTable caching, or allocation failure uses normal lookup.

Before moving a slot, RAM mappings take precedence, including tombstones. A RAM
miss may reuse the validated slot version only while the catalog epoch and
seqcount remain unchanged. Frozen MemTables are removed after catalog publication,
so eviction cannot hide an update without invalidating this cache. An isolated
GC_VICTIM slot cannot receive a new mapping version. Existing slot identity checks,
WAL publication's latest-mapping comparison, and reset guards remain in place.

Catalog churn can make reuse ineffective; this is not a guarantee that the
60-second ACK-stall threshold or the endurance test will pass. The validation
pass itself is unchanged. `gc-diag: validated-reuse ... hits=...` counts avoided
SSTable lookups in the move pass. Compare it with move-cost and space-wait logs.

Run `python3 scripts/test-gc-validated-lookup.py` for a userspace build of the
production helper with UBSan and deterministic catalog-race injection. This does
not replace a Linux module build and concurrent FEMU runtime validation.
