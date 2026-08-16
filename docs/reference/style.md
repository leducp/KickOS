<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# House style

The rules a change must follow. **There is no formatter.** `.clang-format` was removed: the tree
never converged on it (144 of 289 C++ files diverged), so it described no state the code held, and
a config that disagrees with the code misleads a reader into "fixing" conforming files. Layout here
is written by hand and reviewed by eye until a checker exists; `../design-style-enforcement.md`
proposes one, and a shared tool would serve `../../../KickCAT` and `../../../kickmsg` too.

Rules marked **gated** are checked by something in `tests/static/`. The rest are not: they hold
because they are written here and read in review.

## Layout

| | |
| --- | --- |
| indent | 4 spaces, never a tab |
| braces | Allman: every opening brace on its own line |
| one-liners | none. Every `if`, `else`, `for`, `while` and `case` body is braced, including a single statement |
| line length | no hard limit; wrap by hand where it reads better |
| namespaces | indented, and spelled `namespace a::b`, not nested blocks |
| namespace close | a bare `}`. No `// namespace x` comment: it says nothing and goes stale |
| `case` labels | indented one level inside the `switch` |
| access specifiers | outdented one level |
| pointers | left-aligned: `char* p` |
| qualifiers | east const (`char const*`), west volatile (`volatile T x`) |
| ctor init lists | leading comma |
| includes | not sorted; grouped by hand |

## Language

- **No ternary `?:`.** Use `if`/`else`, an early return, or a variable set in a branch. This holds
  for plural selection too: set a `char const*` in an `if`.
- **Spelled logical operators**: `and`, `or`, `not`. `!=` stays.
- **`while (true)`**, never `for (;;)`. **gated**
- **Traditional include guards**, never `#pragma once`. The macro derives from the project prefix
  plus the file path. **gated**
- **Fixed-width C99 types**: `uint8_t`, `int32_t`, `size_t`. Avoid `long`, `short` and bare
  `unsigned` except where a foreign ABI dictates them, and say so at the declaration when it does.
- **A header holds what must be a header**: templates, `constexpr`, and declarations. A
  non-template function body goes in a `.cc` -- `user/src/` for the user substrate -- so the
  tree carries one definition rather than a copy per including TU for the linker to fold.
- **`volatile` is not a concurrency tool.** The tree uses it for cross-thread fields and
  that holds only on a uniprocessor; it is recorded as an M5 correctness fix in
  `../design-m5-smp.md`. Match the surrounding code rather than mixing idioms in one
  struct, and do not read it as making an access atomic or ordered.
- **Check a return** that can fail. A discarded status is how a correct refusal becomes a silent
  hang; `(void)` it only where the value carries nothing, and say why.

## Corpus

- **ASCII only**, in every tracked file. `--` not an em dash, `->` not an arrow, straight quotes,
  "section" spelled out. **gated**
- **SPDX header** within the first five lines, with the copyright line beside it. **gated**
- No trailing whitespace, no CRLF, a final newline.
- `set -u` in a gate script.

## Comments

A comment earns its place by warning of something a reader would otherwise undo: a hidden
constraint, a subtle invariant, a specific workaround, behaviour that would surprise. It does not
restate the code, explain a naming or wrapper choice, or recount how the code came to be that way.

- **No narration.** No dates, no "measured on", no "this used to", no war stories. Git holds that.
- **No ` -- ` in a code comment.** It is a docs separator; in code, use a comma, a colon or a
  second sentence.
- Prefer one line. Length is not the metric, but a paragraph should be carrying a constraint that
  needs one.

The same rule governs commit messages: a subject plus a bullet changelist saying WHAT landed. Never
stats, test results, or how a defect was found.

## Docs

`../README.md` holds the documentation conventions: which tier a page belongs to, the code-synced
contract for this one, and *prose is regenerable, a measurement is not*.
