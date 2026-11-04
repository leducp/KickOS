#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tools/gdb/kickos.py names a stopped task's code by its program's symbols. gdb holds the kernel
# ELF alone and drives the emulator's stub; a hardware breakpoint at the app's main,
# its address read out of the one <elf>.kimg/image<n>.elf defining it, stops main's task in its own
# space, and the stop must have loaded that ELF so the symbol names the pc and `kos-space`
# reports image n. The same run without the helper is the control: the kernel ELF alone must name
# nothing at that pc. Under KICKOS_BOOT=uefi-pe firmware loads the PE32+ image and gdb holds its
# ELF twin, <elf>.elf (tools/x86_64-link.sh). Where no program ELF is beside it, <elf> is the
# merged ELF of a system running two programs at least: `break main` places one location per
# program's main, and the run stops in one.
#
#   check_gdb_space.sh <elf> <gdb> <nm> <kickos source>

set -u
. "$(dirname "$0")/../lib/gate.sh"

ELF="${1:?usage: check_gdb_space.sh <elf> <gdb> <nm> <kickos source>}"
GDB="${2:?usage: check_gdb_space.sh <elf> <gdb> <nm> <kickos source>}"
NM="${3:?usage: check_gdb_space.sh <elf> <gdb> <nm> <kickos source>}"
SRC="${4:?usage: check_gdb_space.sh <elf> <gdb> <nm> <kickos source>}"
HELPER="$SRC/tools/gdb/kickos.py"

need_qemu
need_qemu_machine
if ! command -v "$GDB" >/dev/null 2>&1; then
    echo "SKIP: $GDB not found"
    exit 77
fi
[ -f "$HELPER" ] || fail "no helper at $HELPER"
scratch_dir
boot_args "$ELF"

# gdb on the emulator's stub, with <before> run ahead of the connection and <command>... after,
# the emulator starting <late> seconds after gdb runs it.
# gdb starts the emulator in a session of its own, which `timeout` never signals, and waits for it
# to exit before gdb itself exits: the emulator carries the bound too.
# gdb waits 2 s by default for the stub's first reply, and a reply that comes later desyncs the
# connection: on a loaded host the emulator takes longer than that to start.
gdb_boot() { # <out> <symbol file> <before> <late> <command>...
    _gb_out="$1"
    _gb_file="$2"
    _gb_before="$3"
    _gb_late="$4"
    shift 4
    # A gdb session on a loaded host runs much slower than a plain boot.
    boot_bound "${QEMU_TIMEOUT:-60}"
    # QEMU_EXTRA and KOS_BOOT_ARGS are word lists, so they must split.
    # shellcheck disable=SC2086
    timeout -k "$((KOS_STOP_TICKS / 5))" "$KOS_BOOT_BOUND_S" "$GDB" -nx -batch \
        -ex 'set pagination off' -ex 'set confirm off' -ex "file $_gb_file" -ex "$_gb_before" \
        -ex "set remotetimeout $KOS_BOOT_BOUND_S" \
        -ex "target remote | sleep $_gb_late; exec timeout $((KOS_BOOT_BOUND_S - _gb_late)) \
$QEMU_BIN -M $QEMU_MACHINE ${QEMU_EXTRA:-} -display none -monitor none -serial file:$TMP/console \
$KOS_BOOT_ARGS -gdb stdio -S" \
        "$@" > "$_gb_out" 2>&1 || true
}

# Where every image sits at an address of its own, <elf> is the merged ELF and holds every
# image's symbols: each program's main is a local main of its own, which `break main` places as
# one location each, and the stop names main by the merged ELF alone.
if [ ! -f "$ELF.kimg/image0.elf" ]; then
    tool_out "$TMP/nm" '' "$NM" --defined-only "$ELF"
    MAINS="$(awk '$2 ~ /^[Tt]$/ && $3 == "main"' "$TMP/nm" | grep -c .)"
    [ "$MAINS" -ge 2 ] || fail "$ELF defines $MAINS functions named main, and the gate needs two programs"
    gdb_boot "$TMP/run" "$ELF" '' 0 -ex 'break main' -ex 'continue' -ex 'info symbol $pc' -ex 'kill'
    grep -qE "^Breakpoint 1 at 0x[0-9a-f]+: main\. \($MAINS locations\)\$" "$TMP/run" \
        || fail "break main does not place one location per program's main: $(tail -n 8 "$TMP/run")"
    grep -qE '^main( \+ [0-9]+)? in section ' "$TMP/run" \
        || fail "the run did not stop in a main: $(tail -n 8 "$TMP/run")"
    echo "PASS: break main places $MAINS locations in $(basename "$ELF") and the run stops in one"
    exit 0
fi
KERNEL="$ELF"
if [ "${KICKOS_BOOT:-kernel}" = "uefi-pe" ]; then
    KERNEL="$ELF.elf"
    [ -f "$KERNEL" ] || fail "no ELF twin at $KERNEL, so gdb holds no kernel symbol"
else
    KOS_BOOT_ARGS="-kernel $ELF"
fi

PROGRAM=""
IMAGE=""
N=0
while [ -f "$ELF.kimg/image$N.elf" ]; do
    tool_out "$TMP/nm" '' "$NM" --defined-only "$ELF.kimg/image$N.elf"
    if awk '$2 ~ /^[Tt]$/ && $3 == "main" { f = 1 } END { exit !f }' "$TMP/nm"; then
        [ -z "$PROGRAM" ] || fail "$PROGRAM and $ELF.kimg/image$N.elf both define main"
        PROGRAM="$ELF.kimg/image$N.elf"
        IMAGE="$N"
    fi
    N=$((N + 1))
done
[ -n "$PROGRAM" ] || fail "no program ELF beside $ELF defines main"
tool_out "$TMP/nm" '' "$NM" --defined-only "$PROGRAM"
MAIN="$(awk '$2 ~ /^[Tt]$/ && $3 == "main" { print $1 }' "$TMP/nm")"
[ "$(printf '%s\n' "$MAIN" | grep -c .)" = 1 ] || fail "$PROGRAM defines no single main"
if "$NM" --defined-only "$KERNEL" | awk -v a="$MAIN" '$1 == a { f = 1 } END { exit !f }'; then
    fail "the kernel ELF names 0x$MAIN itself, so the control below proves nothing"
fi

session() { # <out> [source]: break at main in its image's space, then name the pc
    _helper=""
    _condition=""
    # The control's emulator starts past gdb's default wait for its first reply.
    _late=3
    if [ "${2:-}" = source ]; then
        _helper="source $HELPER"
        _condition="condition 1 \$kos_image() == $IMAGE"
        _late=0
    fi
    gdb_boot "$1" "$KERNEL" "$_helper" "$_late" -ex "hbreak *0x$MAIN" -ex "$_condition" \
        -ex 'continue' -ex 'info symbol $pc' -ex 'kos-space' -ex 'kill'
}

session "$TMP/control"
grep -q 'No symbol matches' "$TMP/control" \
    || fail "without the helper gdb still names the pc, or never stopped there: $(tail -n 5 "$TMP/control")"

session "$TMP/run" source
grep -qE "^main in section [^ ]+ of $PROGRAM\$" "$TMP/run" \
    || fail "the stop at main did not load $PROGRAM: $(tail -n 8 "$TMP/run")"
grep -qF "runs image $IMAGE, $PROGRAM" "$TMP/run" \
    || fail "kos-space did not report image $IMAGE: $(tail -n 8 "$TMP/run")"
echo "PASS: a stop in main's task loads $(basename "$PROGRAM") and names main, which the kernel ELF alone does not"
