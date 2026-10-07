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
#   corpus       every tracked *.md (corpus, tests/lib/gate.sh), each read with its NUL
#                bytes mapped to a control byte so no tool skips it as binary.
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

require_repo_root

scratch_dir

# --- corpus -------------------------------------------------------------------
corpus "$TMP/docs.txt" "markdown doc" '*.md'
corpus_floor "$TMP/docs.txt" 50 "doc(s)"
DOCS="$N"
corpus "$TMP/tracked.txt" "tracked file"

# --- valid identifier set, scanned from the tree ------------------------------
# Every such token in a tracked NON-markdown file outside docs/, this script and the vendored
# directories: this script quotes dead names as examples, a document under docs/ in another
# extension would keep every name it records valid forever, and another project's text cannot
# make a KickOS name valid. The left boundary keeps a KCAP_ name from yielding its CAP_ tail.
drop_vendored < "$TMP/tracked.txt" | grep -v '\.md$' | grep -v '^docs/' \
    | grep -v '^tests/static/check_doc_names\.sh$' > "$TMP/src.txt"
corpus_floor "$TMP/src.txt" 1000 "non-markdown source(s)"
# Comments do not confer validity: a name only prose mentions is a name nothing builds, and
# admitting one lets a removed knob validate itself in every doc. `#` opens a comment in sh,
# CMake and Kconfig and the preprocessor in C, so the strip is per type; an extension it does
# not know passes through unstripped, and a `#` inside a shell string takes the line with it.
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
    emit(line)
}

# The left boundary, spelled here rather than in the alphabet: `\b` is not a word boundary
# in awk, where it reads as a backspace and the scan then matches nothing and says so calmly.
# A match whose left neighbour is a name character is dropped whole, so KCAP_X never yields
# its CAP_X tail.
function emit(s,   p, pre) {
    while (match(s, CORE)) {
        p = RSTART
        pre = ""
        if (p > 1) { pre = substr(s, p - 1, 1) }
        if (pre !~ /[A-Za-z0-9_]/) { print substr(s, p, RLENGTH) }
        s = substr(s, p + RLENGTH)
    }
}
AWK

# The one alphabet, matched by awk and never by grep, whose -o is not standard.
ID_CORE='(KICKOS|KOS|KCAP|CAP|AUTH)_[A-Za-z0-9_]*[A-Za-z0-9]'

# awk must have OPENED every member: xargs splits the list, and a file awk cannot open drops
# the rest of its batch while the pipeline reports sort's status. The unread are named.
harvest_ids() { # <file-list> <outfile>; 0 ok, 1 a member went unread (named in $TMP/unread)
    : > "$TMP/seen"
    tr '\n' '\0' < "$1" | xargs -0 awk -v SEEN="$TMP/seen" -v CORE="$ID_CORE" \
        -f "$TMP/harvest.awk" 2>"$TMP/harvest.err" | sort -u > "$2"
    sort -u "$1" > "$TMP/want.s"
    sort -u "$TMP/seen" > "$TMP/seen.s"
    comm -23 "$TMP/want.s" "$TMP/seen.s" > "$TMP/unread"
    [ ! -s "$TMP/unread" ] && [ ! -s "$TMP/harvest.err" ]
}

# --- the planted harvest: names in code count, names in comments do not ----------
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
    || fail "the harvest refused its planted corpus, so it cannot judge the tree"
for _want in KICKOS_ST_DEFINED KOS_ST_ENUM KICKOS_ST_OPTION; do
    grep -qx "$_want" "$TMP/st/ids" \
        || fail "the harvest missed $_want, planted in CODE; it would report live names dangling"
done
for _no in KICKOS_ST_LINE_COMMENT KICKOS_ST_BLOCK_COMMENT KICKOS_ST_BLOCK_SECOND_LINE \
           KICKOS_ST_HASH_COMMENT; do
    ! grep -qx "$_no" "$TMP/st/ids" \
        || fail "the harvest took $_no out of a COMMENT; a name nothing builds would validate itself"
done

# --- the valid identifier set of the real tree --------------------------------
if ! harvest_ids "$TMP/src.txt" "$TMP/valid_ids.txt"; then
    if [ -s "$TMP/unread" ]; then
        echo "" >&2
        sed 's/^/      /' "$TMP/unread" >&2
        fail "the identifier scan did not read the tracked file(s) above, so it would call live
      names dangling. An unstaged rename is the usual cause."
    fi
    sed -n '1,4p' "$TMP/harvest.err" >&2
    fail "awk objected while scanning the tree for identifiers, so the valid set is incomplete"
fi
IDS=$(wc -l < "$TMP/valid_ids.txt" | tr -d ' ')

