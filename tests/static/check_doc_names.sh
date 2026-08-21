#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate against doc rot: every KICKOS_* / KOS_* / CAP_* / AUTH_* identifier and
# every in-repo file path a markdown file NAMES must still resolve in the tree.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_doc_names.sh
#
# EXTRACTION RULE (precision over recall: a checker that cries wolf gets disabled):
#
#   corpus       every tracked *.md, discovered via `git ls-files`. Nothing is
#                hardcoded, so moving a file into docs/archive/ neither hides it from
#                the gate nor breaks the gate. Read with `grep -a`: M1_raw_meas.md
#                holds raw serial captures with NUL bytes, and a checker that lets
#                grep skip a "binary" file passes vacuously.
#
#   fences       lines inside a ``` fence are SKIPPED. Design docs fence PROPOSED
#                code and capture files fence device output; neither names the tree.
#                Unbalanced fences are themselves reported: the state machine's
#                correctness depends on them, so it may not assume them.
#
#   identifiers  matched anywhere on a non-fenced line, backticked or not. Backticks
#                are NOT required here (they are for paths): a KICKOS_/KOS_/CAP_/AUTH_
#                token is never English prose, so demanding them would only lose recall.
#                A token whose spelling is not in the tree but whose UPPERCASED form is
#                gets the sharper message: an ungreppable mis-spelling, not a dead
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
# What a green run states:
#   - the PATH resolves; the `:N` in `path:N` is stripped, never verified. A citation into
#     the syscall enum of user/include/kickos/sys/abi.h keeps resolving after the enum has
#     moved down the file, and lands on unrelated prose. Verifying a line number needs the
#     doc to say WHAT it expects to find there. Do not pin this example to a line.
#   - a directory reference is checked when it carries a TRAILING SLASH. Without one it is
#     shape-identical to the prose alternations this corpus uses constantly (`kernel/app`
#     split, `user/kernel` boundary, `arch/chip` seam), so it is left alone.
#   - a reference needs two components: the corpus says `main.cc` and `switch.S` as bare
#     shorthand for a dozen files, so a lone `M1_state.md` is prose here.
#   - the scan reads unfenced prose, plain path shapes and the four named identifier
#     families. Fenced code, `<kickos/sys/x.h>` include forms, globs and <placeholder>
#     spellings, a first component that is not a tracked top-level entry
#     (`esp32c6/mpu.cmake`), and lowercase wrapper names (`kos_send`, `arch_mpu_apply`)
#     are all outside it.
#   - the valid set comes from CODE with comments stripped, so a name kept alive only by a
#     stale comment reads as dangling: a half-completed rename is caught, not just a
#     deleted symbol. A file whose extension the stripper does not recognise is the one
#     place a commented-out name still validates.
#
# NOTHING is exempt. If a clean run seems to need an exemption, that is a finding about
# the corpus: a proposed name belongs in a fenced listing, and a claim about a name that
# no longer exists can be written without spelling it.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

[ -d .git ] || [ -f .git ] || fail "must run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found"

scratch_dir

# --- corpus -------------------------------------------------------------------
git ls-files -z '*.md' | tr '\0' '\n' > "$TMP/docs.txt"
DOCS=$(wc -l < "$TMP/docs.txt" | tr -d ' ')
[ "$DOCS" -gt 0 ] || fail "no tracked *.md found: wrong directory? (gate would pass vacuously)"

