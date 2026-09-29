<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M4.8.2 host unit-test seams

> **Status: LANDED.** The spike, mutations, landing corrections and numbered sections are in
> [the host-test record](archive/M4_host_unit_test_record.md). Follow-up work is tracked in
> `TODO.md`.

The host layer links production sources into ordinary test executables and substitutes functions
at an existing boundary:

| Seam | Boundary | Subject |
|---|---|---|
| U | `kos_*` syscalls | User libraries, driver bring-up and service behavior |
| K | `arch_*` and related kernel boundary functions | Scheduler, capabilities, synchronization and time |

The U seam needs no kernel fixture. The K seam uses a resettable kernel fixture; a returned
`arch_switch` can prove park state but cannot by itself prove the resumed call's result. The park
resolver supplies that missing witness. The test framework is GoogleTest via Conan. Host gates
carry the `host` ctest label; a mock peripheral does not establish silicon behavior.

The class-backend gate covers public driver and syscall symbols, preventing a test double from
silently replacing a target backend. Migration of suitable on-target selftest arms to the host
fixture remains follow-up work.
