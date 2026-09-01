# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Primitives shared by the gate scripts. SOURCED, never executed:
#   . "$(dirname "$0")/../lib/gate.sh"
# POSIX sh (dash-clean), because /bin/sh is dash on the CI images.

# Every reporter's literal dump marker, as one ERE. Case-sensitive and anchored on the
# banner shape, because a substring match on "fault" also hits "EFAULT" and "default" in
# benign output.
#
# It lives in tests/lib/panic.ere, ONE line, and has two consumers: this file and the root
# CMakeLists, which registers it as a ctest FAIL_REGULAR_EXPRESSION. Read once, and REFUSE an
# empty result: an empty ERE matches nothing, so every panic gate in the suite would silently
# stop failing.
KOS_PANIC_RE="$(cat "$(dirname "$0")/../lib/panic.ere")"
if [ -z "$KOS_PANIC_RE" ]; then
    echo "FAIL: tests/lib/panic.ere is empty or unreadable; every panic gate would pass" >&2
    exit 1
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

# A literal tab, for `while IFS="$TAB" read -r ...` over tab-separated records. NOT $'\t':
# that is a bashism, and dash sets IFS to the three characters $ \ t instead, so every field
# splits on those. `dash -n` passes it and a gate whose records mis-split goes VACUOUS rather
# than loud, so only a run under dash shows it.
TAB="$(printf '\t')"

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

# The same trap one level up: a binutils invocation that FAILED also produces nothing, so
# every absence-assertion reading its output concludes "clean". Route every invocation
# through here. The landmark is a positive control (a section, a symbol shape) that a healthy
# run cannot lack, so a tool that merely emitted a banner is caught too. Exits on the spot,
# and not through fail(), which a gate may legitimately redefine to accumulate.
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

# HOW AN IMAGE IS HANDED TO THE EMULATOR, per board, into KOS_BOOT_ARGS as a word list the
# two runners below leave unquoted. The default is `-semihosting -kernel <elf>`.
#
# KICKOS_BOOT=uefi-pe is x86_64, and -kernel cannot start that image at all: firmware loads a
# PE32+ UEFI application, so the image goes into an EFI system partition and boots off the
# removable-media fallback path. Three things are per run. The ESP is rebuilt from the image
# UNDER TEST, because a stale BOOTX64.EFI left in a reused volume boots instead and prints the
# same banner. The variable store is copied, because the shipped one is root-owned and
# read-only and firmware writes to it. And -no-reboot is not cosmetic: an absent or malformed
# interrupt table triple-faults, which RESETS the machine, so without it the run loops instead
# of ending.
#
# The scratch lives beside the image and not under /tmp: it is tens of megabytes, and the
# gates that use it also call scratch_dir(), whose EXIT trap would replace any trap set here.
KOS_OVMF_CODE="${KICKOS_OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
KOS_OVMF_VARS="${KICKOS_OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"

boot_args() { # <image>
    if [ "${KICKOS_BOOT:-kernel}" != "uefi-pe" ]; then
        KOS_BOOT_ARGS="-semihosting -kernel $1"
        return
    fi
    for _t in dd mformat mmd mcopy mdir; do
        if ! command -v "$_t" >/dev/null 2>&1; then
            echo "SKIP: $_t not found (Debian: mtools, coreutils)"
            exit 77
        fi
    done
    if [ ! -f "$KOS_OVMF_CODE" ] || [ ! -f "$KOS_OVMF_VARS" ]; then
        echo "SKIP: no UEFI firmware at $KOS_OVMF_CODE / $KOS_OVMF_VARS (Debian: ovmf)"
        exit 77
    fi
    KOS_BOOT_DIR="$1.boot"
    rm -rf "$KOS_BOOT_DIR"
    mkdir -p "$KOS_BOOT_DIR" || fail "cannot create $KOS_BOOT_DIR"
    "$(dirname "$0")/../../tools/esp-x86_64.sh" "$1" "$KOS_BOOT_DIR/esp.img" >/dev/null \
        || fail "could not build the EFI system partition for $1"
    cp "$KOS_OVMF_VARS" "$KOS_BOOT_DIR/vars.fd" || fail "cannot copy $KOS_OVMF_VARS"
    chmod u+w "$KOS_BOOT_DIR/vars.fd" || fail "cannot make $KOS_BOOT_DIR/vars.fd writable"
    KOS_BOOT_ARGS="-m 512 -net none -no-reboot \
-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
-drive format=raw,file=$KOS_BOOT_DIR/esp.img \
-drive if=pflash,format=raw,unit=0,readonly=on,file=$KOS_OVMF_CODE \
-drive if=pflash,format=raw,unit=1,file=$KOS_BOOT_DIR/vars.fd"
}

