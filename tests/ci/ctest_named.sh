#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Run a ctest selection made BY NAME, and refuse one whose names are no longer registered.
#
# usage: ctest_named.sh <name list> <ctest argument>...
#          e.g. ctest_named.sh 'a|b' --test-dir build/x --no-tests=error --output-on-failure
#
# WHAT --no-tests=error DOES NOT CATCH. It fires on an ENTIRELY empty selection, so
# `ctest -R 'live_gate|GONE'` runs live_gate and exits 0 with nothing said about GONE. A gate
# that stops being registered, renamed or with the if() around its registration now false,
# leaves every -R step that named it green.
#
# THE NAMES GO THROUGH ctest's OWN MATCHER, one alternative at a time: what this believes a
# pattern selects is what the run will believe. Nothing here reads the list as a regex of its
# own, so an anchored name (^foo$) and a substring name are each asked for as written.
#
# WHAT IT DOES NOT CLAIM: -R matches a SUBSTRING, so a gate renamed from `bench_lock` to
# `bench_lockheld` is still selected by the old name and nothing was lost. The loss this
# refuses is a name that selects NOTHING.
#
# A COUNT IS NOT READ AGAINST THE NUMBER OF NAMES. `ctest -N` pulls in whatever fixture the
# selected tests require, so a one-name selection can list two tests; what cannot happen is a
# fixture appearing beside nothing, so the rule is one or more per name.

set -u

_usage="usage: ctest_named.sh <name list> <ctest argument>..."
_list="${1:?$_usage}"
shift
if [ "$#" -eq 0 ]; then
    echo "FAIL: $_usage" >&2
    exit 1
fi

# Split on '|' and on nothing else, with globbing off so an alternative carrying a '*' is
# asked for and not expanded against the working directory. Every expansion inside the loop is
# quoted, so IFS is put back once, after it.
_oldifs="$IFS"
IFS='|'
set -f
for _name in $_list; do
    if [ -z "$_name" ]; then
        echo "FAIL: [$_list] has an empty alternative, which selects every test there is" >&2
        exit 1
    fi
    _n="$(ctest "$@" -N -R "$_name" 2>/dev/null \
            | sed -n 's/^Total Tests: *\([0-9][0-9]*\)$/\1/p' | tail -n1)"
    case "$_n" in
        ''|*[!0-9]*)
            echo "FAIL: ctest -N answered no test count for [$_name]; the selection below
  cannot be judged, so it is refused rather than run" >&2
            exit 1
            ;;
    esac
    if [ "$_n" -eq 0 ]; then
        echo "FAIL: [$_name] selects no registered test. It was renamed, or the if() around
  its registration is now false. Either way this step would have run the rest of
  [$_list] and exited 0, saying nothing about it" >&2
        exit 1
    fi
done
IFS="$_oldifs"
set +f

exec ctest "$@" -R "$_list"
