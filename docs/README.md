<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS documentation -- map & conventions

Entry point for the docs. Read this first to know **where things live**. The docs are two
tiers plus supporting material:

- **`book/`** -- *the how & why* (narrative + teaching), and
- **`reference/`** -- *the what, exactly* (code-synced technical reference).

The split is deliberate: the Book explains and teaches and is stable across refactors; the
Reference states the exact contract and is a bug when it drifts from the code. The Book links
into the Reference for detail; the Reference links back for context and cites the code/TRM.

## Where things live

### `../STATE.md` -- current state (start here)
One screen: where the milestone stands, what is next in locked order, the board matrix, the open
blockers. **The only file that changes every milestone**, which is what lets the rest of the docs
be read once. It carries no history, no rationale and no task list; it links out instead.

### `book/` -- The KickOS Book (how & why + teaching)
The durable narrative: what KickOS is, why it is built this way, and a teach-how-a-microkernel-
works text (prereq: minimal C/C++ + compile/link/flash; Tanenbaum as the further-reading spine).
Concept chapters stand on their own; KickOS-specific chapters explain the design and point into
the Reference for exact contracts. Start at `book/README.md`. **Not** code-synced 1:1: a
concept does not become a bug when the code is refactored.

### `reference/` -- The KickOS Reference (code-synced)
The exact technical contract; **the code wins, drift is a bug.** Start at `reference/README.md`.
Covers `architecture.md` (kernel design + arch/chip seam), `invariants.md`, `porting.md`
(arch-seam contract), `console.md`, `telemetry.md` (wire-format), `boards.md` (per-board wiring),
`ipc-call-reply.md` (the synchronous call/reply transport), and `bus-service.md` (the SPI/I2C
service wire contract carried over it).

### State & roadmap -- where we are / what's next
**The re-grounding path is four files, in this order.** Stop as soon as you have what you came
for; nothing below is a prerequisite for reading the Reference.
1. **`../STATE.md`** -- current state (above). Usually enough on its own.
2. **`../TODO.md`** -- the detailed, actionable task items, and every open blocker in full.
3. **`m2-readiness.md`** -- the **enforcement ledger**: the board/console readiness matrix, the
   per-chip MPU fan-out, and the per-chip silicon evidence. Read it for "which chip is proven, by
   what evidence".
4. **`../roadmap.md`** -- the milestone-level plan: goals per milestone, no granular items.
   Milestones are keyed to **theme, not sequence**: M2 = MPU/memory-protection enforcement;
   M3 = capabilities + user clock; M4 = the driver era; M5 = SMP; M6 = the MMU / new-platform
   horizon. Work with no MPU/caps/driver/SMP dependency is "anytime coherence".

Per-board wiring and validation status is `reference/boards.md`, not any of the above.

### `design-*.md` -- design records and spikes
Per-topic design documents in this directory, each tagged in its header with a status
marker: **LANDED** (shipped; kept as the why), **ACTIVE** (work in flight),
**SUPERSEDED** (a later document or decision replaced it), **EXPLORATORY** (a spike; no
commitment). `design/README.md` indexes them by status. A LANDED record is history plus
rationale, not a contract: for the current contract go to `reference/`.

### How-to / ops
- **`flashing.md`** -- flash-tool backends + the non-J-Link paths. (Per-target wiring is
  `reference/boards.md`; this box is the tooling.)

### `archive/` -- measurement captures kept as evidence
Closed-milestone measurement records: raw console captures and the bench numbers behind them, kept
because they cost bench time and some can no longer be reproduced, and because they are future Book
material. **Never in the re-grounding path**: do not read it to find out where the project is.
See *Prose is regenerable, a measurement is not* under Conventions.

## Conventions (how the docs are kept)

- **State, not path.** Docs describe the *current* design/state; they do **not** narrate the
  sequence of fixes/commits that produced it (that's git). No "we first did X, then changed to Y".
  A milestone/task ledger is roadmap, not architecture; it belongs in `../TODO.md`.
- **Terse, invariant-first comments** (same rule in code): explain hidden constraints/contracts,
  not what the code plainly says.
- **Prose is regenerable, a measurement is not.** A stale prose record is **deleted**: anyone
  reading the code can regenerate it, and git holds the old text. A **measurement** cost bench time
  and some are permanently unreproducible (the Due unit is retired, no real STM32F103C8 exists, no
  micro:bit unit is recorded), so measurement captures are **archived to `archive/`**, never
  deleted. The test: could this be regenerated from the tree, or did it cost bench time?
- **Spikes are ephemeral.** A design *spike* is scratch that lets one pass explore a path for the
  next to implement. When the code lands, the spike is **deleted + squashed out of history**, and
  its durable teaching is rewritten as a **Book chapter** (spike -> Book, never a lingering doc).
- **Two sync contracts.** The **Reference** is code-synced: if a page and the code disagree, the
  code wins and the page is a bug. The **Book** is durable narrative: reviewed occasionally, not
  pinned to each diff.
- **ASCII only.** Plain ASCII in docs and code: `--` not em-dash, `->` not arrow, straight quotes,
  "section"/"microkernel" not the glyphs.
- **Code style** is `reference/style.md`: layout, language rules, corpus rules, and what earns a
  comment. There is no formatter.