# The status line an image prints for itself, as a sed BRE with the number in \1. Restated from
# arch/x86/chip/q35/chip_q35.cc on purpose: a parse derived from the emitter would assert
# nothing about it. Anchored, because an unanchored match would also take a line quoting it.
# Deliberately not banner-shaped: tests/static/check_panic_banners.sh reads a `=== x ===`
# literal as a fault reporter's marker and would demand panic.ere match an ordinary exit.
KOS_EXIT_LINE_RE='KICKOS-EXIT status \([0-9][0-9]*\)'

# KOS_STATUS: the image's OWN exit status, out of whatever the machine reports.
#
# THE WHOLE MECHANISM IS GATED ON KICKOS_BOOT=uefi-pe. Only the x86_64 posture has a status
# channel too narrow to carry a byte: isa-debug-exit reports (status << 1) | 1 into an 8-bit
# process exit code, so only 0 through 127 round-trip and 139 arrives as 11. Every other board's
# emulator reports the image's status directly, and below this gate they take the raw status
# untouched.
#
# THE TWO CHANNELS CROSS-CHECK, and neither is trusted alone. On the gated posture the console is
# polled and the image runs an unprivileged thread that can write kernel memory, so a printed
# line is a claim an application could make about itself; the device write is privileged and its
# code comes from the emulator. So the printed value is used only where the device corroborated
# it: the recovery must equal the printed status modulo 128. A printed line with NO device report
# is refused rather than believed, which also catches a run configured without the device.
#
# The timeout code is never overridden: an image killed for making no progress must read as
# killed, and a status line it printed before the kill is exactly what would hide that.
#
# NEITHER CHANNEL MOVES THE RAW STATUS ALONE, and the console must speak first: arch_shutdown
# prints the line before it writes the device, so a device report with no line on the wire is
# not this image exiting. An emptiness test does not catch that: run_image folds QEMU's own
# stderr into this argument, so only a SILENT failure ever had an empty one.
#
# Sets a variable rather than printing: a caller's `$(boot_status ...)` would confine fail()
# to a subshell, so a disagreement would be reported and then discarded.
boot_status() { # <raw status> <output>
    KOS_STATUS="$1"
    if [ "$1" -eq 124 ]; then
        return
    fi
    if [ "${KICKOS_BOOT:-kernel}" != "uefi-pe" ]; then
        return
    fi
    _printed="$(printf '%s\n' "$2" | sed -n "s/^$KOS_EXIT_LINE_RE\$/\1/p" | tail -n1)"
    if [ -z "$_printed" ]; then
        return
    fi
    # The device encodes (status << 1) | 1, so every report it makes is odd.
    if [ $(($1 % 2)) -ne 1 ]; then
        fail "the image printed status $_printed and the exit device reported nothing (raw $1):
  the printed line is not corroborated, so it is not used"
    fi
    _recovered="$((($1 - 1) / 2))"
    if [ "$_recovered" -ne "$((_printed % 128))" ]; then
        fail "the image printed status $_printed, whose low seven bits are $((_printed % 128)),
  but the exit device reported $_recovered (raw $1): one of the two is broken
  (arch_shutdown's status line, or this recovery)"
    fi
    KOS_STATUS="$_printed"
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
# QEMU_MACHINE, so an unset one means the script was run bare, and a per-script default
# would silently target the wrong core.
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
        boot_args "$1"
        # QEMU_EXTRA and KOS_BOOT_ARGS are word lists (e.g. `-bios none`), so they must split.
        # shellcheck disable=SC2086
        OUT="$(timeout "${QEMU_TIMEOUT:-20}" "$QEMU_BIN" -M "$QEMU_MACHINE" ${QEMU_EXTRA:-} \
                 -nographic ${KOS_BOOT_ARGS} 2>&1)"
    else
        OUT="$(timeout "${SIM_TIMEOUT:-20}" "$1" 2>&1)"
    fi
    RC=$?
    OUT="$(printf '%s\n' "$OUT" | tr -d '\r')"
    boot_status "$RC" "$OUT"
    RC="$KOS_STATUS"
    printf '%s\n' "$OUT"
}

# For an app that never terminates on its own: boot it in the background and poll its
# output until EVERY pattern has appeared, then stop it. POLL_OK is 1 when they all
# landed, 0 when the poll ran out or the image died first; OUT carries the whole run
# either way. QEMU_TIMEOUT bounds only the no-progress path. POLL_MS is how long the poll
# ran for, at the resolution of its own tick, and POLL_ALIVE is 0 when the image ended before
# the poll did: a bound that ran out and an image that stopped early are different findings.
#
# KOS_POLL_UNTIL names a shell FUNCTION the poll re-evaluates on every tick beside the
# patterns, satisfied when it returns 0; the poll stops when the patterns AND the function
# are both satisfied. POLL_UNTIL_OK carries its final verdict SEPARATELY from POLL_OK: a
# caller whose bound expires has to say which of the two it was still waiting for, and the
# function is what knows what is outstanding.
#
# A caller's function is called in THIS shell, so what it records stays readable after the
# poll; it must not exit, a poll tick being no place to reach a verdict.
KOS_POLL_UNTIL=""

poll_image() { # <elf> <ere>...
    _elf="$1"
    shift
    _log="$(mktemp)" || fail "mktemp failed"
    if [ -n "${QEMU_MACHINE:-}" ]; then
        need_qemu
        boot_args "$_elf"
        # QEMU_EXTRA and KOS_BOOT_ARGS are word lists (e.g. `-bios none`), so they must split.
        # shellcheck disable=SC2086
        "$QEMU_BIN" -M "$QEMU_MACHINE" ${QEMU_EXTRA:-} \
            -nographic ${KOS_BOOT_ARGS} >"$_log" 2>&1 &
    else
        "$_elf" >"$_log" 2>&1 &
    fi
    _qpid=$!
    _n=0
    POLL_ALIVE=1
    while [ "$_n" -lt $(( ${QEMU_TIMEOUT:-8} * 5 )) ]; do   # poll at 5 Hz
        if _poll_matched "$_log" "$@" && _poll_until; then
            break
        fi
        if ! kill -0 "$_qpid" 2>/dev/null; then             # the image exited on its own
            POLL_ALIVE=0
            break
        fi
        sleep 0.2
        _n=$((_n + 1))
    done
    { kill "$_qpid"; wait "$_qpid"; } 2>/dev/null
    POLL_MS=$((_n * 200))
    # Judged on the FINAL log: an image that exited between the last poll and the
    # liveness check has everything on the wire and must not read as no-progress. Both
    # conditions are evaluated, and not short-circuited: each one's verdict is reported on
    # its own, and the second is what records what it is still short of.
    POLL_OK=0
    if _poll_matched "$_log" "$@"; then
        POLL_OK=1
    fi
    POLL_UNTIL_OK=0
    if _poll_until; then
        POLL_UNTIL_OK=1
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

# An unset KOS_POLL_UNTIL is SATISFIED, so a caller that names no function polls on the
# patterns alone.
_poll_until() {
    if [ -z "${KOS_POLL_UNTIL:-}" ]; then
        return 0
    fi
    "$KOS_POLL_UNTIL"
}

# grep OUT as a predicate, with `set -e` kept out of the way.
has() { printf '%s\n' "$OUT" | grep -q "$1"; }
has_e() { printf '%s\n' "$OUT" | grep -qE "$1"; }

assert_no_panic() {
    if has_e "$KOS_PANIC_RE"; then
        fail "$1"
    fi
}

# The faulting address the reporters recorded, as bare hex digits, from whichever spelling
# this backend uses. Every one of them is printed only when the address is live, so a match
# is never a stale register.
reported_fault_addr() {
    printf '%s\n' "$OUT" \
        | sed -n -e 's/.*MMFAR=0x\([0-9a-fA-F]*\).*/\1/p' \
                 -e 's/.*BFAR=0x\([0-9a-fA-F]*\).*/\1/p' \
                 -e 's/.*ADDR=0x\([0-9a-fA-F]*\).*/\1/p' \
                 -e 's/.*attempted [a-z]* at 0x\([0-9a-fA-F]*\).*/\1/p' \
        | head -n1
}

# The thread-kill dump's banner for a named thread, as an ERE; kernel/init/fault.cc owns
# the wording, and four gates pin it through here.
thread_fault_re() { # <thread-name>
    printf "=== THREAD FAULT === thread '%s' killed" "$1"
}

# WHAT THE RV64 FAULT GATES SHARE. Each caller keeps its own markers, its own scause constant,
# its own address and the prose every refusal here prints.
#
# The image is expected to fault, so an `ERROR:` line is the image failing to arrange the fault
# rather than the fault under test. OUT and RC carry the run, as after run_image.
run_faulting_image() { # <image>
    need_qemu_machine
    run_image "$1"
    if has "ERROR:"; then
        printf '%s\n' "$OUT" | grep 'ERROR:'
        fail "the image reported a failure instead of faulting"
    fi
}

# WHAT THE THREE HELPERS BELOW REFUSE BEFORE THEY ASSERT ANYTHING. `[ "" -ne 139 ]` is an ERROR
# in test(1) and not a false condition, and `grep -F -e ""` matches every line, so an empty
# marker and an empty count each turn a refusal into a pass.
require_number() { # <value> <what>
    case "$1" in
        ''|*[!0-9]*) fail "$2 must be a decimal number, got [$1]" ;;
    esac
}

require_literal() { # <value> <what>
    if [ -z "$1" ]; then
        fail "$2 is empty, so every line of the run would match it"
    fi
}

# KOS_COUNT: occurrences of a LITERAL in OUT. grep exits 1 for no match and above 1 for a
# failure of its own, which prints no count at all; taking that for a zero is what lets a
# broken invocation read as an absence the caller then judges.
count_literal() { # <literal>
    _cl_n="$(printf '%s\n' "$OUT" | grep -c -F -e "$1")"
    _cl_rc=$?
    if [ "$_cl_rc" -gt 1 ]; then
        fail "exit $_cl_rc from grep -F -e '$1': the count is UNKNOWN and not zero"
    fi
    require_number "$_cl_n" "the count of '$1'"
    KOS_COUNT="$_cl_n"
}

# KOS_FIELD_N: occurrences of a record `<name>=<value>` in <text>, the value WHOLE. A
# substring count reads `scause=0xdead` as a hit for `scause=0xd` and `ADDR=0x80201000` as a hit
# for `ADDR=0x8020100`. The value ends at the first character that could not continue it, or at
# end of line.
#
# A name or value carrying anything but an identifier character REFUSES rather than being
# escaped.
field_count() { # <text> <name> <value>
    require_literal "$2" "the field name"
    require_literal "$3" "the field value"
    case "$2$3" in
        *[!0-9A-Za-z_]*)
            fail "the field record '$2=$3' holds a character this matcher does not model, so
      its count is UNKNOWN and not zero" ;;
    esac
    _fc_n="$(printf '%s\n' "$1" | grep -c -E "(^|[^0-9A-Za-z_])$2=$3([^0-9A-Za-z_]|\$)")"
    _fc_rc=$?
    if [ "$_fc_rc" -gt 1 ]; then
        fail "exit $_fc_rc from grep while counting '$2=$3': the count is UNKNOWN and not zero"
    fi
    require_number "$_fc_n" "the count of '$2=$3'"
    KOS_FIELD_N="$_fc_n"
}

