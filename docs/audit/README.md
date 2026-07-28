<!-- SPDX-License-Identifier: CECILL-C -->
# Audit records

- **`kickos-codebase-audit.html`** -- the codebase audit re-scored across M4.5.1: 83 findings
  with status, severity, area and the commit subjects that closed them, the source and
  linked-image weight tables, the next-steps ranking, and the silicon coverage boundary.
  Self-contained (inline CSS/JS, no external requests); open it in any browser. The status,
  severity and area filters from the original canvas are preserved.

## History: this was a Cursor canvas

The audit began life as a `.canvas.tsx` Cursor canvas, which the IDE renders only from its own
workspace `canvases/` directory -- so the repository carried a committed mirror and the live
copy sat outside the tree, with a byte-identity `diff` as the staleness check. That split was
the sole reason for the mirror convention, and it is gone: the HTML file above IS the record,
edited in place like any other document here. The `.canvas.tsx` history up to the migration
commit is still in git.

The retired live canvas under the Cursor workspace should be deleted rather than kept -- a
renderable stale copy is exactly the drift this audit spends several findings hunting.

## Conventions

- **ASCII holds again.** The mirror was exempt from the docs ASCII rule to preserve
  byte-identity with the live file. The HTML file is pure ASCII: typographic characters are
  written as entities (`&mdash;`, `&middot;`, `&Delta;`), which render as themselves.
- **Code style rules do not apply.** The no-ternary and spelled-logical-operator rules are for
  the C++ tree; the inline filter script is ordinary browser JavaScript.

Nothing in the build reads this directory. The five `file(GLOB)` calls in the tree match
`*.cc`, `*.S`, `board.cmake` and `mpu.cmake`; no target globs `.html`.
