#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate against doc rot: every KICKOS_* / KOS_* / CAP_* / AUTH_* identifier and
# every in-repo file path a markdown file NAMES must still resolve in the tree.
#
# The failure this exists to stop is not the dangling reference, it is the reference
# that resolves to the WRONG thing: syscall numbers 34/35/36 have already been reused
# in this ABI, so a stale note naming a number now owned by a different syscall reads
# as authoritative and costs debugging time. A name nobody can find is merely useless.
#
# Run from the repo root, no arguments, no build directory:
#   tests/check_doc_names.sh
#
# EXTRACTION RULE (precision over recall -- a checker that cries wolf gets disabled):
#
#   corpus       every tracked *.md, discovered via `git ls-files` -- nothing is
#                hardcoded, so moving a file into docs/archive/ neither hides it from
#                the gate nor breaks the gate. Read with `grep -a`: M1_raw_meas.md
#                holds raw serial captures with NUL bytes, and a checker that lets
#                grep skip a "binary" file passes vacuously.
#
#   fences       lines inside a ``` fence are SKIPPED. Design docs fence PROPOSED
#                code and capture files fence device output; neither names the tree.
#                Unbalanced fences are themselves reported -- the state machine's
#                correctness depends on them, so it may not assume them.
#
#   identifiers  matched anywhere on a non-fenced line, backticked or not. Backticks
#                are NOT required here (they are for paths): a KICKOS_/KOS_/CAP_/AUTH_
#                token is never English prose, so demanding them would only lose recall.
#                A token whose spelling is not in the tree but whose UPPERCASED form is
#                gets the sharper message -- an ungreppable mis-spelling, not a dead
#                name. Dropped:
#                  - a trailing `*` or `_` (KOS_E*, KOS_SYS_SEM_*): a wildcard names
#                    a family, not a symbol.
#                  - a final `_`-separated component of ONE character: metasyntactic
#                    placeholders (CAP_X, KICKOS_MAX_X) and include guards
#                    (KICKOS_SYS_ABI_H). Neither is a symbol reference; the reference
#                    that matters for a guard is the header path, checked below.
#                  - a token that is a real symbol plus an English plural `s`.
#
#   paths        matched anywhere on a non-fenced line, but only where the shape is
#                unambiguous, because `ldrex/strex` and `PA4/PA5` are not paths.
#                Supported forms: bare path, trailing-slash directory, `path:N`,
#                `path:N-M`, `path:N,M,K`, and `../`-relative links between docs
#                (resolved against the doc's own directory, then against the repo
#                root). A candidate is dropped unless its first component is a
#                tracked top-level entry, or its doc-relative parent directory is a
#                real tracked directory. That is what keeps `/dev/ttyUSB0`,
#                `build/...`, `.session/logs/...` and register groups out.
#
# THEREFORE NOT CAUGHT. Know these before trusting a green run:
#   - the `:N` in `path:N` is STRIPPED, never verified. The corpus carries ~270 such
#     citations and they rot silently and badly: `abi.h:36` was written when line 36
#     was the clock syscall and now lands on an unrelated one. Verifying a line
#     number needs the doc to say WHAT it expects to find there; nothing here can.
#   - a directory reference with NO trailing slash (`kernel/domain`), because it is
#     shape-identical to the prose alternations this corpus uses constantly
#     (`kernel/app` split, `user/kernel` boundary, `arch/chip` seam). Write directory
#     references with a trailing slash and they get checked.
#   - a single-component reference (`M1_state.md` naked in a root-level doc), because
#     the corpus also says `main.cc` and `switch.S` as bare shorthand for a dozen files.
#   - names inside fenced code; `<kickos/sys/x.h>` include-form references; anything
#     spelled with a glob or a <placeholder>; paths whose first component is not a
#     tracked top-level entry (`esp32c6/mpu.cmake`).
#   - lowercase wrapper names (`kos_send`, `arch_mpu_apply`): out of the named families.
#   - a name that survives ONLY in a stale source comment still resolves, so this gate
#     catches a DELETED symbol, not a half-completed rename.
#
# Exactly one path is exempt: docs/design/retracted.md, which exists to hold
# retractions whose reasoning is the point. Nothing else is exempt by design -- if a
# clean run seems to need another exemption, that is a finding about the corpus.

set -u
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

