<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Generic driver service

> **Status: LANDED.** The per-instance audit, descriptor validator and original code sketches
> are in the M4.8.1 record (archived `M4_generic_driver_service_record.md`). The sketches predate
> task-based drivers and M8.13 notifications. Use
> [driver_service.h](../user/include/kickos/sys/driver_service.h) and
> [bus-service](reference/bus-service.md) for current fields and behavior.

A service is transport over a device class. Each chip supplies a descriptor with its thread
entries, line bindings, memory block, readiness barrier, endpoint posture and MMIO guard.
Generic bring-up checks that description, allocates and grants the shared block, spawns the
threads into a task, waits for readiness and performs handover or retention. Class-specific
headers still own request parsing and device policy.

The descriptor must state every resource used by bring-up; the validator rejects missing or
inconsistent combinations at build time. Each claimed IRQ line signals a bit in one
notification. The former edge relay thread and per-thread memory grants are historical
shapes preserved only in the archive.
