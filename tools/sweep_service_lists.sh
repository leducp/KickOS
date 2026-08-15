#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Links every declared kickos_services_* provider into an image.
#
# Ten of the thirteen reach an image only under an explicit -DKICKOS_SERVICE_LIST, so a
# fleet build compiles them without ever linking one. user/apps/common/usbcdcwit is gated
# on KICKOS_SERVICE_LIST matching _usbcdc and is built by no default configuration.
#
# An operator tool, not a gate: cross toolchains, minutes per entry.
#
#   source .session/env.sh && tools/sweep_service_lists.sh            # all declared
#   source .session/env.sh && tools/sweep_service_lists.sh picopi     # presets matching
#
# Knobs: SLSWEEP_OUT (default /var/tmp/kickos-slsweep), SLSWEEP_JOBS (8), SLSWEEP_FORCE=1.
#
# NOT CAUGHT: linking is not running, which is the bench's half. Each provider is built at
# the one preset its declaration names, so this is a floor and not a map.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DECL="$ROOT/tests/static/service_lists.txt"
OUT="${SLSWEEP_OUT:-/var/tmp/kickos-slsweep}"
JOBS="${SLSWEEP_JOBS:-8}"
SENTINEL="$OUT/summary.txt"

[ -f "$DECL" ] || { echo "FAIL: no declaration at $DECL" >&2; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "FAIL: no cmake on PATH, source .session/env.sh" >&2; exit 1; }

mkdir -p "$OUT/logs" "$OUT/status" || exit 1
# Cleared first, so a killed run leaves no stale summary.
rm -f "$SENTINEL"

FILTER="${1:-}"
PASS=0
FAIL=0
REUSED=0
N=0

# Comment-stripped and blank-stripped, as the gate that owns this file does it.
sed -e 's/#.*//' "$DECL" | grep -v '^[[:space:]]*$' | while read -r provider preset kind; do
    [ -n "$provider" ] || continue
    [ -n "$preset" ] || continue
    if [ -n "$FILTER" ]; then
        case "$preset" in
            *"$FILTER"*) ;;
            *) continue ;;
        esac
    fi
    N=$((N + 1))
    B="$OUT/trees/$provider"
    LOG="$OUT/logs/$provider.log"
    ST="$OUT/status/$provider"

    if [ -f "$ST" ] && [ -z "${SLSWEEP_FORCE:-}" ]; then
        printf 'REUSED  %-38s %s\n' "$provider" "$(cat "$ST")" | tee -a "$SENTINEL"
        REUSED=$((REUSED + 1))
        continue
    fi

    if ! cmake -S "$ROOT" -B "$B" --preset "$preset" \
            -DKICKOS_SERVICE_LIST="$provider" > "$LOG" 2>&1; then
        printf 'FAIL    %-38s configure failed, see %s\n' "$provider" "$LOG" | tee -a "$SENTINEL"
        FAIL=$((FAIL + 1))
        continue
    fi
    if ! cmake --build "$B" -j "$JOBS" >> "$LOG" 2>&1; then
        printf 'FAIL    %-38s build failed, see %s\n' "$provider" "$LOG" | tee -a "$SENTINEL"
        FAIL=$((FAIL + 1))
        continue
    fi

    # A configuration that links nothing is not a pass.
    # A cross image carries an extension, a sim one is a bare host executable. Both shapes
    # count, or the refusal below fires on the two sim providers.
    IMGS=$(find "$B" -type f \( -name '*.elf' -o -name '*.hex' -o -name '*.bin' -o -name '*.uf2' \
                                -o \( -perm -u+x -path '*/user/apps/*' \) \) \
             2>/dev/null | grep -v CMakeFiles | wc -l | tr -d ' ')
    if [ "$IMGS" -eq 0 ]; then
        printf 'FAIL    %-38s built but produced NO image, see %s\n' "$provider" "$LOG" | tee -a "$SENTINEL"
        FAIL=$((FAIL + 1))
        continue
    fi
    echo "$preset, $IMGS image(s)" > "$ST"
    printf 'PASS    %-38s %s, %s image(s)\n' "$provider" "$preset" "$IMGS" | tee -a "$SENTINEL"
    PASS=$((PASS + 1))
done

# The counters above live in the pipeline's subshell, so totals are re-derived here.
# Not `grep -c || echo 0`: grep prints its 0 and exits 1, so the fallback appends a second
# line and the arithmetic below dies on "Illegal number".
P=$(grep -c '^PASS' "$SENTINEL" 2>/dev/null) || P=0
F=$(grep -c '^FAIL' "$SENTINEL" 2>/dev/null) || F=0
R=$(grep -c '^REUSED' "$SENTINEL" 2>/dev/null) || R=0
echo ""
echo "DONE $((P + F + R)) provider(s): $P pass ($R reused), $F fail" | tee -a "$SENTINEL"
[ "$F" -eq 0 ] || exit 1