# --- valid path set: tracked files plus every ancestor directory ---------------
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

# One file at a time and every stage's status read, /bin/sh having no pipefail. The name goes
# to awk through -v: an operand of the form `name=value` is a variable assignment.
: > "$TMP/corpus.txt"
while IFS= read -r _d; do
    LC_ALL=C tr '\000' '\001' < "$_d" > "$TMP/nulfree" \
        || fail "cannot read $_d, so every name in it would be checked against nothing"
    awk -v F="$_d" '{ print F ":" NR ":" $0 }' < "$TMP/nulfree" >> "$TMP/corpus.txt" \
        || fail "the reader failed on $_d, so its lines are UNREAD and not clean"
done < "$TMP/docs.txt"
[ -s "$TMP/corpus.txt" ] || fail "read zero lines out of $DOCS doc file(s); extraction is broken"

# The reporting pass, in a file because it runs over the planted corpus first and then the tree.
cat > "$TMP/report.awk" <<'AWKEOF'
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

# The reader prints "<file>:<lineno>:<text>"; -F: splits it, but the text may hold
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

    # THE PLURAL AND MIS-CASED FALLBACKS ONLY APPLY WHEN TOK ITSELF IS NOT ALREADY VALID.
    # Nested inside that branch and not ahead of it: a bare `if (singular in VALID_ID)
    # continue` fires on every EXACT match too (singular equals tok when tok has no
    # trailing s), which would skip a live, correctly-spelled KOS_SYS_* name straight
    # past the syscall-number check below and leave the reused-number case unreachable
    # for the one spelling that matters most.
    name = tok
    if (!(tok in VALID_ID)) {
      singular = tok
      sub(/s$/, "", singular)
      if (singular in VALID_ID) { continue }                    # English plural of a real symbol

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
}
AWKEOF

# --- the planted citations: a dead identifier and a dead path, beside their live twins -----
mkdir -p "$TMP/rt"
printf '%s\n' KICKOS_RT_LIVE > "$TMP/rt/valid_ids.txt"
printf '%s\n' docs kernel docs/rt.md kernel/rt_live.cc > "$TMP/rt/valid_paths.txt"
printf '%s\n' docs kernel > "$TMP/rt/toplevel.txt"
printf '%s\n' md cc > "$TMP/rt/exts.txt"
printf '%s\n' 'KOS_SYS_RT 7' > "$TMP/rt/sysnum.txt"

cat > "$TMP/rt/clean" <<'RTEOF'
docs/rt.md:1:KICKOS_RT_LIVE is the knob
docs/rt.md:2:the body is in kernel/rt_live.cc today
RTEOF
if ! awk -v T="$TMP/rt" -v ABIH="$ABI" -F: -f "$TMP/report.awk" \
        < "$TMP/rt/clean" > "$TMP/rt/clean.out"; then
    cat "$TMP/rt/clean.out" >&2
    fail "the reporting pass reports a finding on a planted corpus whose every citation
      resolves, so every finding it makes against the tree is unattributable"
fi

cat > "$TMP/rt/dirty" <<'RTEOF'
docs/rt.md:1:KICKOS_RT_GONE is the knob
docs/rt.md:2:the body is in kernel/rt_gone.cc today
RTEOF
if awk -v T="$TMP/rt" -v ABIH="$ABI" -F: -f "$TMP/report.awk" \
       < "$TMP/rt/dirty" > "$TMP/rt/dirty.out"; then
    fail "THE REPORTING PASS FOUND NOTHING in a corpus citing the identifier KICKOS_RT_GONE
      and the path kernel/rt_gone.cc, neither of which exists. It cannot report a dead
      citation, which is the one thing this gate is for, so a green run over the tree
      witnesses nothing."
fi
_rt_n="$(wc -l < "$TMP/rt/dirty.out" | tr -d " ")"
[ "$_rt_n" -eq 2 ] \
    || fail "the reporting pass made $_rt_n finding(s) on a planted corpus carrying exactly
      one dead identifier and one dead path: $(cat "$TMP/rt/dirty.out")"
grep -q "KICKOS_RT_GONE" "$TMP/rt/dirty.out" \
    || fail "the reporting pass made two findings and neither names the dead identifier
      KICKOS_RT_GONE: $(cat "$TMP/rt/dirty.out")"
grep -q "kernel/rt_gone.cc" "$TMP/rt/dirty.out" \
    || fail "the reporting pass made two findings and neither names the dead path
      kernel/rt_gone.cc: $(cat "$TMP/rt/dirty.out")"

awk -v T="$TMP" -v ABIH="$ABI" -F: -f "$TMP/report.awk" \
    < "$TMP/corpus.txt" > "$TMP/findings.txt"
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
