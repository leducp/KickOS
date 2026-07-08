# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Shared helpers for the KickOS flash scripts. SOURCED (not run) by flash.sh and
# every flash-<tool>.sh backend. The sourcing script must set FL_ROOT (repo root)
# before sourcing.
#
# flash_resolve <board> [app] populates, from boards/<board>/board.cmake + the
# build tree, the globals the backends use:
#   FL_BOARD FL_APP FL_CHIP FL_ARCH   FL_ELF FL_BIN FL_HEX FL_APPBIN
# and refuses the QEMU/sim boards (nothing to flash there).

have() { command -v "$1" >/dev/null 2>&1; }
say()  { printf 'flash: %s\n' "$*" >&2; }
die()  { say "$*"; exit 1; }
run()  { say "+ $*"; [ -n "${DRY_RUN:-}" ] || "$@"; }

# first present serial device (override with FLASH_PORT); empty + nonzero if none
pick_port() {
    local p
    for p in ${FLASH_PORT:-} /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0 /dev/ttyUSB1; do
        [ -e "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

# _bf <board> <KICKOS_VAR> -> value from board.cmake (empty if absent; never fails)
_bf() {
    grep -oP "$2"'\s+"?\K[A-Za-z0-9_]+' "$FL_ROOT/boards/$1/board.cmake" 2>/dev/null | head -1 || true
}

flash_resolve() {
    FL_BOARD=${1:?usage: <board> [app]}
    FL_APP=${2:-hello}
    [ -f "$FL_ROOT/boards/$FL_BOARD/board.cmake" ] \
        || die "unknown board '$FL_BOARD' (no boards/$FL_BOARD/board.cmake)"
    FL_CHIP=$(_bf "$FL_BOARD" KICKOS_CHIP)
    FL_ARCH=$(_bf "$FL_BOARD" KICKOS_ARCH)

    # QEMU / host targets are not flashed.
    case "$FL_ARCH:$FL_CHIP" in
        sim:*) die "'$FL_BOARD' is the host sim -- run it: ctest --preset sim" ;;
    esac
    case "$FL_CHIP" in
        mps2) die "'$FL_BOARD' is a QEMU target -- run it: ctest --preset qemu" ;;
        virt) die "'$FL_BOARD' is a QEMU target -- run it: ctest --preset qemu-riscv" ;;
    esac

    # kickos_emit_image outputs: ELF, .hex, .bin, and .app.bin for Espressif.
    local ad="$FL_ROOT/build/$FL_BOARD/user/apps/$FL_APP"
    FL_ELF="$ad/$FL_APP"; FL_BIN="$FL_ELF.bin"; FL_HEX="$FL_ELF.hex"; FL_APPBIN="$FL_ELF.app.bin"
    [ -e "$FL_ELF" ] || die "not built: build/$FL_BOARD/user/apps/$FL_APP/$FL_APP
       build it: cmake --preset $FL_BOARD && cmake --build build/$FL_BOARD --target $FL_APP"
}
