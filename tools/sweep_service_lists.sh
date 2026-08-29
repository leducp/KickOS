#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Links every declared kickos_services_* provider into an image.
#
# Most of them, the `select` rows of tests/static/service_lists.txt, reach an image only
# under an explicit -DKICKOS_SERVICE_LIST, so a fleet build compiles them without ever
# linking one.
#
# An operator tool, not a gate: cross toolchains, minutes per entry.
#
#   source .session/env.sh && tools/sweep_service_lists.sh            # all declared
#   source .session/env.sh && tools/sweep_service_lists.sh picopi     # presets matching
#
# Knobs: SLSWEEP_OUT (default /var/tmp/kickos-slsweep), SLSWEEP_JOBS (8), SLSWEEP_FORCE=1.
#
# The DONE sentinel is written only when every selected row reached a verdict, at least one
# provider linked, and the fleet total of images is above zero. A filter that matches no row
# and a declaration with no rows are refusals. The image counter every verdict rests on is
# proved on a planted directory first.
#
# The recorded status belongs to a TREE. A previous run's status is reused only when the
# source tree is byte for byte the one it was recorded against, and only when the recorded
# line still names its own image count.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DECL="$ROOT/tests/static/service_lists.txt"
OUT="${SLSWEEP_OUT:-/var/tmp/kickos-slsweep}"
JOBS="${SLSWEEP_JOBS:-8}"
SUMMARY="$OUT/summary.txt"
SENTINEL="$OUT/DONE"

die() { echo "FAIL: $*" >&2; exit 1; }

[ -f "$DECL" ] || die "no declaration at $DECL"
command -v cmake >/dev/null 2>&1 || die "no cmake on PATH, source .session/env.sh"

# count_images <build dir>
# A cross image carries an extension, a sim one is a bare host executable. Both shapes
# count, or the refusal below fires on the two sim providers.
count_images() {
    find "$1" -type f \( -name '*.elf' -o -name '*.hex' -o -name '*.bin' -o -name '*.uf2' \
                          -o \( -perm -u+x -path '*/user/apps/*' \) \) \
        2>/dev/null | grep -v CMakeFiles | wc -l | tr -d ' '
}

# One planted tree per shape the counter has to see, and one it has to ignore, so a counter
# that answers zero for everything is caught before any verdict rests on it.
CTL="$(mktemp -d)" || die "mktemp -d failed"
mkdir -p "$CTL/user/apps/blink" "$CTL/CMakeFiles"
[ "$(count_images "$CTL")" = 0 ] || die "the image counter finds images in a tree with none"
: > "$CTL/kickos.elf"
: > "$CTL/CMakeFiles/decoy.elf"
: > "$CTL/user/apps/blink/blink"
chmod +x "$CTL/user/apps/blink/blink"
: > "$CTL/notes.txt"
CTL_N="$(count_images "$CTL")"
rm -rf "$CTL"
[ "$CTL_N" = 2 ] || die "the image counter answered $CTL_N over a planted tree holding one
      cross image, one host executable, one CMakeFiles decoy and one plain file; it must
      answer 2. Every PASS below would rest on a count it cannot take."

mkdir -p "$OUT/logs" "$OUT/status" || exit 1
# Cleared first, so a killed run leaves no stale summary and no stale sentinel.
rm -f "$SUMMARY" "$SENTINEL"

# What the recorded status is evidence ABOUT. Without git the identity is unique to this run,
# so nothing is ever reused.
TREE_ID="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null)"
if [ -n "$TREE_ID" ]; then
    TREE_ID="$TREE_ID $(git -C "$ROOT" status --porcelain 2>/dev/null | cksum)"
else
    TREE_ID="no git under $ROOT, run $$ at $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
fi
STAMP="$OUT/tree.stamp"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" != "$TREE_ID" ]; then
    echo "note: $OUT holds status recorded against another tree; discarding it" >&2
    rm -rf "$OUT/status" || die "cannot clear $OUT/status"
    mkdir -p "$OUT/status" || die "cannot create $OUT/status"
fi
printf '%s\n' "$TREE_ID" > "$STAMP"

FILTER="${1:-}"
PASS=0
FAIL=0
REUSED=0
IMAGES=0

