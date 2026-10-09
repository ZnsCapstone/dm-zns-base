# GC bulk validation and move reuse

GC validation defaults to `gc_bulk_validation=1`. Instead of issuing up to one
SSTable binary-search lookup for every valid victim slot, it builds one
newest-sequence mapping snapshot by reading each published SSTable
sequentially in 32-block, lower-queue-bounded bios. RAM MemTables are merged
before and after the catalog scan. This two-sided merge prevents a frozen
MemTable entry from being missed while it is published into a new SSTable.
Catalog SRCU pins the copied descriptors' zones until the scan completes.

The temporary snapshot costs `logical_blocks * sizeof(struct mapping_entry)`;
for the benchmark's 16 GiB logical device this is about 96 MiB. Allocation
failure falls back to the original per-slot point validation. I/O, CRC, and
catalog errors do not fall back and remain fatal GC errors. Set
`gc_bulk_validation=0` to force the old point-validation path for comparison.

The GC worker optionally retains one byte per victim slot (512 KiB for 524288
slots, capped at 1 MiB). Only exact successful validation matches are retained.
`gc_validated_reuse=0` or allocation failure uses normal lookup.

Before moving a slot, RAM mappings take precedence, including tombstones. On a
RAM miss, GC reuses the exact version established by validation even if the
SSTable catalog changed. This can copy bytes that became stale after validation,
but cannot publish them: WAL publication resolves the latest mapping again and
rejects any `{PBA, seq}` mismatch while invalidating the old reverse-map slot.
An isolated GC_VICTIM cannot receive new allocations. Existing slot identity
checks and reset guards remain in place.

`gc-diag: phase=validate ... mode=bulk tables=... elapsed_ms=...` reports the
bulk pass. `gc-diag: validated-reuse ... hits=...` counts avoided SSTable
lookups in the following move pass. The WAL's conditional publication and the
final victim reset guard remain unchanged, so a mapping changed after the
snapshot cannot be published incorrectly.

Run `python3 scripts/test-gc-validated-lookup.py` for a userspace build of the
move-reuse helper with UBSan and deterministic catalog-race injection, and
`python3 scripts/test-gc-bulk-validation.py` for bulk-path wiring and race-order
checks. These do not replace a Linux module build and concurrent FEMU runtime
validation.