fail() { echo "FAIL: $1" >&2; exit 1; }

[ -d .git ] || [ -f .git ] || fail "must run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found"

TMP="$(mktemp -d)" || fail "mktemp failed"
trap 'rm -rf "$TMP"' EXIT

EXEMPT="docs/design/retracted.md"

# --- corpus -------------------------------------------------------------------
git ls-files -z '*.md' | tr '\0' '\n' | grep -v "^${EXEMPT}\$" > "$TMP/docs.txt"
DOCS=$(wc -l < "$TMP/docs.txt" | tr -d ' ')
[ "$DOCS" -gt 0 ] || fail "no tracked *.md found -- wrong directory? (gate would pass vacuously)"

# --- valid identifier set, scanned from the tree ------------------------------
# Every such token appearing in a tracked NON-markdown file: CMake option()/set()/
# add_compile_definitions, #define, enum members, board_config.h overrides, linker
# scripts and presets all land here without the gate needing to know their syntax.
# This script is excluded from its own scan: its comments quote mis-spellings and dead
# names as EXAMPLES, and once it is tracked those examples would enter the valid set and
# mask the very findings they describe. (Measured: it masked two.)
git ls-files -z | tr '\0' '\n' | grep -v '\.md$' | grep -v '^tests/check_doc_names\.sh$' > "$TMP/src.txt"
[ -s "$TMP/src.txt" ] || fail "no tracked non-markdown sources -- cannot build the valid identifier set"
# The alphabet here MUST match the one the doc scan below uses, lowercase included:
# a source-side scan that stopped at the first lowercase letter would put `KOS_E` in
# the valid set and then report the tree's own `KOS_Exxx` metasyntax as dangling.
tr '\n' '\0' < "$TMP/src.txt" | xargs -0 grep -aohE '(KICKOS|KOS|CAP|AUTH)_[A-Za-z0-9_]*[A-Za-z0-9]' 2>/dev/null \
  | sort -u > "$TMP/valid_ids.txt"
IDS=$(wc -l < "$TMP/valid_ids.txt" | tr -d ' ')
[ "$IDS" -gt 100 ] || fail "only $IDS identifiers collected from the tree -- scan is broken, findings would be noise"

# --- valid path set: tracked files plus every ancestor directory ---------------
# Built from git, never from the filesystem, so an untracked build/ or .session/
# can never make a stale reference resolve.
git ls-files -z | tr '\0' '\n' > "$TMP/tracked.txt"
awk '{ print; n = split($0, c, "/"); p = ""; for (i = 1; i < n; i++) { p = p c[i]; print p; p = p "/" } }' \
  "$TMP/tracked.txt" | sort -u > "$TMP/valid_paths.txt"
awk -F/ '{ print $1 }' "$TMP/tracked.txt" | sort -u > "$TMP/toplevel.txt"

# Extensions the tree actually uses. Derived, not listed, so a doc naming a BUILD
# ARTIFACT (.hex/.uf2/.elf/.log) or a linker section (.bss/.eh_frame) is never
# mistaken for a source reference -- no such extension is tracked.
sed 's|.*/||' "$TMP/tracked.txt" | grep '\.' | sed 's|.*\.||' | sort -u > "$TMP/exts.txt"
[ -s "$TMP/exts.txt" ] || fail "derived no file extensions from the tree -- path shape rule is broken"

# --- syscall numbers, from the ABI header itself ------------------------------
ABI="user/include/kickos/sys/abi.h"
[ -f "$ABI" ] || fail "$ABI not found -- cannot cross-check syscall numbers"
sed -n 's/^ *\(KOS_SYS_[A-Z0-9_]*\) *= *\([0-9][0-9]*\).*/\1 \2/p' "$ABI" > "$TMP/sysnum.txt"
[ -s "$TMP/sysnum.txt" ] || fail "parsed zero syscall numbers out of $ABI -- number cross-check is broken"

# =============================================================================
# One pass over the corpus. Output is file-ordered then line-ordered, so the
# report is byte-identical across runs given the same tree.
# =============================================================================
tr '\n' '\0' < "$TMP/docs.txt" | xargs -0 grep -an '' /dev/null 2>/dev/null \
  | awk -v T="$TMP" -v ABIH="$ABI" -F: '
function load(f, arr,   l) { while ((getline l < f) > 0) { arr[l] = 1 } close(f) }

