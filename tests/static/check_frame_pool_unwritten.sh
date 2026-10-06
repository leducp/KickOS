#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kernel/mem/frame_pool_unwritten.h hands frames out with their previous owner's bytes, and
# kernel-alias-maintained-over-uncached-memory (docs/reference/invariants.md) holds only while
# every caller of it writes every byte first. Those callers live in kernel/mem, so no tracked
# source outside kernel/mem and tests may include it, under any spelling: "..." or <...>, with
# or without a directory prefix, nor name its entry point in code, which a redeclaration of
# it would.
#
# Run from the repo root, no arguments: tests/static/check_frame_pool_unwritten.sh
#
# THE CORPUS COMES FROM `git ls-files`, so an untracked file is invisible: stage before gating.
# An include spelled through a macro, or a name pasted together by one, is not seen.

set -u
# The corpus is handed to the reader unquoted through xargs; a glob character in a tracked
# path must not expand against the cwd.
set -f
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root

scratch_dir

HEADER=frame_pool_unwritten.h
ENTRY=frame_pool_alloc_run

includes() { # <list-file> <outfile>
    tr '\n' '\0' < "$1" | xargs -0 awk '
        /^[ \t]*#[ \t]*include[ \t]*[<"]([^<>"]*\/)?frame_pool_unwritten\.h[>"]/ {
            print FILENAME ":" FNR ": " $0
        }' > "$2" || fail "the include reader failed, so the set of includers is UNKNOWN"
}

# A line of code naming the entry point, a comment line not.
names() { # <list-file> <outfile>
    tr '\n' '\0' < "$1" | xargs -0 awk '
        /^[ \t]*(\/\/|\/\*|\*)/ { next }
        /(^|[^A-Za-z0-9_])frame_pool_alloc_run([^A-Za-z0-9_]|$)/ {
            print FILENAME ":" FNR ": " $0
        }' > "$2" || fail "the name reader failed, so the set of names is UNKNOWN"
}

# --- the readers' controls -----------------------------------------------------
mkdir -p "$TMP/ctl" || fail "cannot create the control tree"
printf '#include "%s"\n' "$HEADER" > "$TMP/ctl/quoted.cc"
printf '#include <kernel/mem/%s>\n' "$HEADER" > "$TMP/ctl/angled.cc"
printf '  #  include "../mem/%s"\n' "$HEADER" > "$TMP/ctl/spaced.cc"
printf '// %s is private to kernel/mem\n' "$HEADER" > "$TMP/ctl/prose.cc"
printf '#include "frame_pool_unwritten.hh"\n' > "$TMP/ctl/other_ext.cc"
printf '#include "not_%s"\n' "$HEADER" > "$TMP/ctl/other_name.cc"
for f in quoted angled spaced prose other_ext other_name; do
    printf '%s\n' "$TMP/ctl/$f.cc"
done > "$TMP/ctl.list"
includes "$TMP/ctl.list" "$TMP/ctl.hits"
n="$(wc -l < "$TMP/ctl.hits" | tr -d ' ')"
for f in quoted angled spaced; do
    grep -q "/$f\.cc:" "$TMP/ctl.hits" || fail "the reader missed a planted $f include of
  $HEADER, so an includer spelled that way outside kernel/mem would pass"
done
[ "$n" = "3" ] || fail "the reader found $n include(s) in the control tree instead of 3; it
  is reading prose or another header as an include of $HEADER"
printf 'namespace kickos {\n    arch_phys_addr_t %s(size_t pages);\n}\n' "$ENTRY" \
    > "$TMP/ctl/redeclared.cc"
printf 'x = kickos::%s(4);\n' "$ENTRY" > "$TMP/ctl/called.cc"
printf '    // %s is private to kernel/mem\n' "$ENTRY" > "$TMP/ctl/named_prose.cc"
printf ' * %s, in a block comment\n' "$ENTRY" > "$TMP/ctl/block_prose.cc"
printf 'y = %ss(4);\n' "$ENTRY" > "$TMP/ctl/longer_name.cc"
for f in redeclared called named_prose block_prose longer_name; do
    printf '%s\n' "$TMP/ctl/$f.cc"
done > "$TMP/ctl_names.list"
names "$TMP/ctl_names.list" "$TMP/ctl_names.hits"
n="$(wc -l < "$TMP/ctl_names.hits" | tr -d ' ')"
for f in redeclared called; do
    grep -q "/$f\.cc:" "$TMP/ctl_names.hits" || fail "the name reader missed a planted $f
  $ENTRY, so a source outside kernel/mem reaching it that way would pass"
done
[ "$n" = "2" ] || fail "the name reader found $n line(s) in the control tree instead of 2; it
  is reading a comment or a longer name as $ENTRY"

# --- the corpus ------------------------------------------------------------------
corpus_sources "$TMP/corpus"
ncorpus="$(wc -l < "$TMP/corpus" | tr -d ' ')"
grep '^kernel/mem/' "$TMP/corpus" > "$TMP/inside" || true
grep -v -e '^kernel/mem/' -e '^tests/' "$TMP/corpus" | drop_vendored > "$TMP/outside"
noutside="$(wc -l < "$TMP/outside" | tr -d ' ')"
echo "frame_pool_unwritten: $ncorpus tracked source(s), $noutside outside kernel/mem and tests"
require_nonempty "$TMP/outside" "no tracked source outside kernel/mem and tests, so there is
  nothing to keep $HEADER out of and this gate would pass on any tree"

# Its own users must still be seen, or the header was renamed and this gate watches nothing.
require_nonempty "$TMP/inside" "no tracked source under kernel/mem"
includes "$TMP/inside" "$TMP/inside.hits"
require_nonempty "$TMP/inside.hits" "nothing in kernel/mem includes $HEADER any more. It was
  renamed or removed, and this gate is keeping a name nothing uses private"

names "$TMP/inside" "$TMP/inside_names.hits"
require_nonempty "$TMP/inside_names.hits" "nothing in kernel/mem names $ENTRY any more. It was
  renamed or removed, and this gate is keeping a name nothing uses private"
names "$TMP/outside" "$TMP/outside_names.hits"
if [ -s "$TMP/outside_names.hits" ]; then
    cat "$TMP/outside_names.hits" >&2
    fail "$(wc -l < "$TMP/outside_names.hits" | tr -d ' ') line(s) above name $ENTRY outside
  kernel/mem and the tests, a redeclaration of the entry point $HEADER keeps private. Ask
  kernel/mem for cleared frames instead"
fi

includes "$TMP/outside" "$TMP/outside.hits"
if [ -s "$TMP/outside.hits" ]; then
    cat "$TMP/outside.hits" >&2
    fail "$(wc -l < "$TMP/outside.hits" | tr -d ' ') include(s) above of $HEADER outside
  kernel/mem. It hands out frames still holding their previous owner's bytes, and the alias
  invariant holds only because every caller in kernel/mem overwrites them first. Ask
  kernel/mem for cleared frames instead"
fi

ninside="$(wc -l < "$TMP/inside.hits" | tr -d ' ')"
echo "PASS: $HEADER is included $ninside time(s) under kernel/mem and by none of the"
echo "  $noutside tracked source(s) outside it"
exit 0
