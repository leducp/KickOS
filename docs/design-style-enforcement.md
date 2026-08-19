<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Style enforcement across code, docs and build files

> **Status: ACTIVE** -- because the M4.5.9 item asking for this pass is open and the mechanism is proposed for build.

## 1. The problem

Layout is written by hand now, `.clang-format` having been removed as a config the tree never
converged on; `reference/style.md` states it. What drifts is the set
of rules no formatter can see: an ASCII-only corpus, an SPDX header, a spelled logical operator, a
braced `case` arm, a guard macro instead of `#pragma once`, `set -u` in a gate script. Two fifths of
the files those rules cover are markdown, CMake, YAML and shell, which a C++ formatter does not read at
all. Re-measured on the tree this revision was written against, **seven of the eight bucket-1 rules
are at zero and one is not**: the `gate.sh` sourcing rule has five violations. So the mechanism holds
a zero on seven rules, which is what keeps those off the ratchet, and the eighth needs a decision
before it can gate (section 2, and Q6).

## 2. Rule inventory

Tiers: **C** = `.c .cc .h .S .ld`; **M** = `.md`; **B** = `CMakeLists.txt .cmake .yml`; **S** = `.sh .py`.
"Now" is the violation count over the four tiers, which is `git ls-files` filtered to those
extensions. Every figure below is re-derivable with the command in its Note; none of them is a
recorded number to be trusted, because all of them move.

### Bucket 1 -- decidable, zero judgment, not a proxy

| Rule | Tiers | Now | Note |
|---|---|---|---|
| No `#pragma once`; no `for (;;)`, `while (true)` instead | C | 0 | literal tokens; `while (true)` appears 90 times and `#pragma once` and `for (;;)` never |
| No byte outside 0x00-0x7F | C M B S | 0 | tree-wide the only non-ASCII file is `LICENSE`, already exempt in section 3. Subsumes em-dash, arrow, smart quote and glyph "section" into ONE rule |
| `SPDX-License-Identifier` within the first 5 lines | C M B S | 0 | tree-wide 8 files lack one: the six JSON presets, `.gitignore` and `tests/lib/panic.ere`, none of which has comment syntax |
| Copyright line beside the SPDX, per the template | C M B S | 0 | tree-wide the only gap is the same 8 comment-syntax-less files as the row above. See Q1 |
| Guard triple present, `#ifndef` and `#define` agree | C headers | 0 | over every `git ls-files '*.h'`; existence and self-consistency, not the spelling |
| No tab indent, no trailing whitespace, no CRLF, final newline present | C M B S | 0 | `UseTab: Never`, and `.S` holds it too |
| `set -u` in every `tests/*/check_*.sh` | S | 0 | every one of them has it. Tree-wide three `.sh` files do not, and all three are Q5's question: `tests/lib/gate.sh`, `tools/bench/rig.sh`, `tools/flash-common.sh` |
| Every `tests/*/check_*.sh` sources `tests/lib/gate.sh` | S | **5** | `tests/integration/check_sim_drvdeath.sh`, `check_sim_faultsurvive_pub.sh`, `check_sim_multi_instance.sh`, `check_sim_pubpanic.sh`, `check_sim_uartloop.sh`. The only bucket-1 rule NOT at zero, and the reason Q6 exists |

### Bucket 2 -- decidable only with real scanning

Naive false-positive counts are measured. Each rule needs strings, comments and preprocessor lines
classified first; two also need paren-depth tracking. The naive column is a raw `git grep`.

| Rule | Tiers | Naive regex | Why it fails |
|---|---|---|---|
| No ternary `?:` | C | 52 hits, **52 false** | every `?` in the tree sits in a string or a comment; the two that survive a crude string/comment filter are a `.ld` prose comment and the char literal `'?'` |
| Spelled `and` / `or` / `not`; `!=` stays | C B | 125 hits, **125 false** | in C, 102 are preprocessor conditions (`#if defined(X) && X`) and 9 are `T&&` rvalue refs; in B the remaining 14 are `&&` inside a shell command, which is sequencing and never a logical operator |
| Brace on every `if` / `for` / `while` body | C | every hit false | Allman puts the brace on the NEXT line, so any naive "brace on this line" regex fires on the whole tree, and a "next line is `{`" regex fires on every wrapped condition, which ends its first line in `)` |
| Brace on every `case` and `default` arm | C | every hit false | a "next line is `{`" regex fires on stacked `case 'd': case 'i': {`, which is one arm and not two |
| Guard spelling matches project prefix + path | C headers | n/a | `include/kickos/` is elided, and two seam headers deliberately share `KICKOS_ARCH_CONTEXT_H` |
| East `char const*`, west `volatile T` | C | n/a | needs type parsing; stated in `reference/style.md`, held by review |

