<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS documentation -- map & conventions

Entry point for the docs. Read this first to know **where things live**. The docs are two
tiers plus supporting material:

- **`book/`** -- *the how & why* (narrative + teaching), and
- **`reference/`** -- *the what, exactly* (code-synced technical reference).

The split is deliberate: the Book explains and teaches and is stable across refactors; the
Reference states the exact contract and is a bug when it drifts from the code. The Book links
into the Reference for detail; the Reference links back for context and cites the code/TRM.

## Where things live

### `book/` -- The KickOS Book (how & why + teaching)
The durable narrative: what KickOS is, why it is built this way, and a teach-how-a-microkernel-
works text (prereq: minimal C/C++ + compile/link/flash; Tanenbaum as the further-reading spine).
Concept chapters stand on their own; KickOS-specific chapters explain the design and point into
the Reference for exact contracts. Start at `book/README.md`. **Not** code-synced 1:1 -- a
concept does not become a bug when the code is refactored.

### `reference/` -- The KickOS Reference (code-synced)
The exact technical contract; **the code wins, drift is a bug.** Start at `reference/README.md`.
Covers `architecture.md` (kernel design + arch/chip seam), `invariants.md`, `porting.md`
(arch-seam contract), `console.md`, `telemetry.md` (wire-format), `boards.md` (per-board wiring).

### State & roadmap -- where we are / what's next
Three altitudes, coarse to fine:
- **`../roadmap.md`** -- the milestone-level plan: the general goals/ideas to tackle per
  milestone, no granular items. Milestones are keyed to **theme, not sequence**:
  M2 = MPU/memory-protection enforcement; M3 = capabilities + user clock; M4 = SMP. Work with no
  MPU/caps/SMP dependency is "anytime coherence".
- **`../TODO.md`** -- the detailed, actionable task items behind the roadmap (the granular list).
- **`../M1_state.md`** -- the validated M1 end state (fleet, bench, fault dumps, scoping
  decisions), plus per-board HW-validation status.
- **`../M1_raw_meas.md`** -- raw console captures behind M1_state.
- **`m2-readiness.md`** -- the board/console readiness matrix + the M2 MPU-readiness notes.

### How-to / ops
- **`flashing.md`** -- flash-tool backends + the non-J-Link paths. (Per-target wiring is
  `reference/boards.md`; this box is the tooling.)

## Conventions (how the docs are kept)

- **State, not path.** Docs describe the *current* design/state; they do **not** narrate the
  sequence of fixes/commits that produced it (that's git). No "we first did X, then changed to Y".
  A milestone/task ledger is roadmap, not architecture -- it belongs in `../TODO.md`.
- **Terse, invariant-first comments** (same rule in code): explain hidden constraints/contracts,
  not what the code plainly says.
- **Spikes are ephemeral.** A design *spike* is scratch that lets one pass explore a path for the
  next to implement. When the code lands, the spike is **deleted + squashed out of history**, and
  its durable teaching is rewritten as a **Book chapter** (spike -> Book, never a lingering doc).
- **Two sync contracts.** The **Reference** is code-synced: if a page and the code disagree, the
  code wins and the page is a bug. The **Book** is durable narrative: reviewed occasionally, not
  pinned to each diff.
- **ASCII only.** Plain ASCII in docs and code: `--` not em-dash, `->` not arrow, straight quotes,
  "section"/"microkernel" not the glyphs.
- **Code style** (enforced): no ternary (`?:`); spelled logical operators (`and`/`or`/`not`, not
  `&&`/`||`/`!`; `!=` stays); traditional include guards (no `#pragma once`); SPDX header per
  `SPDX-header-template.txt`.
