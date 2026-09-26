#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Repeated x86 IPC sweep on named physical-core sets. Build all five images first.
# Usage: run_m95_matrix.sh [repetitions]

set -eu

case "${1:-3}" in
    ''|*[!0-9]*) echo "usage: $0 [positive repetitions]" >&2; exit 1 ;;
esac
reps="${1:-3}"
[ "$reps" -gt 0 ] || { echo "repetitions must be positive" >&2; exit 1; }

root="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
build="$root/build/m95"
out="$build/runs-pinned"
mkdir -p "$out"

run_case() { # label cores host-cpu-set repetition
    label="$1"
    cores="$2"
    cpuset="$3"
    r="$4"
    app="$build/ipc$cores/user/apps/common/bench/bench_smp_ipc.efi"
    dest="$out/$label-r$r"
    "$root/tools/bench/run_m95_ipc.sh" "$app" "$cores" "$dest" "$cpuset" \
        > "$dest.summary"
    printf '%s rep %s: ' "$label" "$r"
    grep '^ipc-pairs: len=8 cores=' "$dest.summary"
}

# This machine's CPUs 0-3 are fast physical cores, 4-11 slower physical cores;
# 12-23 are SMT siblings. m95_pin_qemu.py binds each vCPU to one listed core
# before OVMF starts. Other QEMU threads may share those cores.
r=1
while [ "$r" -le "$reps" ]; do
    if [ $((r % 2)) -eq 1 ]; then
        run_case fast1   1 0-1  "$r"
        run_case fast2   2 0-1  "$r"
        run_case fast4   4 0-3  "$r"
        run_case slow1   1 4-5  "$r"
        run_case slow2   2 4-5  "$r"
        run_case slow4   4 4-7  "$r"
        run_case slow8   8 4-11 "$r"
        run_case mixed12 12 0-11 "$r"
    else
        run_case mixed12 12 0-11 "$r"
        run_case slow8   8 4-11 "$r"
        run_case slow4   4 4-7  "$r"
        run_case slow2   2 4-5  "$r"
        run_case slow1   1 4-5  "$r"
        run_case fast4   4 0-3  "$r"
        run_case fast2   2 0-1  "$r"
        run_case fast1   1 0-1  "$r"
    fi
    r=$((r + 1))
done