### Bucket 3 -- not mechanically decidable, never gated

Comment narrates instead of stating a constraint; comment restates the code; comment justifies a
naming or design choice; a doc stating a path instead of a state; doc-tier placement (teaching ->
Book, contract -> Reference); a comment that is an invariant's only guard.

A doubled hyphen used as sentence punctuation was listed here and does not belong: it IS decidable
once flags, end-of-options separators, the decrement operator, banner runs and heredoc bodies are
excluded first, and `tests/static/check_dash_punct.sh` gates it over the non-markdown corpus.

## 3. Proposed mechanism

**One stdlib-Python file, a new `check_style.py`: a shared per-tier scanner plus one function per
rule.** The scanner is why this is one tool and not one script per rule. It classifies each line
region as code, comment or string (C/C++ `//`, `/* */`, literals, continuations; `#` and quotes for B and S; fenced and
inline spans for M), after which every bucket-2 rule is a few lines over classified text. Rules declare
their tiers; the corpus is `git ls-files`; findings print as `path:line: rule: message`, sorted.

| Invocation | Corpus | Rules | Exit | Runs where |
|---|---|---|---|---|
| a new `check_style.py` | whole tree | bucket 1 | non-zero on any finding | ctest gate `style`, beside `doc_names`; inside the existing sim CI job, so no new CI job |
| the same checker, `--advise [ref]` | files changed vs `ref`, default the merge base | buckets 1 and 2 | always 0 | a human, on the files being edited |

Bucket 1 gates the whole tree precisely because it is already at zero on seven of its eight rules: a
touched-files gate leaves a hole any rebase or bulk move walks through. The eighth rule ships only
once Q6 is answered, since a rule that lands red is exactly the baseline this design refuses.
Bucket 2 is advisory on the touched set only, which is how this supports sweep-on-touch rather than
replacing it: it tells whoever is already in the file what to fix on the way past, and cannot block a
third party over a heuristic.

It never rewrites a file (no `--fix`), owns no layout rule (Allman, indent, pointer
alignment, wrapping), keeps no baseline or count file, and takes no in-file suppression marker. Two
exemptions only, each named in the source with its reason, both unauthored: `LICENSE` (upstream text,
Latin-1 bytes, byte-exact) and `docs/archive/` (captures the archive convention forbids editing); an
authored file never gets one.

## 4. Why each alternative loses

| Alternative | Why it loses |
|---|---|
| `clang-format` gate | Decided against 2026-07-27 (`TODO.md`, CI hygiene). The recorded reason is the strong one: the checked-in config is a per-file starting point and **not a target state the tree converges to**, so the 144-of-289 divergence is the expected state, not drift; gating it would also restyle every kernel `extern "C"` block as a side effect of an `IndentExternBlock` value nobody chose. Reaching no markdown, CMake or YAML is the second reason; that reformatting buries real changes in large diffs is a third and weaker one. Not re-opened here: this tool never rewrites and checks no formatter-owned rule. |
| `uncrustify` | Same shape, plus a second config to keep in sync with `.clang-format`, the same C-family-only reach, and a version to pin on a CI image where `pip install` is unavailable. |
| One script per rule | One re-implementation of comment-and-string stripping per rule, and as many independent false-positive behaviours. The four binary-introspection gates in this same milestone are the measured cost: no `pipefail`, hand-placed landmarks, one whose own failure diagnostic was broken and never ran. `TODO.md` already rules out "another per-rule script". |
| Ratchet or count gate | Rejected twice on record, and rightly. A count measures a **proxy**: the `--` count falls only by repunctuation, which keeps the bad sentence and destroys the detector, so the gate reads the tree as clean while every bad sentence survives. A baseline additionally declares the current tree correct by construction. Nothing here counts: each bucket-1 rule is a direct yes/no on a literal fact. Seven of the eight are at zero already; the eighth is Q6, and the answer there is to fix the five scripts or scope the rule, never to baseline them. |
| Review-time human checklist only | The status quo, and it does not hold a zero: `docs/design-rp2350-mpu-armv8m.md` carried no SPDX header while every sibling record did, and re-measuring found it, not review. The same is true of this page's own numbers, which drifted in BOTH directions between writing and re-measuring. |