# Collapse "a/b/../c" and "a/./b". Leading ".." that escapes the root is left in
# place, which makes the path unresolvable -- correct, it is outside the repo.
function norm(p,   n, c, i, out, top) {
  gsub(/\/\/+/, "/", p)
  n = split(p, c, "/")
  top = 0
  for (i = 1; i <= n; i++) {
    if (c[i] == "" || c[i] == ".") { continue }
    if (c[i] == ".." && top > 0 && out[top] != "..") { top--; continue }
    out[++top] = c[i]
  }
  p = ""
  for (i = 1; i <= top; i++) {
    if (i > 1) { p = p "/" }
    p = p out[i]
  }
  return p
}

function report(f, l, msg) { printf "%s:%d: %s\n", f, l, msg; findings++ }

BEGIN {
  load(T "/valid_ids.txt",   VALID_ID)
  load(T "/valid_paths.txt", VALID_PATH)
  load(T "/toplevel.txt",    TOP)
  load(T "/exts.txt",        EXT)
  while ((getline l < (T "/sysnum.txt")) > 0) { split(l, a, " "); SYSNUM[a[1]] = a[2] }
  close(T "/sysnum.txt")
  findings = 0
}

# grep -an prints "<file>:<lineno>:<text>"; -F: splits it, but the text may hold
# colons, so rebuild it from field 3 onward.
{
  file = $1; lineno = $2 + 0
  text = $3
  for (i = 4; i <= NF; i++) { text = text ":" $i }

  if (file != prevfile) {
    if (prevfile != "" && infence) { report(prevfile, fenceline, "unbalanced ``` fence opened here and never closed -- extraction cannot trust this file") }
    prevfile = file; infence = 0; fenceline = 0
    n = split(file, fc, "/"); dir = ""
    for (i = 1; i < n; i++) {
      if (i > 1) { dir = dir "/" }
      dir = dir fc[i]
    }
  }

  if (text ~ /^ *```/) { if (infence) { infence = 0 } else { infence = 1; fenceline = lineno }; next }
  if (infence) { next }

  # ---- identifiers ----------------------------------------------------------
  # Lowercase is admitted after the prefix ON PURPOSE: `KOS_SYS_cpu_clock_hz` is not a
  # symbol, it is an ungreppable mis-spelling of one, and the enum is exactly where a
  # mis-spelling is expensive.
  rest = text
  while (match(rest, /(KICKOS|KOS|CAP|AUTH)_[A-Za-z0-9_]*[A-Za-z0-9]/)) {
    tok  = substr(rest, RSTART, RLENGTH)
    tail = substr(rest, RSTART + RLENGTH, 1)
    rest = substr(rest, RSTART + RLENGTH)
    if (tail == "*" || tail == "_") { continue }                # wildcard family / bare prefix
    if (tok ~ /_[A-Za-z0-9]$/) { continue }                     # placeholder (CAP_X) / include guard (..._H)

    singular = tok
    sub(/s$/, "", singular)
    if (singular in VALID_ID) { continue }                      # English plural of a real symbol

    name = tok
    if (!(tok in VALID_ID)) {
      up = toupper(tok)
      if (!(up in VALID_ID)) {
        report(file, lineno, "identifier does not exist anywhere in the tree: " tok)
        continue
      }
      report(file, lineno, "identifier is mis-cased and cannot be grepped: " tok " -- the tree spells it " up)
      name = up
    }

    # A KOS_SYS_* name spelled next to a number is the dangerous case: these numbers
    # have been REUSED (34/35/36), so a stale pair names a live but unrelated syscall.
    # Check the pair, never just the name.
    if (name in SYSNUM) {
      near = substr(rest, 1, 24)
      if (match(near, /^[` ]*=[ ]*[0-9]+/) || match(near, /^[` ]*\([ ]*[0-9]+[ ]*\)/)) {
        claim = substr(near, RSTART, RLENGTH); gsub(/[^0-9]/, "", claim)
        if (claim != SYSNUM[name]) {
          report(file, lineno, "syscall number disagrees with " ABIH ": doc says " name " = " claim ", abi.h says " SYSNUM[name])
        }
      }
    }
  }

  # ---- paths ---------------------------------------------------------------
  # Split on every character that delimits a token in markdown prose, code spans
  # and link targets, then judge each candidate on shape alone. NOT on < > : an
  # `arch/<arch>/include/.../context.h` split there yields a plausible-looking path
  # that was never claimed to exist; kept whole, the placeholder rule below drops it.
  line = text
  gsub(/[`()\[\]"'"'"'|;, \t]+/, "\n", line)
  m = split(line, cand, "\n")
  for (i = 1; i <= m; i++) {
    p = cand[i]
    if (index(p, "/") == 0) { continue }
    if (index(p, "://") > 0) { continue }                      # URL
    sub(/[.,:;!?*]+$/, "", p)                                  # prose punctuation
    sub(/:[0-9][0-9,-]*$/, "", p)                              # :N / :N-M / :N,M,K
    sub(/#.*$/, "", p)                                         # link anchor
    if (p == "" || index(p, "/") == 0) { continue }
    if (p ~ /^\//) { continue }                                # absolute: not a repo path
    if (p ~ /[*?{}<>$=%!@^~]/) { continue }                    # glob or <placeholder>
    if (p ~ /\.\.\./) { continue }                             # elision
    if (p ~ /:/) { continue }                                  # C++ scope, drive spec

    slash = (p ~ /\/$/); sub(/\/+$/, "", p)
    if (p == "") { continue }
    if (index(p, "/") == 0) { continue }   # one component: see the leaf rule below

    # A path must be SHAPED like one: a tree-known extension on the last component,
    # or an explicit trailing slash for a directory. This is the whole precision
    # story. Without it, the prose alternations this corpus uses constantly -- the
    # kernel/app split, the user/kernel boundary, the arch/chip seam, ldrex/strex,
    # PA4/PA5, SIM_SCGC4/5 -- are shape-identical to directory references and every
    # one of them reports. The cost is stated in the header: write a directory
    # reference WITH a trailing slash or the gate will not check it.
    if (!slash) {
      leaf = p; sub(/^.*\//, "", leaf)
      if (index(leaf, ".") == 0) { continue }
      ext = leaf; sub(/^.*\./, "", ext)
      if (!(ext in EXT)) { continue }
    }

    r1 = norm(p)
    r2 = norm(dir "/" p)
    if (r1 in VALID_PATH || r2 in VALID_PATH) { continue }

    # Only judge candidates that plausibly MEAN this repo: rooted at a tracked
    # top-level entry, or landing in a directory that really exists once resolved
    # against the doc. `../../roadmap.md` resolves to the ROOT, whose parent is the
    # empty string -- and the root always exists, so it counts as plausible.
    split(r1, t1, "/")
    r1parent = r1; if (!sub(/\/[^\/]*$/, "", r1parent)) { r1parent = "" }
    r2parent = r2; if (!sub(/\/[^\/]*$/, "", r2parent)) { r2parent = "" }
    havedir1 = (r1parent == "" || r1parent in VALID_PATH)
    havedir2 = (r2parent == "" || r2parent in VALID_PATH)
    if (!(t1[1] in TOP) && !havedir2) { continue }

    kind = "file"; if (slash) { kind = "directory" }
    hint = ""
    if (!havedir1 && !havedir2) {
      hint = " (its parent directory does not exist either -- an out-of-tree citation?)"
    }
    report(file, lineno, kind " path does not exist: " p hint)
  }
}

END {
  if (prevfile != "" && infence) { report(prevfile, fenceline, "unbalanced ``` fence opened here and never closed -- extraction cannot trust this file") }
  exit (findings > 0)
}' > "$TMP/findings.txt"
RC=$?

echo "== checked $DOCS doc file(s) against $IDS tree identifier(s) and $(wc -l < "$TMP/valid_paths.txt" | tr -d ' ') tracked path(s) =="

if [ "$RC" -ne 0 ]; then
  cat "$TMP/findings.txt" >&2
  echo "" >&2
  echo "per-file finding count:" >&2
  cut -d: -f1 "$TMP/findings.txt" | sort | uniq -c | sort -rn >&2
  echo "" >&2
  echo "FAIL: $(wc -l < "$TMP/findings.txt" | tr -d ' ') unresolved doc reference(s)." >&2
  echo "      Fix the doc, or delete the claim. Widening this gate is not the fix." >&2
  exit 1
fi

echo "PASS: every named identifier and in-repo path resolves"
