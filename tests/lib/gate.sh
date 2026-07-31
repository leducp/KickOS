# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Primitives shared by the gate scripts. SOURCED, never executed:
#   . "$(dirname "$0")/lib/gate.sh"
# POSIX sh (dash-clean), because /bin/sh is dash on the CI images.

# The reporters' literal dump markers, as one ERE: kpanic (which KICKOS_UNREACHABLE
# routes through), the armv7m/armv6m/sim/riscv fault reporters, kickos_isr_fault, the
# K64F SYSMPU hook. Case-sensitive and anchored on the banner shape, because a
# substring match on "fault" also hits "EFAULT" and "default" in benign output.
# cmake/kickos.cmake SCRAPES this assignment for the CTest FAIL_REGULAR_EXPRESSION
# registrations, so it must stay one line of the form KOS_PANIC_RE='<ere>'.
KOS_PANIC_RE='KERNEL PANIC:|=== (HARD|MPU|SIM) FAULT|=== RISC-V TRAP|MPU FAULT: task|ISOLATION FAULT:'

fail() { echo "FAIL: $*" >&2; exit 1; }

# TMP: a fresh directory, removed on exit.
scratch_dir() {
    TMP="$(mktemp -d)" || fail "mktemp -d failed"
    trap 'rm -rf "$TMP"' EXIT
}

# A tool that produced nothing leaves every grep below it vacuously satisfied.
require_nonempty() {
    if [ ! -s "$1" ]; then
        fail "$2"
    fi
}

# The same trap one level up: a binutils invocation that FAILED also produces nothing,
# so every absence-assertion reading its output concludes "clean". Route every
# invocation through here. The landmark is a positive control (a section, a symbol
# shape) that a healthy run cannot lack, so a tool that merely emitted a banner is
# caught too. Exits on the spot; a broken tool is not a result to accumulate. Does not
# go through fail(), because a gate may legitimately redefine that to accumulate.
tool_out() { # <outfile> <landmark-ere, empty for success-only> <tool> <arg>...
    _o="$1"
    _mark="$2"
    shift 2
    # Inside the condition so a `set -e` caller does not abort before $? is read.
    if "$@" > "$_o" 2>"$_o.err"; then
        _rc=0
    else
        _rc=$?
    fi
    if [ "$_rc" -ne 0 ]; then
        echo "FAIL: exit $_rc from: $*" >&2
        sed -n '1,3p' "$_o.err" >&2
        exit 1
    fi
    if [ -n "$_mark" ] && ! grep -qE "$_mark" "$_o"; then
        echo "FAIL: nothing matching /$_mark/ came out of: $*" >&2
        exit 1
    fi
}

# QEMU_BIN: the emulator this board needs. Exit 77 -> CTest SKIP (not PASS), so a
# QEMU-less box cannot green-light a boot gate. Not a command substitution: the exit
# has to leave the SCRIPT, not a subshell.
need_qemu() {
    QEMU_BIN="${QEMU:-qemu-system-arm}"
    if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
        echo "SKIP: $QEMU_BIN not found"
        exit 77
    fi
}

# For a gate that only ever runs under QEMU. kickos_add_qemu_test always exports
# QEMU_MACHINE, so an unset one means the script was run bare; refusing beats the old
# per-script mps2-an386 default, which silently targeted the wrong core.
need_qemu_machine() {
    if [ -z "${QEMU_MACHINE:-}" ]; then
        fail "QEMU_MACHINE is unset; this gate boots on QEMU only"
    fi
}

# OUT: the image's combined output, CR-stripped (a KICKOS_CONSOLE_CRLF board emits CR,
# which defeats end-anchored parses). RC: its exit status, which a pipeline here would
# replace with tr's. QEMU when QEMU_MACHINE is set (kickos_add_qemu_test always exports
# it), native otherwise.
run_image() {
    if [ -n "${QEMU_MACHINE:-}" ]; then
        need_qemu
        # QEMU_EXTRA is a word list (e.g. `-bios none`), so it must split.
        # shellcheck disable=SC2086
        OUT="$(timeout "${QEMU_TIMEOUT:-20}" "$QEMU_BIN" -M "$QEMU_MACHINE" ${QEMU_EXTRA:-} \
                 -nographic -semihosting -kernel "$1" 2>&1)"
    else
        OUT="$(timeout "${SIM_TIMEOUT:-20}" "$1" 2>&1)"
    fi
    RC=$?
    OUT="$(printf '%s\n' "$OUT" | tr -d '\r')"
    printf '%s\n' "$OUT"
}

# For an app that never terminates on its own: boot it in the background and poll its
# output until EVERY pattern has appeared, then stop QEMU. POLL_OK is 1 when they all
# landed, 0 when the poll ran out or QEMU died first; OUT carries the whole run either
# way. QEMU_TIMEOUT bounds only the no-progress path.
poll_image() { # <elf> <ere>...
    need_qemu
    need_qemu_machine
    _elf="$1"
    shift
    _log="$(mktemp)" || fail "mktemp failed"
    # QEMU_EXTRA is a word list (e.g. `-bios none`), so it must split.
    # shellcheck disable=SC2086
    "$QEMU_BIN" -M "$QEMU_MACHINE" ${QEMU_EXTRA:-} \
        -nographic -semihosting -kernel "$_elf" >"$_log" 2>&1 &
    _qpid=$!
    _n=0
    while [ "$_n" -lt $(( ${QEMU_TIMEOUT:-8} * 5 )) ]; do   # poll at 5 Hz
        if _poll_matched "$_log" "$@"; then
            break
        fi
        kill -0 "$_qpid" 2>/dev/null || break               # QEMU exited on its own
        sleep 0.2
        _n=$((_n + 1))
    done
    { kill "$_qpid"; wait "$_qpid"; } 2>/dev/null
    # Judged on the FINAL log: an image that exited between the last poll and the
    # liveness check has everything on the wire and must not read as no-progress.
    POLL_OK=0
    if _poll_matched "$_log" "$@"; then
        POLL_OK=1
    fi
    OUT="$(tr -d '\r' < "$_log")"
    rm -f "$_log"
    printf '%s\n' "$OUT"
}

_poll_matched() { # <log> <ere>...
    _l="$1"
    shift
    for _p in "$@"; do
        grep -qE "$_p" "$_l" || return 1
    done
    return 0
}

# grep OUT as a predicate, with `set -e` kept out of the way.
has() { printf '%s\n' "$OUT" | grep -q "$1"; }
has_e() { printf '%s\n' "$OUT" | grep -qE "$1"; }

assert_no_panic() {
    if has_e "$KOS_PANIC_RE"; then
        fail "$1"
    fi
}

# The faulting address the reporters recorded, as bare hex digits: armv7m dumps MMFAR
# (MemManage) or BFAR (BusFault), the kernel-reported path (sim, RISC-V) names it in
# its MPU FAULT line. Both are printed only when the address is live, never stale.
reported_fault_addr() {
    printf '%s\n' "$OUT" \
        | sed -n -e 's/.*MMFAR=0x\([0-9a-fA-F]*\).*/\1/p' \
                 -e 's/.*BFAR=0x\([0-9a-fA-F]*\).*/\1/p' \
                 -e 's/.*attempted [a-z]* at 0x\([0-9a-fA-F]*\).*/\1/p' \
        | head -n1
}