## 5. Left to humans

All of bucket 3, unmechanised. Three things carry it: sweep-on-touch, the per-milestone review, and the
standing rule that a comment which is an invariant's only guard is a **missing gate** -- write the test
and the judgment call becomes a bucket-1 fact elsewhere (`virt.ld` is the model). Bucket 2 is human too
in the sense that matters: advisory output, never a red build.

## 6. Budget and false-positive policy

About 300 lines in one file (~110 scanner, ~120 rules, ~70 driver, report), against the 496 of
`check_doc_names.sh`. Half a day. The cleanup it finds is NOT `set -u`, which is already everywhere
it is scoped to: it is the five scripts that do not source `tests/lib/gate.sh`, which is a real
conversion and not a one-line fix, so Q6 has to be answered before that rule can gate.

- **Precision over recall**, the rule `check_doc_names.sh` already states. A rule that cannot be
  made exact is written down here and kept out of the tool.
- **A bucket-1 rule that produces one false positive is demoted to bucket 2 in the same commit**,
  not patched with an exemption and not left gating.
- **No vacuous pass.** It fails loud on zero files read, an empty tier corpus, or a file count under a
  floor: the trap `require_nonempty` and `tool_out` exist for, and M4.5.9 root-caused in four gates.
- **A registered arm proving it still fails.** `--selftest` plants each bucket-1 violation in a
  fixture and asserts the matching rule fires; the only thing stopping a rule going vacuous.

## 7. Open questions

- **Q1. The copyright line. ANSWERED BY THE TREE, not by this document.** The option this question
  called "fix the files" has since been done: every markdown file in the tree carries a Copyright
  line. What survives of the question is narrow: the only remaining gaps tree-wide are the files with no comment syntax
  (the JSON presets, `.gitignore`, `tests/lib/panic.ere`), so the rule either states the four tiers
  as its corpus or names those file types as exempt. Either way it is now a zero-holding rule.
- **Q2. Spelled operators in `#if`.** `and` / `or` are valid preprocessor tokens, yet 102 lines of
  preprocessor condition in the C tier spell it `&&` and only two conditions spell it `and`
  (`user/apps/common/selftest/main.cc:27` and `:6361`). Exempt by intent, or drift?
- **Q3. Python.** Does no-ternary bind the S tier? `git ls-files '*.py' | xargs grep -ln ' if .* else '`
  is the live list, five files at the time of this edit.
- **Q4. Pre-commit hook.** Ship an opt-in hook under `tools/`? Developer-local, so it can only ever be
  a convenience on top of the ctest gate, never the enforcement point.
- **Q5. Scope of the `set -u` and `gate.sh` rules.** Proposed as `tests/*/check_*.sh` only, and at
  that scope `set -u` is already at zero. The three `.sh` files in the tree without it are exactly
  the out-of-scope ones: `tests/lib/gate.sh` and `tools/flash-common.sh` are sourced with no shebang,
  and `tools/bench/rig.sh` is a bench driver. Cover `tools/`, and does a sourced fragment count?
- **Q6. The five scripts that do not source `tests/lib/gate.sh`.** They are the sim integration
  gates listed in section 2, each of which builds its own image with its own CMake invocation rather
  than using the shared harness. Convert them (a real cleanup, which is what this mechanism otherwise
  refuses to do), narrow the rule to the gates that use the harness, or drop the rule? Until this is
  answered that row cannot gate, and it is the ONLY bucket-1 row in that position.
