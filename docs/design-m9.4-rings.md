<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M9.4 per-pair scheduler rings

> **Status: STAGE 1 LANDED; STAGE 2 REFUSED.** The full design, measurements, superseded sketches
> and numbered sections A to H are in the M9.4 record (archived `M9.4_rings_record.md`). The current
> scheduler contract is in [architecture](reference/architecture.md) and the code.

Each kernel core owns its ready structures. Cross-core wake, handoff and re-seat requests are
published through per-pair rings and drained by the target core. The target's scheduler pass
updates its own ready queue. Publication, draining and placement remain under the one kernel
lock. A peer's published level guides placement; an owed raise is flushed to the target.

The proposal to move the switch half of a scheduler pass outside the lock was refused. Its
measured ceiling on two-core ESP32-WROOM was below the 5% gain required by the stage gate.
The lock remains the serialization boundary, including map edits and install. The stage-1
performance repair met its regression budget; the archived record (archived `M9.4_rings_record.md`)
keeps the exact measurements and the declined alternatives.
