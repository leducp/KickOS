<!-- SPDX-License-Identifier: CECILL-C -->
# The KickOS Reference

The **code-synced technical reference**: what KickOS does, exactly. Terse, invariant-first.
The contract for this tier: **the code is the source of truth -- if a page and the code
disagree, the page is a bug.** A diff that changes observable behavior must update the page
in the same change. (The *how & why* narrative lives one tier up, in `../book/`; this tier is
what you consult to check the exact contract. The Book links in here; here we cite the code
and, for hardware facts, the TRM section.)

## Pages
- **`architecture.md`** -- the kernel's design and the arch/chip seam: design pillars,
  scheduler model, user/kernel split, memory domains, driver/interrupt model, C++ and
  build decisions. The structural contract the per-ISA/per-chip pages instantiate.
- **`invariants.md`** -- the invariants a change must not break (the checklist a review runs).
- **`porting.md`** -- the arch-seam contract: how to add a board / chip / ISA, and what each
  `arch_*` entry point must guarantee.
- **`console.md`** -- the console model: polled vs buffered ring, the sync (panic) path, the
  arch forwarding seam.
- **`telemetry.md`** -- the trace wire-format: the record layout, the pure encoders/decoders,
  and the golden-vector source of truth.
- **`boards.md`** -- per-board reference: pins, console, LED, flash backend, and per-target
  quirks. The source of truth for "how board X is wired", with a summary HW-validated indicator
  per board. The authoritative validation *matrix* (per-app, per-board results) is
  `../../M1_state.md`; flashing *how-to* is in `../flashing.md`.