# The same count over OUT.
count_field() { # <name> <value>
    field_count "$OUT" "$1" "$2"
    KOS_COUNT="$KOS_FIELD_N"
}

# The matcher, before it is asked to report an absence. Both directions, because only one of
# them is the defect: a planted record must count ONCE for its own value and NOT AT ALL for a
# value it merely begins with. A prefix matcher passes the first control and fails the second.
field_matcher_control() {
    _fmc="  PC=0x80001234 scause=0xdead
  ADDR=0x80201000"
    field_count "$_fmc" ADDR 0x80201000
    [ "$KOS_FIELD_N" -eq 1 ] \
        || fail "the field matcher counted $KOS_FIELD_N hit(s) for the address a planted
      record names, so it cannot find the record the assertions below rest on"
    field_count "$_fmc" ADDR 0x8020100
    [ "$KOS_FIELD_N" -eq 0 ] \
        || fail "the field matcher counts a planted ADDR=0x80201000 as a hit for
      ADDR=0x8020100, so a fault at a longer address passes as the address asserted"
    field_count "$_fmc" scause 0xdead
    [ "$KOS_FIELD_N" -eq 1 ] \
        || fail "the field matcher counted $KOS_FIELD_N hit(s) for the cause a planted record
      names"
    field_count "$_fmc" scause 0xd
    [ "$KOS_FIELD_N" -eq 0 ] \
        || fail "the field matcher counts a planted scause=0xdead as a hit for scause=0xd, so
      a fault with a longer cause code passes as the cause asserted"
    field_count "$_fmc" ADDR 0x80201001
    [ "$KOS_FIELD_N" -eq 0 ] \
        || fail "the field matcher reports an address the planted record does not carry"
}