# --- valid identifier set, scanned from the tree ------------------------------
# Every such token appearing in a tracked NON-markdown file: CMake option()/set()/
# add_compile_definitions, #define, enum members, board_config.h overrides, linker
# scripts and presets all land here without the gate needing to know their syntax.
# This script is excluded from its own scan: its comments quote mis-spellings and dead
# names as EXAMPLES, and once it is tracked those examples would enter the valid set and
# mask the very findings they describe.
# docs/ is excluded for the same reason and it is NOT redundant with the *.md filter:
# a DOCUMENT tracked under a non-markdown extension behaves as a source here, so every
# name it records would stay valid to this gate forever after the build dropped it, and
# the next .html, .svg or .json committed under docs/ would do exactly that. The corpus
# being CHECKED is unaffected: docs/*.md is still every bit of it.
git ls-files -z | tr '\0' '\n' | grep -v '\.md$' | grep -v '^docs/' | grep -v '^tests/static/check_doc_names\.sh$' > "$TMP/src.txt"
[ -s "$TMP/src.txt" ] || fail "no tracked non-markdown sources: cannot build the valid identifier set"
# The alphabet here MUST match the one the doc scan below uses, lowercase included:
# a source-side scan that stopped at the first lowercase letter would put `KOS_E` in
# the valid set and then report the tree's own `KOS_Exxx` metasyntax as dangling.
#
# The left boundary is load-bearing. Without it `grep -o` cuts a CAP_ prefixed tail out of
# a KCAP_ prefixed name, so 26 identifiers that exist nowhere enter the valid set as
# substrings and a doc may drop the K from any KCAP_ name and pass. KCAP is listed
# explicitly because with the boundary and without it here, those names match on neither
# side and go unchecked.
# Comments do not confer validity. A name mentioned only in prose is a name nothing builds,
# and admitting one lets a removed knob validate itself in every doc. No run can show that
# happening, because the masking IS the green.
#
# Type-aware, because the marker is not: `#` opens a comment in sh, CMake and Kconfig, and
# opens the preprocessor in C where `#define` is the definition site. An unrecognised
# extension passes through unstripped.
#
# A `#` inside a shell string takes the rest of that line with it, so a name living only
# there loses validity. A name in dead code is code, not prose, and still counts.
# The program goes to a FILE and the harvest to a function, so the self-test below runs the
# SAME scan the corpus does.
cat > "$TMP/harvest.awk" <<'AWK'
FNR == 1 {
    print FILENAME >> SEEN
    inblk = 0
    ctype = 0
    if (FILENAME ~ /\.(c|cc|h|hpp|S|ld|lds)$/)                              { ctype = 1 }
    else if (FILENAME ~ /\.(sh|py|cmake|txt|json|yml|yaml|cfg|conf|ere)$/)  { ctype = 2 }
    else if (FILENAME ~ /Kconfig/)                                          { ctype = 2 }
}
{
    line = $0
    if (ctype == 1) {
        out = ""
        while (length(line) > 0) {
            if (inblk) {
                i = index(line, "*/")
                if (i == 0) { line = ""; break }
                line = substr(line, i + 2)
                inblk = 0
            } else {
                b = index(line, "/*")
                l = index(line, "//")
                # A `//` reached FIRST consumes the rest of the line, so a `/*` after it
                # opens nothing. Strip `//` after the block scan instead and a path glob in
                # a line comment opens a block that never closes, dropping every identifier
                # below it from the valid set while the gate still passes.
                if (l > 0 && (b == 0 || l < b)) {
                    out = out substr(line, 1, l - 1)
                    line = ""
                    break
                }
                if (b == 0) { out = out line; line = ""; break }
                out = out substr(line, 1, b - 1)
                line = substr(line, b + 2)
                inblk = 1
            }
        }
        line = out
    } else if (ctype == 2) {
        sub(/#.*/, "", line)
    }
    print line
}
AWK

# The one alphabet, as one ERE, so the self-test and the corpus scan cannot disagree.
ID_ERE='\b(KICKOS|KOS|KCAP|CAP|AUTH)_[A-Za-z0-9_]*[A-Za-z0-9]'

# Refuses by name instead of reading less. xargs splits the list across SEVERAL awk
# invocations, and awk exits FATALLY on a file it cannot open, dropping every remaining file
# of THAT batch; the pipeline status comes from `sort`, so the loss reads as a smaller tree.
# The usual way in is a path `git ls-files` reports whose file is gone: a rename whose
# deletion is not staged yet.
# A return, not a fail(), so the self-test can assert the refusal.
harvest_ids() { # <file-list> <outfile>; 0 ok, 1 a member went unread (named in $TMP/unread)
    : > "$TMP/unread"
    : > "$TMP/seen"
    : > "$TMP/harvest.err"
    while IFS= read -r _f; do
        [ -r "$_f" ] || printf '%s\n' "$_f" >> "$TMP/unread"
    done < "$1"
    if [ ! -s "$TMP/unread" ]; then
        tr '\n' '\0' < "$1" | xargs -0 awk -v SEEN="$TMP/seen" -f "$TMP/harvest.awk" \
            2>"$TMP/harvest.err" | grep -aohE "$ID_ERE" | sort -u > "$2"
        # COVERAGE, the general form of the failure above: awk must have OPENED every
        # member. A count floor cannot tell a smaller tree from an unread batch; the
        # per-file marker can, and it names what was missed.
        sort -u "$1" > "$TMP/want.s"
        sort -u "$TMP/seen" > "$TMP/seen.s"
        comm -23 "$TMP/want.s" "$TMP/seen.s" >> "$TMP/unread"
    fi
    [ ! -s "$TMP/unread" ] || return 1
    # A pipeline hides awk's status, and an awk failure here costs part of the corpus rather
    # than the run.
    [ ! -s "$TMP/harvest.err" ] || return 1
    return 0
}

# --- self-test: prove the harvest both ways before reading the tree ------------
# A stripper that ate everything, or an alphabet that matched nothing, would each leave the
# valid set empty and report every name in every doc as dangling.
mkdir -p "$TMP/st"
cat > "$TMP/st/code.c" <<'EOF'
#define KICKOS_ST_DEFINED 1
enum { KOS_ST_ENUM = 2 };
// KICKOS_ST_LINE_COMMENT is prose, not a definition
/* KICKOS_ST_BLOCK_COMMENT spans
   KICKOS_ST_BLOCK_SECOND_LINE too */
EOF
cat > "$TMP/st/code.cmake" <<'EOF'
option(KICKOS_ST_OPTION "a knob" OFF)
# KICKOS_ST_HASH_COMMENT is prose here
EOF
printf '%s\n' "$TMP/st/code.c" "$TMP/st/code.cmake" > "$TMP/st/list"

harvest_ids "$TMP/st/list" "$TMP/st/ids" \
    || fail "the harvest refused its own self-test corpus, so it cannot judge the tree"
for _want in KICKOS_ST_DEFINED KOS_ST_ENUM KICKOS_ST_OPTION; do
    grep -qx "$_want" "$TMP/st/ids" \
        || fail "the harvest missed $_want, planted in CODE; it would report live names dangling"
done
for _no in KICKOS_ST_LINE_COMMENT KICKOS_ST_BLOCK_COMMENT KICKOS_ST_BLOCK_SECOND_LINE \
           KICKOS_ST_HASH_COMMENT; do
    ! grep -qx "$_no" "$TMP/st/ids" \
        || fail "the harvest took $_no out of a COMMENT; a name nothing builds would validate itself"
done

# The mutation, in both directions: ONE unreadable member must make the harvest refuse and
# name it, and removing that member must restore the pass.
ST_PHANTOM="$TMP/st/deleted_by_an_unstaged_rename.cc"
printf '%s\n' "$ST_PHANTOM" >> "$TMP/st/list"
if harvest_ids "$TMP/st/list" "$TMP/st/ids.mut"; then
    fail "the harvest accepted a corpus member that does not exist, so a tracked-but-deleted
      file would silently shrink the valid set again"
fi
grep -Fxq "$ST_PHANTOM" "$TMP/unread" \
    || fail "the harvest refused but did not name the missing file; the report has to say which"
grep -Fxv "$ST_PHANTOM" "$TMP/st/list" > "$TMP/st/list.ok"
harvest_ids "$TMP/st/list.ok" "$TMP/st/ids.back" \
    || fail "the harvest still refuses once the phantom is gone, so the refusal was not its"
cmp -s "$TMP/st/ids" "$TMP/st/ids.back" \
    || fail "the harvest read a different valid set before and after the phantom; not deterministic"

# --- the valid identifier set of the real tree --------------------------------
if ! harvest_ids "$TMP/src.txt" "$TMP/valid_ids.txt"; then
    if [ -s "$TMP/unread" ]; then
        echo "" >&2
        sed 's/^/      /' "$TMP/unread" >&2
        fail "the file(s) above are listed by git ls-files but the identifier scan did not read
      them, so it covers LESS of the tree than it reports and would call live names dangling.
      A rename whose deletion is not staged yet is the usual cause: stage it (git add -A)
      and re-run. If the file is present and readable, the scan skipped it and that is a bug
      in this gate, not in the tree."
    fi
    sed -n '1,4p' "$TMP/harvest.err" >&2
    fail "awk objected while scanning the tree for identifiers, so the valid set is incomplete"
fi
IDS=$(wc -l < "$TMP/valid_ids.txt" | tr -d ' ')

# --- valid path set: tracked files plus every ancestor directory ---------------
# Built from git, never from the filesystem, so an untracked build/ or .session/
# can never make a stale reference resolve.
git ls-files -z | tr '\0' '\n' > "$TMP/tracked.txt"
awk '{ print; n = split($0, c, "/"); p = ""; for (i = 1; i < n; i++) { p = p c[i]; print p; p = p "/" } }' \
  "$TMP/tracked.txt" | sort -u > "$TMP/valid_paths.txt"
awk -F/ '{ print $1 }' "$TMP/tracked.txt" | sort -u > "$TMP/toplevel.txt"

# Extensions the tree actually uses. Derived, not listed, so a doc naming a BUILD
# ARTIFACT (.hex/.uf2/.elf/.log) or a linker section (.bss/.eh_frame) is never
# mistaken for a source reference: no such extension is tracked.
sed 's|.*/||' "$TMP/tracked.txt" | grep '\.' | sed 's|.*\.||' | sort -u > "$TMP/exts.txt"
[ -s "$TMP/exts.txt" ] || fail "derived no file extensions from the tree; path shape rule is broken"

# --- syscall numbers, from the ABI header itself ------------------------------
ABI="user/include/kickos/sys/abi.h"
[ -f "$ABI" ] || fail "$ABI not found; cannot cross-check syscall numbers"
sed -n 's/^ *\(KOS_SYS_[A-Z0-9_]*\) *= *\([0-9][0-9]*\).*/\1 \2/p' "$ABI" > "$TMP/sysnum.txt"
[ -s "$TMP/sysnum.txt" ] || fail "parsed zero syscall numbers out of $ABI; number cross-check is broken"


# =============================================================================
# One pass over the corpus. Output is file-ordered then line-ordered, so the
# report is byte-identical across runs given the same tree.
# =============================================================================
# Two stages, not one pipe: under /bin/sh there is no pipefail, so `RC=$?` after a
# pipeline sees only awk. A grep that cannot read a file would then feed awk short
# input and the gate would report PASS on a corpus it never read.
tr '\n' '\0' < "$TMP/docs.txt" | xargs -0 grep -an '' /dev/null > "$TMP/corpus.txt" 2>/dev/null
[ -s "$TMP/corpus.txt" ] || fail "read zero lines out of $DOCS doc file(s); extraction is broken"
# Non-emptiness alone is satisfied by ONE readable doc: xargs splits the corpus into
# several grep invocations and keeps going after one of them dies, so a doc the scan
# never reached would simply contribute no findings. Reconcile file for file. A doc that
# is genuinely EMPTY contributes no line either and is the one admissible absence.
cut -d: -f1 < "$TMP/corpus.txt" | sort -u > "$TMP/corpus_files.txt"
MISSED=""
while read -r d; do
  if ! grep -qxF "$d" "$TMP/corpus_files.txt" && [ -s "$d" ]; then
    MISSED="$MISSED $d"
  fi
done < "$TMP/docs.txt"
[ -z "$MISSED" ] || fail "the scan never read:$MISSED, so those were checked against nothing"

awk -v T="$TMP" -v ABIH="$ABI" -F: '
function load(f, arr,   l) { while ((getline l < f) > 0) { arr[l] = 1 } close(f) }

# Collapse "a/b/../c" and "a/./b". Leading ".." that escapes the root is left in
# place, which makes the path unresolvable. That is correct: it is outside the repo.
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
    if (prevfile != "" && infence) { report(prevfile, fenceline, "unbalanced ``` fence opened here and never closed; extraction cannot trust this file") }
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
  # awk has no portable \b, so the left boundary the source scan gets from grep is done
  # by hand here: a match whose preceding character is an identifier character is a
  # SUBSTRING of a longer name (CAP_INDEX_BITS inside KCAP_INDEX_BITS), not a citation.
  # Both sides must agree, or every KCAP_ name in a doc reads as an unknown CAP_ one.
  rest = text
  off  = 0
  while (match(rest, /(KICKOS|KOS|KCAP|CAP|AUTH)_[A-Za-z0-9_]*[A-Za-z0-9]/)) {
    abs  = off + RSTART
    prev = ""
    if (abs > 1) { prev = substr(text, abs - 1, 1) }
    tok  = substr(rest, RSTART, RLENGTH)
    tail = substr(rest, RSTART + RLENGTH, 1)
    off  = abs + RLENGTH - 1
    rest = substr(rest, RSTART + RLENGTH)
    if (prev ~ /[A-Za-z0-9_]/) { continue }
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
      report(file, lineno, "identifier is mis-cased and cannot be grepped: " tok ", the tree spells it " up)
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
    # story. Without it, the prose alternations this corpus uses constantly (the
    # kernel/app split, the user/kernel boundary, the arch/chip seam, ldrex/strex,
    # PA4/PA5, SIM_SCGC4/5) are shape-identical to directory references and every
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
    # empty string; the root always exists, so it counts as plausible.
    split(r1, t1, "/")
    r1parent = r1; if (!sub(/\/[^\/]*$/, "", r1parent)) { r1parent = "" }
    r2parent = r2; if (!sub(/\/[^\/]*$/, "", r2parent)) { r2parent = "" }
    havedir1 = (r1parent == "" || r1parent in VALID_PATH)
    havedir2 = (r2parent == "" || r2parent in VALID_PATH)
    if (!(t1[1] in TOP) && !havedir2) { continue }

    kind = "file"; if (slash) { kind = "directory" }
    hint = ""
    if (!havedir1 && !havedir2) {
      hint = " (its parent directory does not exist either: an out-of-tree citation?)"
    }
    report(file, lineno, kind " path does not exist: " p hint)
  }
}

END {
  if (prevfile != "" && infence) { report(prevfile, fenceline, "unbalanced ``` fence opened here and never closed; extraction cannot trust this file") }
  exit (findings > 0)
}' < "$TMP/corpus.txt" > "$TMP/findings.txt"
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