# Comment-stripped and blank-stripped, as the gate that owns this file does it. Read from a
# file rather than a pipeline, so the counters below are the run's own and not a subshell's.
ROWS="$OUT/rows.txt"
sed -e 's/#.*//' "$DECL" \
    | awk -v f="$FILTER" 'NF >= 2 && (f == "" || index($2, f) > 0)' > "$ROWS"
N_SEL="$(wc -l < "$ROWS" | tr -d ' ')"
# Handed nothing, this tool would link nothing and report nothing wrong.
[ "$N_SEL" -gt 0 ] || die "no declared provider matches '$FILTER', so there is no corpus to
      sweep. A run over zero providers has nothing to fail and would report clean over
      nothing."

while read -r provider preset kind; do
    B="$OUT/trees/$provider"
    LOG="$OUT/logs/$provider.log"
    ST="$OUT/status/$provider"

    if [ -f "$ST" ] && [ -z "${SLSWEEP_FORCE:-}" ]; then
        # A recorded line that no longer names its own image count is not readable evidence.
        RIMG="$(sed -n 's/.*, \([0-9][0-9]*\) image(s).*/\1/p' "$ST")"
        if [ -n "$RIMG" ]; then
            printf 'REUSED  %-38s %s\n' "$provider" "$(cat "$ST")" | tee -a "$SUMMARY"
            REUSED=$((REUSED + 1))
            IMAGES=$((IMAGES + RIMG))
            continue
        fi
    fi

    # stdin is the row file this loop reads, so every child gets /dev/null instead.
    if ! cmake -S "$ROOT" -B "$B" --preset "$preset" \
            -DKICKOS_SERVICE_LIST="$provider" > "$LOG" 2>&1 < /dev/null; then
        printf 'FAIL    %-38s configure failed, see %s\n' "$provider" "$LOG" | tee -a "$SUMMARY"
        FAIL=$((FAIL + 1))
        continue
    fi
    if ! cmake --build "$B" -j "$JOBS" >> "$LOG" 2>&1 < /dev/null; then
        printf 'FAIL    %-38s build failed, see %s\n' "$provider" "$LOG" | tee -a "$SUMMARY"
        FAIL=$((FAIL + 1))
        continue
    fi

    # A configuration that links nothing is not a pass.
    IMGS="$(count_images "$B")"
    if [ "$IMGS" -eq 0 ]; then
        printf 'FAIL    %-38s built but produced NO image, see %s\n' "$provider" "$LOG" | tee -a "$SUMMARY"
        FAIL=$((FAIL + 1))
        continue
    fi
    echo "$preset, $IMGS image(s)" > "$ST"
    printf 'PASS    %-38s %s, %s image(s)\n' "$provider" "$preset" "$IMGS" | tee -a "$SUMMARY"
    PASS=$((PASS + 1))
    IMAGES=$((IMAGES + IMGS))
done < "$ROWS"

SEEN=$((PASS + FAIL + REUSED))
echo ""
echo "DONE $SEEN provider(s), $IMAGES image(s): $PASS pass ($REUSED reused), $FAIL fail" \
    | tee -a "$SUMMARY"

# Every clause below is something this run has to have DONE.
WHY=""
refuse() { WHY="$WHY
      $*"; }

[ "$SEEN" -eq "$N_SEL" ] \
    || refuse "$SEEN of the $N_SEL selected provider(s) reached a verdict"
[ "$FAIL" -eq 0 ] \
    || refuse "$FAIL provider(s) failed"
[ $((PASS + REUSED)) -gt 0 ] \
    || refuse "no provider linked"
[ "$IMAGES" -gt 0 ] \
    || refuse "zero images were produced across the whole selection"

if [ -n "$WHY" ]; then
    {
        echo "REFUSED: this sweep is not a witness for the selection it was handed:$WHY"
        echo "      no DONE sentinel written under $OUT"
    } | tee -a "$SUMMARY" >&2
    exit 1
fi

# The only writer of this file, and it carries what it asserts.
{
    echo "tree     $TREE_ID"
    echo "selected $N_SEL provider(s)"
    echo "linked   $PASS provider(s) ($REUSED reused)"
    echo "images   $IMAGES"
    echo "finished $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} > "$SENTINEL"
