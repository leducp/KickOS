#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Verdict on a completed TAP stream, read from stdin. Shared by every selftest gate.
#
# EXPECT_SKIPS (default empty) is a permission set, not a budget: a skip whose name is
# not listed fails the gate, and a listed name that did not skip is a NOTE, never a
# failure.

set -u
label="${1:-selftest}"
# A KICKOS_CONSOLE_CRLF board emits CR, which defeats the end-anchored parses below.
out="$(tr -d '\r')"

# A raw CMake list arrives semicolon-separated. Flattened to single-space separation
# because the membership tests below are `case` globs against " $name ".
expect_skips="$(printf '%s' "${EXPECT_SKIPS:-}" | tr ',;\t\n' '    ')"

if echo "$out" | grep -q "not ok"; then
    echo "FAIL: a TAP test reported not ok"
    exit 1
fi
if ! echo "$out" | grep -q "# all tests passed"; then
    echo "FAIL: TAP completion marker missing (crash / hang / truncated run?)"
    exit 1
fi

# Parsed after the completion marker so a truncated run is reported as truncated.
skipped="$(echo "$out" | sed -n 's/^# skipped: \([0-9][0-9]*\)$/\1/p' | tail -1)"
if [ -z "$skipped" ]; then
    echo "FAIL: no '# skipped: N' summary in the TAP stream (harness regression?)"
    exit 1
fi

# TAP spells a skip as a passing case with a directive: `ok <n> - <name> # SKIP <reason>`.
skipped_names="$(echo "$out" \
    | sed -n 's/^ok [0-9][0-9]* - \([A-Za-z0-9_]*\) # SKIP.*/\1/p' \
    | tr '\n' ' ')"
parsed=0
for _n in $skipped_names; do parsed=$((parsed + 1)); done

# If the parse and the harness disagree, the list below is checking nothing.
if [ "$parsed" -ne "$skipped" ]; then
    echo "FAIL: harness reported $skipped skip(s) but $parsed SKIP line(s) parsed."
    echo "      The 'ok N - name # SKIP' format moved; the expected-skip list is vacuous."
    echo "$out" | grep "# SKIP"
    exit 1
fi

unexpected=""
for n in $skipped_names; do
    case " $expect_skips " in
        *" $n "*) ;;
        *) unexpected="$unexpected $n" ;;
    esac
done
if [ -n "$unexpected" ]; then
    echo "FAIL: skip(s) not on the expected list:$unexpected"
    echo "      expected:${expect_skips:+ $expect_skips}"
    echo "$out" | grep "# SKIP"
    exit 1
fi

for e in $expect_skips; do
    case " $skipped_names " in
        *" $e "*) ;;
        *) echo "NOTE: '$e' is on the expected-skip list but did not skip -- trim it" ;;
    esac
done

echo "PASS: $label TAP suite clean ($skipped skipped, all expected)"
exit 0
