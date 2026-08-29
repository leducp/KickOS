#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Reachability gate on the fault-record console route. kvprintf_route hands a fault record to a
# published console driver through cap_console_deliver; any kpanic reachable from there prints,
# and the print re-enters kvprintf_route, so the delivery runs again with nothing bounding the
# depth. This gate walks the call graph from that route and fails when a panic terminal is
# reachable.
#
# tests/lib/scratch_ci.sh configures a SCRATCH tree of its own with -fcallgraph-info=su,da, and
# tests/static/console_reach.py merges the .ci files through tests/static/trap_redzone.py and
# walks from the roots tests/static/console_reach_roots.txt declares.

set -u
# Every path arrives as an argument and is re-split unquoted below; a glob character in a
# build path must not expand against the cwd.
set -f
. "$(dirname "$0")/../lib/gate.sh"
KOS_CI_TAG=console_reach
. "$(dirname "$0")/../lib/scratch_ci.sh"

# The record parses below are structural, never keyed on a translated heading.
export LC_ALL=C

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <src-dir> <cmake> <preset> <arch>" >&2
    exit 2
fi

SRC="$1"
CMAKE="$2"
PRESET="$3"
ARCH="$4"

case "$SRC$CMAKE$PRESET$ARCH" in
    *[[:space:]]*) fail "an argument contains whitespace; every path here is re-split" ;;
esac

HERE="$(dirname "$0")"
DECL="$HERE/console_reach_roots.txt"
INDIRECT="$HERE/trap_redzone_indirect.txt"
TOOL="$HERE/console_reach.py"
GRAPH="$HERE/trap_redzone.py"
for f in "$DECL" "$INDIRECT" "$TOOL" "$GRAPH"; do
    [ -r "$f" ] || fail "cannot read $f"
done
[ -d "$SRC" ] || fail "source dir does not exist: $SRC"
command -v "$CMAKE" >/dev/null 2>&1 || [ -x "$CMAKE" ] || fail "cmake not executable: $CMAKE"

scratch_dir

# --- clause 0: the preset/arch pair is declared -------------------------------
# A pair nobody declared has no root set and no floor. The tool refuses it too; this copy is
# what keeps the message short when a board is registered by mistake.
if ! awk -v A="$ARCH" -v P="$PRESET" '
        { sub(/#.*/, "") }
        { n = split($0, f, /[[:space:]]+/) }
        n >= 3 && f[1] == "preset" && f[2] == A && f[3] == P { found = 1 }
        END { exit !found }' "$DECL"
then
    KNOWN="$(awk -v A="$ARCH" '{ sub(/#.*/, "") }
             $1 == "preset" && $2 == A { printf "%s ", $3 }' "$DECL")"
    fail "preset/arch pair $PRESET/$ARCH is not declared in $DECL (for $ARCH: $KNOWN)"
fi

# --- clause 1: the floor is armed ---------------------------------------------
# THE ONE THING A CLAUSE THAT ASSERTS AN ABSENCE CANNOT SKIP. The tool is handed an empty
# directory and has to refuse it. A floor that has stopped firing then fails HERE, on every
# board, rather than the day a build breaks and the gate reports the route clean over nothing.
mkdir -p "$TMP/empty"
if python3 "$TOOL" --ci-dir "$TMP/empty" --arch "$ARCH" --preset "$PRESET" \
       --decl "$DECL" --indirect "$INDIRECT" > "$TMP/floor.log" 2>&1
then
    sed -n '1,20p' "$TMP/floor.log" >&2
    fail "the clause reported success over an EMPTY .ci directory. Its corpus floor is not
    working, so every green run it has ever produced is an absence over an unknown corpus"
fi
if ! grep -q 'no .ci file under' "$TMP/floor.log"; then
    sed -n '1,20p' "$TMP/floor.log" >&2
    fail "the clause refused an empty .ci directory for the wrong reason; the refusal must be
    the corpus floor and not an accident of the declaration"
fi
echo "console_reach: floor armed, an empty .ci directory is refused"

# --- configure and build the scratch tree ------------------------------------
# Named per preset so two boards do not fight over one tree, and reused so a re-run is cheap.
# Under /var/tmp because these trees are large and must outlive a /tmp wipe.
BUILD="${KICKOS_CONSOLE_REACH_DIR:-/var/tmp/kickos-console-reach-$PRESET}"
scratch_ci_build "$SRC" "$CMAKE" "$PRESET" "$BUILD"

# --- the walk -----------------------------------------------------------------
python3 "$TOOL" --ci-dir "$BUILD" --arch "$ARCH" --preset "$PRESET" \
    --decl "$DECL" --indirect "$INDIRECT"
rc=$?

if [ "$rc" -eq 0 ]; then
    echo "console_reach: OK ($PRESET/$ARCH)"
fi
exit "$rc"
