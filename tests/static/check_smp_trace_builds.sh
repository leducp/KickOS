#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KICKOS_SMP_TRACE is OFF in every preset, so its enabled arm is compiled by nothing and would
# rot unnoticed: a field added to the record, a renamed hook, a signature moved. This compiles
# THAT ARM, with the preset's own flags, and runs no image.
#
# THE FLAGS COME FROM compile_commands.json rather than from a list here, so they cannot drift
# from what the preset actually uses; only -DKICKOS_SMP_TRACE is overridden.
#
# An empty corpus is a FAILURE: a TU this cannot find is a rename, not a pass.
#
# usage: check_smp_trace_builds.sh <compile_commands.json>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_smp_trace_builds.sh <compile_commands.json>"
cc_json="${1:?$_usage}"
[ -f "$cc_json" ] || fail "no compile_commands.json at $cc_json"

# Every TU that carries a hook or the rings themselves.
TUS="smptrace.cc sync.cc sched.cc"
scratch_dir

built=0
for tu in $TUS; do
    line="$(sed -n 's/.*"command": *"\(.*\)".*/\1/p' "$cc_json" \
        | grep -F -- "$tu" | grep -v -- '-fsyntax-only' | head -n1)"
    if [ -z "$line" ]; then
        fail "no compile command for '$tu' in $cc_json. It was renamed or dropped from the
  build, so this gate would compile nothing and pass"
    fi
    # Drop the object output and the dep file, force the knob on, syntax only.
    # The preset already defines the knob OFF, so it is REMOVED before being forced on: a
    # second -D is a redefinition and -Werror refuses it.
    cmd="$(printf '%s' "$line" | sed 's/\\"/"/g; s/-o [^ ]*//; s/-c /-fsyntax-only /; s/-M[TFD] [^ ]*//g; s/-MD//; s/-DKICKOS_SMP_TRACE=[0-9]*//g' )"
    cmd="$cmd -DKICKOS_SMP_TRACE=1"
    if ! eval "$cmd" 2>"$TMP/err.$tu"; then
        sed -n '1,25p' "$TMP/err.$tu"
        fail "'$tu' does not compile with KICKOS_SMP_TRACE=1. The enabled arm has rotted:
  nothing else in the tree compiles it"
    fi
    built=$((built + 1))
done

require_number "$built" "the count of translation units compiled"
if [ "$built" -ne 3 ]; then
    fail "compiled $built translation unit(s), expected 3"
fi
echo "PASS: the KICKOS_SMP_TRACE arm compiles in $built translation unit(s), with the preset's
  own flags and no image run"
exit 0