# Exactly ONE occurrence of a gate's fault-dump marker. The absence prose is the caller's,
# because what a missing dump means is the whole of what that gate asserts; a second occurrence
# is a repeated fault and says the same thing everywhere.
#
# Never a control marker: a control's absence and its repetition are two different findings.
# check_aspace_ufault_rv64.sh asserts its control on its own lines.
require_single_marker() { # <marker> <absence-prose>
    require_literal "$1" "the fault-dump marker"
    count_literal "$1"
    if [ "$KOS_COUNT" -eq 0 ]; then
        fail "fault-dump marker '$1' missing: $2"
    fi
    if [ "$KOS_COUNT" -ne 1 ]; then
        fail "fault-dump marker '$1' appeared $KOS_COUNT times"
    fi
}

# The three assertions an RV64 fault record carries: the address the caller computed, the cause
# the caller spells out as a scause constant, and the image's exit status. Every refusal prints
# the record's own lines first. Each field is matched WHOLE, through field_count above, whose
# control runs here before any of the three is judged.
#
# scause is the whole of what this architecture publishes about the access: no fault-status
# field and no level field sits beside it (RISC-V Privileged ISA, Supervisor Cause Register).
require_rv64_fault_at() { # <addr-hex> <addr-prose> <scause-hex> <cause-prose> <expect-status>
    require_literal "$1" "the faulting address"
    require_literal "$3" "the scause constant"
    require_number "$5" "the expected exit status"
    require_number "$RC" "the status the run reported"
    field_matcher_control
    count_field ADDR "0x$1"
    if [ "$KOS_COUNT" -eq 0 ]; then
        printf '%s\n' "$OUT" | grep -E 'ADDR=|scause='
        fail "the record faults somewhere other than 0x$1, $2"
    fi
    count_field scause "$3"
    if [ "$KOS_COUNT" -eq 0 ]; then
        printf '%s\n' "$OUT" | grep -E 'scause='
        fail "the cause is not $4"
    fi
    if [ "$RC" -ne "$5" ]; then
        fail "expected exit $5, got $RC"
    fi
}
