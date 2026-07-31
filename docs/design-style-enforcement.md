<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Style enforcement across code, docs and build files -- ACTIVE

**ACTIVE**, because the M4.5.9 item asking for this pass is open and the mechanism is proposed for build.

## 1. The problem

Layout does not drift; `.clang-format` handles it per file as a starting point. What drifts is the set
of rules no formatter can see: an ASCII-only corpus, an SPDX header, a spelled logical operator, a
braced `case` arm, a guard macro instead of `#pragma once`, `set -u` in a gate script. Two thirds of
the files those rules cover are markdown, CMake, YAML and shell, which a C++ formatter does not read at
all. Measured at 2026-07-31, all are at **zero violations tree-wide** except `set -u`: the mechanism
holds a zero rather than cleaning one up, which is what keeps it off the ratchet.

## 2. Rule inventory

Tiers: **C** = `.c .cc .h .S .ld`; **M** = `.md`; **B** = `CMakeLists.txt .cmake .yml`; **S** = `.sh .py`.
"Now" is the violation count over 615 tracked files.

### Bucket 1 -- decidable, zero judgment, not a proxy

| Rule | Tiers | Now | Note |
|---|---|---|---|
| No `#pragma once`; no `for (;;)`, `while (true)` instead | C | 0 | literal tokens; 76 conforming loops |
| No byte outside 0x00-0x7F | C M B S | 2 files | subsumes em-dash, arrow, smart quote and glyph "section" into ONE rule |
| `SPDX-License-Identifier` within the first 5 lines | C M B S | 7 files | all 7 are JSON or `.gitignore`, which have no comment syntax |
| Copyright line beside the SPDX, per the template | C M B S | ~58 files | see Q1 |
| Guard triple present, `#ifndef` and `#define` agree | C headers | 0 | existence and self-consistency, not the spelling |
| No tab indent, no trailing whitespace, no CRLF, final newline present | C M B S | 0 | `UseTab: Never`, and `.S` holds it too |
| `set -u` in every `tests/check_*.sh` | S | **5** | the same scripts M4.5.9 flags for discarded exit status |
| Every `tests/check_*.sh` sources `tests/lib/gate.sh` | S | 0 | |

### Bucket 2 -- decidable only with real scanning

Naive false-positive counts are measured. Each rule needs strings, comments and preprocessor lines
classified first; two also need paren-depth tracking. The naive column is a raw `git grep`.

| Rule | Tiers | Naive regex | Why it fails |
|---|---|---|---|
| No ternary `?:` | C | 16 hits, **16 false** | every `?` in the tree sits in a string or a comment |
| Spelled `and` / `or` / `not`; `!=` stays | C B | 85 hits, **85 false** | 77 are `#if defined(X) && X`, 8 are `T&&` rvalue refs; in B, `&&` inside a shell command is sequencing, never a logical operator |
| Brace on every `if` / `for` / `while` body | C | 3 hits, **3 false** | a wrapped condition ends its first line in `)` |
| Brace on every `case` and `default` arm | C | 1 hit, **1 false** | stacked `case 'd': case 'i': {` is one arm, not two |
| Guard spelling matches project prefix + path | C headers | n/a | `include/kickos/` is elided, and two seam headers deliberately share `KICKOS_ARCH_CONTEXT_H` |
| East `char const*`, west `volatile T` | C | n/a | needs type parsing; `.clang-format` already records it as not auto-enforceable |

### Bucket 3 -- not mechanically decidable, never gated

Comment narrates instead of stating a constraint; comment restates the code; comment justifies a
naming or design choice; ` -- ` clause chains; a doc stating a path instead of a state; doc-tier
placement (teaching -> Book, contract -> Reference); a comment that is an invariant's only guard.

## 3. Proposed mechanism

**One stdlib-Python file, a new `check_style.py`: a shared per-tier scanner plus one function per
rule.** The scanner is why this is one tool and not thirteen. It classifies each line region as code,
comment or string (C/C++ `//`, `/* */`, literals, continuations; `#` and quotes for B and S; fenced and
inline spans for M), after which every bucket-2 rule is a few lines over classified text. Rules declare
their tiers; the corpus is `git ls-files`; findings print as `path:line: rule: message`, sorted.

| Invocation | Corpus | Rules | Exit | Runs where |
|---|---|---|---|---|
| a new `check_style.py` | whole tree | bucket 1 | non-zero on any finding | ctest gate `style`, beside `doc_names`; inside the existing sim CI job, so no new CI job |
| the same checker, `--advise [ref]` | files changed vs `ref`, default the merge base | buckets 1 and 2 | always 0 | a human, on the files being edited |

