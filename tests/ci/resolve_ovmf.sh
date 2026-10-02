#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Find the OVMF firmware code and the variable store that matches it, and print them as
# KICKOS_OVMF_CODE=<path> and KICKOS_OVMF_VARS=<path>, the form $GITHUB_ENV reads.
#
# usage: resolve_ovmf.sh >> "$GITHUB_ENV"
#
# The pair is matched by name, OVMF_VARS beside OVMF_CODE, so the two sizes agree. The shortest
# code name wins, which keeps a secure-boot build away from an unsigned image.
set -u

ovmf_dirs="/usr/share/OVMF /usr/share/ovmf /usr/share/edk2/x64 /usr/share/edk2-ovmf/x64 /usr/share/qemu"
ovmf_code=""
ovmf_vars=""
for d in $ovmf_dirs; do
    for c in "$d"/OVMF_CODE*.fd; do
        [ -f "$c" ] || continue
        v=$(printf '%s' "$c" | sed 's/OVMF_CODE/OVMF_VARS/')
        [ -f "$v" ] || continue
        if [ -z "$ovmf_code" ] || [ "${#c}" -lt "${#ovmf_code}" ]; then
            ovmf_code="$c"
            ovmf_vars="$v"
        fi
    done
    [ -z "$ovmf_code" ] || break
done
if [ -z "$ovmf_code" ]; then
    echo "no OVMF_CODE*.fd beside a matching OVMF_VARS store under: $ovmf_dirs" >&2
    echo "the ovmf package names those files, so a release that spells them otherwise" >&2
    echo "needs its directory added above rather than a path corrected" >&2
    exit 1
fi
echo "resolved firmware: $ovmf_code with $ovmf_vars" >&2
echo "KICKOS_OVMF_CODE=$ovmf_code"
echo "KICKOS_OVMF_VARS=$ovmf_vars"
