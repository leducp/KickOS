<!-- SPDX-License-Identifier: CECILL-C -->
# Audit records

- **`kickos-codebase-audit.html`** -- the codebase audit re-scored across M4.5.1: 83 findings
  with status, severity, area and the commit subjects that closed them, the source and
  linked-image weight tables, the next-steps ranking, and the silicon coverage boundary.
  Self-contained (inline CSS/JS, no external requests), with status, severity and area filters;
  open it in any browser.
- The ledger is point-in-time: scored at M4.5.1, it predates everything from M4.5.6 on, so the tree is the newer truth.

## Conventions

- **ASCII only**, as everywhere in `docs/`: typographic characters are written as entities
  (`&mdash;`, `&middot;`, `&Delta;`), which render as themselves.
- **Code style rules do not apply.** The no-ternary and spelled-logical-operator rules are for
  the C++ tree; the inline filter script is ordinary browser JavaScript.

Nothing in the build reads this directory. The five `file(GLOB)` calls in the tree match
`*.cc`, `*.S`, `board.cmake` and `mpu.cmake`; no target globs `.html`.
