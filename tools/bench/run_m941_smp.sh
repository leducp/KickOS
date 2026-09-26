#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Run the x86 pinned-worker sweep under KVM and retain its full console capture.
# Usage: run_m941_smp.sh <bench_smp.efi> <cores> <output-dir> [host-cpu-set]

set -u

fail() { echo "FAIL: $*" >&2; exit 1; }
[ "$#" -eq 3 ] || [ "$#" -eq 4 ] ||
    fail "usage: $0 <bench_smp.efi> <cores> <output-dir> [host-cpu-set]"
app="$1"
cores="$2"
out="$3"
cpuset="${4:-}"
case "$cores" in 1|2|4|8|12) ;; *) fail "unsupported core count: $cores" ;; esac
vcpus="${KICKOS_M941_VCPUS:-$cores}"
case "$vcpus" in 1|2|4|8|12) ;; *) fail "unsupported QEMU vCPU count: $vcpus" ;; esac
[ "$vcpus" -ge "$cores" ] || fail "QEMU exposes fewer vCPUs than KickOS drives"
[ -f "$app" ] || fail "no image at $app"
[ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] || fail "OVMF code file missing"
[ -f /usr/share/OVMF/OVMF_VARS_4M.fd ] || fail "OVMF variable store missing"
command -v qemu-system-x86_64 >/dev/null 2>&1 || fail "qemu-system-x86_64 missing"
command -v timeout >/dev/null 2>&1 || fail "timeout missing"
if [ -n "$cpuset" ]; then
    command -v python3 >/dev/null 2>&1 || fail "python3 missing"
fi

mkdir -p "$out" || fail "cannot create $out"
root="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
"$root/tools/esp-x86_64.sh" "$app" "$out/esp.img" || fail "ESP build failed"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd" || fail "variable store copy failed"

rc=0
set -- qemu-system-x86_64 -M q35,accel=kvm -cpu host -smp "$vcpus" \
    -nographic -m 512 -net none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive "format=raw,file=$out/esp.img" \
    -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive "if=pflash,format=raw,unit=1,file=$out/vars.fd"
if [ -n "$cpuset" ]; then
    python3 "$root/tools/bench/m95_pin_qemu.py" "$out" "$cpuset" "$vcpus" "$@" || rc=$?
else
    timeout 90 "$@" > "$out/run.log" 2>&1 || rc=$?
fi

# isa-debug-exit translates guest status zero to host exit code one.
[ "$rc" -eq 1 ] || fail "QEMU exit $rc; see $out/run.log"
tr -d '\r' < "$out/run.log" > "$out/run.txt"
grep -qx 'smp-lock: done' "$out/run.txt" || fail "no completion marker in $out/run.txt"
grep -qx 'KICKOS-EXIT status 0' "$out/run.txt" || fail "guest did not exit cleanly"
grep -E '^smp-lock: (core=|cores=)' "$out/run.txt"
