# GC validation reuse

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

This is not a guarantee that the 60-second ACK-stall threshold or endurance
test will pass: the validation pass itself is unchanged. `gc-diag:
validated-reuse ... hits=...` counts avoided SSTable lookups in the move pass.
Compare it with move-cost and space-wait logs.

Run `python3 scripts/test-gc-validated-lookup.py` for a userspace build of the
production helper with UBSan and deterministic catalog-race injection. This does
not replace a Linux module build and concurrent FEMU runtime validation.