Bucket 1 gates the whole tree precisely because it is already at zero: a touched-files gate leaves a hole
any rebase or bulk move walks through. Bucket 2 is advisory on the touched set only, which is how this
supports sweep-on-touch rather than replacing it: it tells whoever is already in the file what to fix on
the way past, and cannot block a third party over a heuristic.

It never rewrites a file (no `--fix`), owns no rule `.clang-format` owns (Allman, indent, pointer
alignment, wrapping), keeps no baseline or count file, and takes no in-file suppression marker. Two
exemptions only, each named in the source with its reason, both unauthored: `LICENSE` (upstream text,
Latin-1 bytes, byte-exact) and `docs/archive/` (captures the archive convention forbids editing); an
authored file never gets one.

## 4. Why each alternative loses

| Alternative | Why it loses |
|---|---|
| `clang-format` gate | Decided against 2026-07-27 (`TODO.md`, CI hygiene). The recorded reason is the strong one: the checked-in config is a per-file starting point and **not a target state the tree converges to**, so the 144-of-289 divergence is the expected state, not drift; gating it would also restyle every kernel `extern "C"` block as a side effect of an `IndentExternBlock` value nobody chose. Reaching no markdown, CMake or YAML is the second reason; that reformatting buries real changes in large diffs is a third and weaker one. Not re-opened here: this tool never rewrites and checks no formatter-owned rule. |
| `uncrustify` | Same shape, plus a second config to keep in sync with `.clang-format`, the same C-family-only reach, and a version to pin on a CI image where `pip install` is unavailable. |
| One script per rule | Thirteen re-implementations of comment-and-string stripping, thirteen false-positive behaviours. The four binary-introspection gates in this same milestone are the measured cost: no `pipefail`, hand-placed landmarks, one whose own failure diagnostic was broken and never ran. `TODO.md` already rules out "another per-rule script". |
| Ratchet or count gate | Rejected twice on record, and rightly. A count measures a **proxy**: the `--` count falls only by repunctuation, which keeps the bad sentence and destroys the detector, so the gate reads the tree as clean while every bad sentence survives. A baseline additionally declares the current tree correct by construction. Nothing here counts: each bucket-1 rule is a direct yes/no on a literal fact, and its target is zero because the tree is already there. |
| Review-time human checklist only | The status quo, and it does not hold a zero: `docs/design-rp2350-mpu-armv8m.md` carried no SPDX header while all 26 sibling records did, and re-measuring found it, not review. |

## 5. Left to humans

All of bucket 3, unmechanised. Three things carry it: sweep-on-touch, the per-milestone review, and the
standing rule that a comment which is an invariant's only guard is a **missing gate** -- write the test
and the judgment call becomes a bucket-1 fact elsewhere (`virt.ld` is the model). Bucket 2 is human too
in the sense that matters: advisory output, never a red build.

## 6. Budget and false-positive policy

About 300 lines in one file (~110 scanner, ~120 rules, ~70 driver, report), against the 322 of
`check_doc_names.sh`. Half a day, plus the five `set -u` fixes it finds.

- **Precision over recall**, the rule `check_doc_names.sh` already states. A rule that cannot be
  made exact is written down here and kept out of the tool.
- **A bucket-1 rule that produces one false positive is demoted to bucket 2 in the same commit**,
  not patched with an exemption and not left gating.
- **No vacuous pass.** It fails loud on zero files read, an empty tier corpus, or a file count under a
  floor: the trap `require_nonempty` and `tool_out` exist for, and M4.5.9 root-caused in four gates.
- **A registered arm proving it still fails.** `--selftest` plants each bucket-1 violation in a
  fixture and asserts the matching rule fires; the only thing stopping a rule going vacuous.

## 7. Open questions

- **Q1. The copyright line.** The template requires it beside every SPDX identifier, yet only 17 of 74
  markdown files carry one (~58 tree-wide lack it). Fix 58 files, gate the C/B/S tiers only, or amend
  the template? It stays out of the tool until answered.
- **Q2. Spelled operators in `#if`.** `and` / `or` are valid preprocessor tokens, yet all 77 such
  conditions use `&&`. Exempt by intent, or drift?
- **Q3. Python.** Does no-ternary bind the S tier? Three files use `a if c else b` today.
- **Q4. Pre-commit hook.** Ship an opt-in hook under `tools/`? Developer-local, so it can only ever be
  a convenience on top of the ctest gate, never the enforcement point.
- **Q5. Scope of the `set -u` and `gate.sh` rules.** Proposed as `tests/check_*.sh` only: `gate.sh` and
  `tools/flash-common.sh` are sourced with no shebang, and `tools/flash-*` are bash. Cover `tools/`?
