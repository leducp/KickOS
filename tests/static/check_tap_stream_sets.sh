#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tests/integration/check_tap_stream.sh's expected skip and partial sets against planted streams:
# the exact sets pass, and an undeclared skip or partial fails, as does a declared one that the
# stream does not report.

set -u
. "$(dirname "$0")/../lib/gate.sh"

scratch_dir
rc=0

cat > "$TMP/stream" <<'TAP'
1..3
ok 1 - alpha
ok 2 - beta # PARTIAL half not run
ok 3 - gamma # SKIP no pool
# todo: 0
# todo-fixed: 0
# skipped: 1
# vacuous: 0
# partial: 1
# all tests passed (1 skipped, 0 vacuous, 1 partial)
TAP

# <skips> <partials>: the gate's exit status over the planted stream, its output in $TMP/out.
judged() {
    EXPECT_SKIPS="$1" EXPECT_PARTIALS="$2" EXPECT_FAULTS="" \
        sh tests/integration/check_tap_stream.sh planted 3 < "$TMP/stream" > "$TMP/out" 2>&1
}

judged gamma beta || bad "the exact sets fail: $(tail -n 1 "$TMP/out")"
if judged gamma beta,alpha; then
    bad "a declared partial the stream does not report passes"
elif ! grep -q "^FAIL: on the expected-partial list but not reported PARTIAL: alpha" "$TMP/out"; then
    bad "a declared partial the stream does not report fails for another reason: $(head -n 1 "$TMP/out")"
fi
if judged gamma,alpha beta; then
    bad "a declared skip the stream does not report passes"
elif ! grep -q "^FAIL: on the expected-skipped list but not reported SKIP: alpha" "$TMP/out"; then
    bad "a declared skip the stream does not report fails for another reason: $(head -n 1 "$TMP/out")"
fi
if judged gamma ""; then
    bad "an undeclared partial passes"
fi
if judged "" beta; then
    bad "an undeclared skip passes"
fi

[ "$rc" -eq 0 ] || exit 1
echo "PASS: the expected skip and partial sets are exact by name"
