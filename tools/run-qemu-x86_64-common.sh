# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The machine the five M6.4 x86_64 witnesses boot, and the serial capture their arms read.
# SOURCED (not run) by tools/run-qemu-x86_64.sh and every tools/run-qemu-x86_64-xN.sh.
#
# The sourcing script sets these before calling kos_boot, and keeps the environment knobs they
# come from (KICKOS_XN_FIRMWARE and friends) beside its own arms:
#   KOS_STEP       X1 .. X5, named in the diagnostics
#   KOS_TOKEN      the token this step's arms hold the image to
#   KOS_FIRMWARE   pflash (split OVMF_CODE_4M plus OVMF_VARS_4M) or bios (combined OVMF.fd)
#   KOS_MACHINE    qemu machine type
#   KOS_TIMEOUT    seconds
#   KOS_WORK_LEAF  workdir under the image's own directory, used when the caller passes none
#   KOS_END        halt: the image halts, so the emulator is killed once its last line lands.
#                  exit: the image writes isa-debug-exit, so the emulator's own status is
#                  waited for and carries the image's.
#
# kos_boot <application.efi> [workdir] then sets, for the arms that follow it:
#   WORK     the resolved workdir
#   PLAIN    the CR-stripped serial capture every arm reads
#   TOK      KOS_TOKEN, escaped for a grep pattern
#   QEMU_RC  the emulator's exit status, 0 under KOS_END=halt where nothing reports one
#
# POSIX sh (dash-clean).

fail() { echo "FAIL: $*" >&2; exit 1; }

KOS_OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
KOS_OVMF_VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
KOS_OVMF_COMBINED=/usr/share/ovmf/OVMF.fd

QEMU_RC=0
KOS_QEMU_PID=""

# The emulator, once, with the firmware drives passed in. isa-debug-exit is on the machine
# for every step, not only the three that write it.
#
# The run is ALWAYS backgrounded, so KOS_QEMU_PID is the timeout's own pid. A wrapper around it
# would be what the halt path killed, and the emulator would then run on to its timeout.
kos_run_qemu() { # <firmware-specific qemu args>...
    echo "run: $KOS_QEMU ${KOS_CPU_ARGS} -machine $KOS_MACHINE -m 512 -display none" \
         "-no-reboot -net none -device isa-debug-exit,iobase=0xf4,iosize=0x04" \
         "-serial file:$KOS_LOG -drive format=raw,file=$KOS_ESP $*"
    # KOS_CPU_ARGS is a word list and must split.
    # shellcheck disable=SC2086
    timeout "$KOS_TIMEOUT" "$KOS_QEMU" ${KOS_CPU_ARGS} \
        -machine "$KOS_MACHINE" -m 512 -display none -no-reboot -net none \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -serial "file:$KOS_LOG" -drive "format=raw,file=$KOS_ESP" "$@" \
        >"$WORK/qemu.out" 2>"$WORK/qemu.err" &
    KOS_QEMU_PID=$!
    if [ "$KOS_END" != halt ]; then
        wait "$KOS_QEMU_PID"
        QEMU_RC=$?
    fi
}

