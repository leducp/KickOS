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
    # An explicit FLASH_PORT is STRICT: use it, or fail -- NEVER fall through to
    # another board's port. (Falling through once sent esptool at the wrong device
    # on a multi-board bench.) Only auto-scan when FLASH_PORT is unset.
    if [ -n "${FLASH_PORT:-}" ]; then
        [ -e "$FLASH_PORT" ] && { echo "$FLASH_PORT"; return 0; }
        return 1
    fi
    local p
    for p in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0 /dev/ttyUSB1; do
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

    # Build dir defaults to build/<board>; override with FLASH_BUILD to flash a
    # variant preset's output (e.g. FLASH_BUILD=build/rx72m-st for the selftest build,
    # which carries the diagnostic apps that the plain board build does not).
    local bd="${FLASH_BUILD:-$FL_ROOT/build/$FL_BOARD}"
    # kickos_emit_image outputs: ELF, .hex, .bin, and .app.bin for Espressif.
    # In-tree apps emit under user/apps/<app>/. FLASH_IMAGE points the flasher
    # directly at an image instead -- an out-of-tree find_package(KickOS) consumer
    # emits outside that layout, so name its image explicitly rather than guessing a
    # path. A trailing .hex/.bin/.elf is stripped to a base; the siblings derive from
    # it (jlink loads .hex, st-flash the .bin, esptool the .app.bin).
    local base
    if [ -n "$FLASH_IMAGE" ]; then
        base=${FLASH_IMAGE%.hex}; base=${base%.bin}; base=${base%.elf}
    else
        base="$bd/user/apps/$FL_APP/$FL_APP"
    fi
    FL_ELF="$base"; FL_BIN="$base.bin"; FL_HEX="$base.hex"; FL_APPBIN="$base.app.bin"
    [ -e "$FL_ELF" ] || [ -e "$FL_HEX" ] || die "not built: ${FLASH_IMAGE:-$base}
       in-tree:     cmake --preset $FL_BOARD && cmake --build $bd --target $FL_APP
       out-of-tree: FLASH_IMAGE=<path/to/$FL_APP.hex> flash-jlink.sh $FL_BOARD $FL_APP
       (selftest/diagnostic build: FLASH_BUILD=$FL_ROOT/build/$FL_BOARD-st)"
}
