# GC relocation write batching

GC packs at most 32 validated victim blocks (and never more than the lower
queue transfer limit or remaining destination capacity) into one sequential
bio. Per-LBA mapping validation, MemTable reservations, conditional WAL
publication, and victim reset guards remain unchanged.

The move-item array and write buffer are allocated once per GC worker run, not
on the kernel stack. If either allocation fails, GC uses the single-block path.

The device write completes before the destination software write pointer and
pending reverse-map slots are committed together under the target lock. WAL
staging then retains ownership of each pending slot and mapping reservation.
Preparation or I/O failures release reservations; catalog or foreground races
are still rejected by conditional WAL publication.

`gc_write_batch=0` disables the optimization at module load time. Allocation
failure also falls back to the previous single-block path. GC diagnostics now
report `write_ios` and `write_blocks` per victim; successful batching should
make `write_blocks` substantially larger than `write_ios`.

Run `python3 scripts/test-gc-write-batch.py` to compile the production atomic
commit helper with UBSan and verify write/commit/WAL ordering in the source.
This does not replace building the module against the guest kernel and running
the FEMU endurance workload.
