<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# How the project works

The rules for working on KickOS: how a change is tested, how a static gate lives and goes, how CI
and the bench are used, and how work is committed and closed. Kernel and board rules live in the
sibling pages, and this page links to them instead of repeating them. Where the maintainer's words
carry a rule, they are quoted with their date.

## Tests

- **The code under test is the production code** (maintainer, 2026-10-07). No test code lives in
  `kernel/` or `arch/`, and the selftest is an app on the production ABI. Fault injection lives in
  host suites that compile the real source file through a link seam. A counter's or a witness's
  claim goes to a unit suite or an emulator-level check, or the counter becomes a production
  observable that every build compiles. A probe call goes once a unit suite holds its claim, or
  its arm is rewritten on the production ABI.
- **Each static gate at its level** (maintainer, 2026-10-08). A gate that reads the code's shape
  to judge a result belongs in a unit test; one that reads it to judge a running behaviour belongs
  in an emulator or silicon run. A static gate stays only for what no run can witness, such as a
  fence or a placement.
- **Every deletion keeps the witness somewhere** (maintainer, 2026-10-03). An arm, a suite or a
  gate goes only once something else holds its claim.
- **A new fix's red arm extends an existing gate or is a unit test.** A new static checker is
  written only for a bug class likely to recur.
- **"An emulator that cannot test a security feature is a liability"** (maintainer, 2026-10-12).
  For the armv8a user-page guard, which has a software and a hardware form, see `porting.md`,
  *The kernel cannot reach a user page*.
- **No cache and no workaround before real use shows the cost** (maintainer, 2026-11-10). Prefer a smaller codebase and a
  lean algorithm; add a cache only when real use proves it matters.
- **No budget for our own bugs** (maintainer, 2026-11-10). Never size or harden code for a failure only a kernel bug can
  reach, such as a fault inside the fault reporter. That is the test suite's job, and a margin
  for it never ends (a third fault, an Nth).
- **Stress runs pin and hog the same cores, in parallel groups** (maintainer, 2026-11-10).

## Static gates

- **A gate proves it can fire, and refuses a corpus it could not read.** It starts from
  `../../tests/lib/gate.sh`, which holds the planted controls and the corpus walk once, so a gate
  carries only its rule and its planted file. The rules a gate holds over the source are marked
  **gated** in `style.md`.
- **A gate's replacement is proven once and keeps no test of its own** (maintainer, 2026-10-07).
  The commit that deletes a gate plants that gate's defect, shows the build refusing it with the
  error text, and reverts.
- **Every failure case of a deleted gate must fail its replacement** (maintainer, 2026-10-07):
  its clauses and its planted controls, not one planted defect.
- A flag or link mode that replaces a gate is set where the build carries it to every library;
  `architecture.md`, *Build system*, says where.

## CI

- **Each CI job installs only what it uses** (maintainer, 2026-11-04). Either each job's package
  list holds only the commands that job runs, or one job prepares the environment once and every
  other job restores it. Otherwise the install cost and the concurrent apt traffic grow with every
  job added, and a job sticks in its install against a throttled mirror.
- What CI builds and runs per ISA is in `boards.md`, *CI coverage & cross toolchains*.

## The bench

- How a capture is taken, and what it may claim, is `bench.md`.
- **`frdmk64f` is out of the fleet pass, by ruling, and runs on the local rig:**
  `../../tools/bench/bench-fleet.sh` runs it only when named.

## Commits and merges

- A commit message is a subject and a short list of what landed: `style.md`, *Comments*.
- No doc names a commit hash, and a spike leaves history once its code lands: `../README.md`,
  *Conventions*.

## Closing a milestone

- A closed `../../TODO.md` entry leaves the file at its milestone's merge, and git keeps it; the
  file's header says how to find it again.