# Boot one application and leave its capture in PLAIN.
kos_boot() { # <application.efi> [workdir]
    APP="$1"
    WORK="${2:-}"

    [ -f "$APP" ] || fail "no application at $APP"
    if [ -z "$WORK" ]; then
        WORK="$(dirname "$APP")/$KOS_WORK_LEAF"
    fi
    mkdir -p "$WORK" || fail "cannot create $WORK"

    KOS_QEMU="$(command -v qemu-system-x86_64)" || fail "qemu-system-x86_64 is not installed"
    command -v timeout >/dev/null 2>&1 || fail "timeout is not installed (Debian: coreutils)"

    # KICKOS_X86_64_CPU names a -cpu model, and nothing is passed when it is unset. It is how
    # a witness is taken on a processor model other than the emulator's default:
    # `-cpu qemu64,nx=off` varies the execute-disable bit.
    KOS_CPU_ARGS=""
    if [ -n "${KICKOS_X86_64_CPU:-}" ]; then
        KOS_CPU_ARGS="-cpu ${KICKOS_X86_64_CPU}"
    fi

    KOS_ESP="$WORK/esp.img"
    "$KOS_TOOLS/esp-x86_64.sh" "$APP" "$KOS_ESP" || fail "could not build the ESP"

    KOS_LOG="$WORK/serial.log"
    rm -f "$KOS_LOG"

    case "$KOS_FIRMWARE" in
        pflash)
            [ -f "$KOS_OVMF_CODE" ] || fail "no firmware at $KOS_OVMF_CODE (Debian: ovmf)"
            [ -f "$KOS_OVMF_VARS" ] \
                || fail "no variable store at $KOS_OVMF_VARS (Debian: ovmf)"
            # The shipped store is root-owned and read-only; firmware writes to it.
            KOS_VARS="$WORK/OVMF_VARS.fd"
            cp "$KOS_OVMF_VARS" "$KOS_VARS" \
                || fail "cannot copy $KOS_OVMF_VARS to $KOS_VARS"
            chmod u+w "$KOS_VARS" || fail "cannot make $KOS_VARS writable"
            kos_run_qemu \
                -drive "if=pflash,format=raw,unit=0,readonly=on,file=$KOS_OVMF_CODE" \
                -drive "if=pflash,format=raw,unit=1,file=$KOS_VARS"
            ;;
        bios)
            [ -f "$KOS_OVMF_COMBINED" ] \
                || fail "no firmware at $KOS_OVMF_COMBINED (Debian: ovmf)"
            kos_run_qemu -bios "$KOS_OVMF_COMBINED"
            ;;
        *)
            fail "KICKOS_${KOS_STEP}_FIRMWARE must be pflash or bios, not '$KOS_FIRMWARE'"
            ;;
    esac

    if [ "$KOS_END" = halt ]; then
        kos_waited=0
        while [ "$kos_waited" -lt "$KOS_TIMEOUT" ]; do
            if [ -s "$KOS_LOG" ] && grep -q "halting\|FAIL" "$KOS_LOG" 2>/dev/null; then
                break
            fi
            if ! kill -0 "$KOS_QEMU_PID" 2>/dev/null; then
                break
            fi
            sleep 1
            kos_waited=$((kos_waited + 1))
        done
        kill "$KOS_QEMU_PID" 2>/dev/null
        wait "$KOS_QEMU_PID" 2>/dev/null
        if [ "$kos_waited" -ge "$KOS_TIMEOUT" ]; then
            echo "note: the image never reached its last line within ${KOS_TIMEOUT}s" >&2
        fi
    else
        echo "qemu exit: $QEMU_RC"
        if [ "$QEMU_RC" = 124 ]; then
            echo "note: the image never reached arch_shutdown within ${KOS_TIMEOUT}s" >&2
        fi
    fi

    [ -s "$KOS_LOG" ] || fail "no serial output at all in $KOS_LOG"
    # Firmware drives the serial line as a terminal, so every assertion reads a CR-stripped
    # copy and anchors on end of line.
    PLAIN="$WORK/serial.txt"
    tr -d '\r' < "$KOS_LOG" > "$PLAIN" || fail "cannot strip CR from $KOS_LOG"
    TOK="$(printf '%s' "$KOS_TOKEN" | sed 's/[][\.*^$/]/\\&/g')"
}

# The pattern is the caller's; only the reading of the capture is shared.
need() { # <what> <bre>
    grep -q "$2" "$PLAIN" || fail "$1: no line matching /$2/ in $PLAIN"
}
need_ere() { # <what> <ere>
    grep -qE "$2" "$PLAIN" || fail "$1: no line matching /$2/ in $PLAIN"
}

# Each arm reports its own name and outcome, so a MISSING arm and a FAILING arm are different
# failures rather than one silence.
arm_ok() { # <arm>
    grep -q "^  $TOK arm=$1 ok=1\$" "$PLAIN" \
        || fail "arm $1 did not report ok: $(grep "arm=$1 " "$PLAIN" | head -1)"
}

# isa-debug-exit reports (status << 1) | 1, so a clean arch_shutdown(0) is 1.
kos_require_clean_exit() {
    [ "$QEMU_RC" = 1 ] || fail "arch_shutdown did not exit with status 0 (qemu rc $QEMU_RC)"
}
