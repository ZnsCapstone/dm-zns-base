# Zone Append experiment

This branch sends user data and WAL records with `REQ_OP_ZONE_APPEND`. Multiple
appends to one initialized zone may remain outstanding; the device selects the
actual write location and returns it on completion.

The write protocol is:

1. reserve capacity in a USER_DATA zone;
2. append the data and obtain its actual physical sector;
3. append `PUT(lba, actual_phys)` to the WAL;
4. publish the mapping and complete the original bio.

A new zone's conventional header write must complete before queued appends are
released. This prevents an append from occupying sector zero before the
recovery header.

## FEMU validation gate

Before loading the module, verify that the device reports a non-zero Maximum
Zone Append Size. The target constructor rejects devices that cannot append a
4 KiB request.

Run the basic, crash, GC, and Kafka tests in that order. In addition to normal
data verification, inspect raw-device I/O errors and confirm after crash/reload
that replay maps every acknowledged write correctly.

## Crash-consistency distinction

Data is durable before its WAL record. A crash in that interval leaves an
unreferenced physical record (an orphan), but never publishes a mapping to
undurable data. Space reclamation for recovery-time orphans should be measured
before treating this branch as production-ready.
