#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Silicon pass. ONE board per invocation, on the tree as committed. TAG names the
# milestone and keys both the build dir and the log, so two milestones never share either.
#
#   tools/bench/bench.sh xmc4800-relax [jlink-sn]
#   tools/bench/bench.sh frdmk64f      [jlink-sn]
#   tools/bench/bench.sh f302nucleo
#   tools/bench/bench.sh rx72m
#   tools/bench/bench.sh esp32c6-wroom
#   tools/bench/bench.sh esp32-wroom
#
# THE SERIAL ARGUMENT IS OPTIONAL and exists only to override: with none, a board whose
# flasher needs one has it read off the bus here. Serials are never quoted from a note,
# there being more than one physical XMC and K64F in rotation, and no caller should ever
# pair a board with a serial by hand. tools/bench/bench-present.sh shows the same live
# answer without flashing anything.
#
# REMOTE MODE. Set BENCH_HOST and the build happens here, the flashing and capturing
# happen there:
#
#   BENCH_HOST=<bench-host> BENCH_PORT=<port> TAG=<tag> tools/bench/bench.sh xmc4800-relax <sn>
#
# The bench host needs no toolchain: it receives an image, plus tools/ and boards/, which
# are the flash recipes themselves rather than a copy of them. What it does NOT receive is
# a decision: bench-capture.sh runs there and every refusal in it fires there, so a
# remote failure cannot read as a local success. Forgetting BENCH_HOST while the boards
# are remote is loud: the by-id symlink is absent here and the capture refuses.
#
# The order is FLASH -> wait for the flasher's own reset-and-run to drain -> arm exactly
# ONE reader by its by-id symlink -> reset separately, and it lives in bench-capture.sh.
# Arming before the flash yields an empty log on a J-Link and a truncated banner on an
# ST-Link; two readers on one port yield a full-looking log with interleaved half-lines.
# Both failures read as a pass at a glance, which is why that script refuses rather than
# improvises.
#
# The rig values, meaning the session directory, the default tree and the bench host's
# paths, come from .session/rig.conf. See tools/bench/rig.conf.example. How to REACH that
# host is ssh_config's: a port is passed only where the rig names one, so an ssh alias
# carrying its own Port and User works as itself.
#
# The -st variant states the enforcing posture itself; there is no posture flag. VARIANT set
# empty builds the board's own preset.
#
# PACKAGE_PROJECT names a consumer project, relative to the tree, built against the board build's
# installed package as the golden and composition gates build one (tests/lib/package_image.sh);
# APP is its target and PACKAGE_ARGS reaches its configure verbatim.
#
# JUDGE names a gate script, relative to the tree, that reads a capture through KOS_CAPTURE. It
# runs here over the log just taken, as `<script> <board build> <tree> cmake <args>...`, the args
# being JUDGE_ARGS split at each `;`, then `@<fitting>` for each of the board's RIG_WIRED_<BOARD>
# fittings, and its verdict is this run's exit status:
#
#   PACKAGE_PROJECT=examples/composition APP=sensor_system VARIANT= \
#     JUDGE=tests/integration/check_golden_system.sh tools/bench/bench.sh xmc4800-relax
#
# An image is judged by the gate script its row of the build's image listing names, with the
# row's args, unless JUDGE names another: an image's kickos_app_judge, or the TAP validator for a
# selftest image, which the capture runs itself:
#
#   VARIANT=st APP=xmcspi tools/bench/bench.sh xmc4800-relax
#
# An image that no row and no JUDGE judges is refused, and so is one its row marks emulator-judged,
# human-judged or inapplicable; JUDGE=none takes the capture unjudged.
set -u
# AMP_PARTITION=1, with APP=ampping_n0 and VARIANT=amp2-n0, builds the board's whole AMP partition,
# every node's image merged into one, flashes that, and judges the capture with the judge
# ampping_n0's row names.
#
# WHICH IMAGES THIS BOARD'S BUILD SHIPS IS A PROPERTY OF ITS CONFIGURE, and LIST_IMAGES=1 is how a
# caller asks: configure, print one `<image>|<stdout>|<judge>|<args>` row per image, flash nothing.
# The stdout is `kernel`, the packaged console driver the image's composition names, or `-`. The
# judge is a gate script relative to the tree; `emulator` or `emulator-owed` for an image whose
# verdict an emulator gate holds, by whether this build registers that gate; `human` for one whose
# verdict only a person reads; `inapplicable` for one whose claim this posture voids; or `-`. The
# args are the judge's, `;`-separated. Everything this script narrates goes to stderr in that mode,
# so the caller's $(...) holds the rows and nothing else.
#
# BUILD_ONLY=1 configures and builds the image as a capture would, then stops before the flash,
# so a caller can build every image before the first board is flashed.
if [ "${LIST_IMAGES:-0}" = "1" ]; then
  exec 3>&1 1>&2
fi
# readlink, because .session/ carries a SYMLINK to this script for muscle memory: without
# it $0's directory is .session/ and the sibling scripts below are not there.
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# TREE, not a hardcoded main repo. A caller working in a git worktree would otherwise build
# and flash the MAIN tree and read the result as its own: a green that means nothing, which
# is worse than a failure. The rig assets stay where rig.conf points either way; only the
# tree under build/ and the sources move.
. "$HERE/rig.sh"
. "$HERE/bench-host.sh"
. "$HERE/board-rows.sh"
. "$HERE/amp_peers.sh"
. "$HERE/banner.sh"
rig_load "$(cd "$HERE/../.." && pwd)"
rig_need RIG_SESSION "the session directory holding env.sh and receiving logs/"
rig_need RIG_TREE "the tree to build when the caller sets no TREE"
SESSION="$RIG_SESSION"
[ -f "$SESSION/env.sh" ] || { echo "REFUSING: no $SESSION/env.sh: the cross toolchains are not on the default PATH and the build would fail as a missing compiler" >&2; exit 2; }
source "$SESSION/env.sh" >/dev/null 2>&1
TREE="${TREE:-$RIG_TREE}"
[ -e "$TREE/CMakePresets.json" ] || { echo "REFUSING: $TREE is not a KickOS tree" >&2; exit 2; }
cd "$TREE" || exit 2
echo "=== tree $TREE"

TAG="${TAG:-m475}"
BOARD="${1:?usage: bench.sh <board> [jlink-sn]}"
if [ -n "${JUDGE:-}" ] && [ "$JUDGE" != none ] && [ ! -f "$JUDGE" ]; then
  echo "REFUSING: JUDGE $JUDGE names no gate script in $TREE" >&2
  exit 2
fi
SN="${2:-}"
APP="${APP:-selftest}"

# Where the boards are, decided once, so the serial below is read off the right bus.
bench_host_select "${BENCH_HOST:-}"

# THE PROBE SERIAL IS READ OFF THE BUS when the caller passes none. It is never paired with
# a board by hand: more than one XMC and more than one K64F are in rotation, so a serial is
# not a desk fact. A serial given as an argument still wins.
#
# Not when listing: asking which images a board ships must not fail for a board that is
# unplugged, or the caller is left choosing between a stale list of its own and nothing.
if [ -z "$SN" ] && [ "${LIST_IMAGES:-0}" != "1" ]; then
  PROBE_ID=$(board_probe_rows "$BOARD" 2>/dev/null | awk -F '|' '$2 == "sn" { print $1; exit }')
  if [ -n "$PROBE_ID" ]; then
    bench_bus_read || {
      echo "REFUSING: could not read the bus on $BENCH_WHERE, so $BOARD's probe serial" >&2
      echo "  cannot be resolved. tools/bench/bench-present.sh reach says whether that" >&2
      echo "  machine answers at all." >&2
      exit 2
    }
    SN=$(usb_serial_of "${PROBE_ID%%:*}" "${PROBE_ID##*:}") || {
      echo "REFUSING: no $PROBE_ID on $BENCH_WHERE, so $BOARD has no probe to flash." >&2
      echo "  tools/bench/bench-present.sh $BOARD says what that bus carries." >&2
      exit 2
    }
    echo "=== $BOARD  SN $SN, read off $BENCH_WHERE"
  fi
fi
# The preset variant. `st` states the enforcing posture and the selftest syscalls; `bench`
# states the same posture plus the microbench. The variant is part of the BUILD DIR so a
# bench capture and a selftest capture at one TAG cannot share a tree.
VARIANT="${VARIANT-$BENCH_DEFAULT_VARIANT}"
PRESET="$BOARD"
BUILD="build/$TAG-$BOARD"
if [ -n "$VARIANT" ]; then
  PRESET="$BOARD-$VARIANT"
  BUILD="$BUILD-$VARIANT"
fi
LOG="$SESSION/logs/$TAG-$BOARD-$APP.log"
mkdir -p "$SESSION/logs"

# The RP boards cannot be reflashed once KickOS runs: J-Link finds the SW-DP and then fails to
# power up the DAP, and BOOTSEL is the only way back, so every run would otherwise cost a physical
# power-cycle. KICKOS_SHUTDOWN_TO_BOOTLOADER exists for exactly this: kickos_terminate tries
# arch_reboot before halting, so the board lands back in BOOTSEL by itself. It touches only the
# path AFTER the last TAP line, and it requires KICKOS_ENABLE_SELFTEST, which the -st variant has.
EXTRA=()
case $BOARD in
  picopi|pizero2350) EXTRA+=(-DKICKOS_SHUTDOWN_TO_BOOTLOADER=ON) ;;
  # teensy41: HalfKay is otherwise reachable only by a physical button press. arch_reboot's
  # bkpt #251 is caught by the MKL02 companion, which presents HalfKay itself.
  teensy41) EXTRA+=(-DKICKOS_SHUTDOWN_TO_BOOTLOADER=ON) ;;
  *) ;;
esac
# EXTRA_CMAKE reaches the configure verbatim.
if [ -n "${EXTRA_CMAKE:-}" ]; then
  # Deliberately unquoted: the caller passes one or more -D words.
  # shellcheck disable=SC2206
  EXTRA+=($EXTRA_CMAKE)
fi
# Every -D above lands in the build dir's CACHE and survives there, so reusing the dir at the
# same TAG-BOARD-VARIANT would measure an EXTRA_CMAKE this invocation never
# passed. The stamp records the set that configured the dir, and a dir whose set differs, or
# that carries no stamp, is discarded instead of reused: a capture is a witness for the flags
# of ITS run. Written only after the configure succeeds, so a half-configured dir is discarded
# on the next run too.
EXTRA_STAMP="$BUILD/.kickos-extra-d"
EXTRA_WANT=$(printf '%s\n' "${EXTRA[@]+"${EXTRA[@]}"}")
if [ -d "$BUILD" ]; then
  if [ ! -f "$EXTRA_STAMP" ] || [ "$(cat "$EXTRA_STAMP")" != "$EXTRA_WANT" ]; then
    echo "=== discarding $BUILD: its cache was not configured with this run's extra -D set"
    rm -rf "$BUILD"
  fi
fi

# A TAG can COLLIDE with a build dir an earlier session left behind, and then the generator loads
# that dir's stale generated/.config and refuses a symbol this tree does not declare. It reads as a
# broken preset. A removed knob's name is deliberately not spelled here, because this file is
# tracked and doc_names would take a dead name from it into its valid set and stop reporting it
# in the docs.
if ! CFGOUT=$(cmake --preset "$PRESET" -B "$BUILD" "${EXTRA[@]+"${EXTRA[@]}"}" 2>&1); then
  printf '%s\n' "$CFGOUT" | tail -20 >&2
  if printf '%s\n' "$CFGOUT" | grep -q 'no such symbol'; then
    echo "REFUSING: $BUILD holds a stale generated/.config from an earlier session." >&2
    echo "  This is a TAG collision, not a broken tree: rm -rf $BUILD and retry." >&2
  fi
  exit 1
fi
printf '%s\n' "$EXTRA_WANT" > "$EXTRA_STAMP"

# WHAT THIS BOARD'S CONFIGURE SAYS ABOUT ITS SELFTEST IMAGES: one row per image, carrying the
# image name, the arm count that image plans and the skip, partial and fault permission sets.
# Written by tests/integration/gates/selftest.cmake, which is the file that hands the same three
# sets to the CTest entries, so nothing here is a second statement of them.
MANIFEST="$BUILD/kickos-selftest-manifest.txt"
# EVERY IMAGE THIS CONFIGURE EMITS, one `<image>|<stdout>|<judge>|<args>` row each. Written by the
# root CMakeLists.txt.
IMAGES="$BUILD/kickos-images.txt"
[ -s "$IMAGES" ] || { echo "REFUSING: $PRESET configured but listed no image at $IMAGES" >&2; exit 1; }
TAP_JUDGE=tests/integration/check_tap_stream.sh
MANIFEST_OWED=0
case $APP in
  selftest*) MANIFEST_OWED=1 ;;
  *) ;;
esac
if [ "${LIST_IMAGES:-0}" = "1" ] && awk -F '|' -v j="$TAP_JUDGE" '$3 == j { f = 1 } END { exit !f }' "$IMAGES"; then
  MANIFEST_OWED=1
fi
if [ "$MANIFEST_OWED" -eq 1 ] && [ ! -s "$MANIFEST" ]; then
  echo "REFUSING: $PRESET configured but published no selftest manifest at $MANIFEST." >&2
  echo "  tests/integration/gates/selftest.cmake writes it, and it is skipped whole when" >&2
  echo "  KICKOS_BUILD_TESTS is off. Without it the capture has no arm count and no permission" >&2
  echo "  sets, and a TAP stream nothing checks is a count of the lines that survived." >&2
  exit 1
fi
if [ "${LIST_IMAGES:-0}" = "1" ]; then
  cat "$IMAGES" >&3
  exit 0
fi

APP_ROW=$(awk -F '|' -v app="$APP" '$1 == app { print; exit }' "$IMAGES")
APP_STDOUT=""
APP_JUDGE=""
APP_JUDGE_ARGS=""
if [ -n "$APP_ROW" ]; then
  IFS='|' read -r _ APP_STDOUT APP_JUDGE APP_JUDGE_ARGS <<AROW
$APP_ROW
AROW
fi
APP_EMULATED=""
APP_HUMAN=""
APP_VOID=""
case $APP_JUDGE in
  -) APP_JUDGE="" ;;
  emulator|emulator-owed)
    APP_EMULATED=$APP_JUDGE
    APP_JUDGE=""
    ;;
  human)
    APP_HUMAN=$APP_JUDGE_ARGS
    APP_JUDGE=""
    ;;
  inapplicable)
    APP_VOID=$APP_JUDGE_ARGS
    APP_JUDGE=""
    ;;
  *) ;;
esac
if [ -z "${JUDGE:-}" ]; then
  JUDGE="$APP_JUDGE"
  JUDGE_ARGS="$APP_JUDGE_ARGS"
fi
if [ -z "$JUDGE" ]; then
  if [ -n "$APP_EMULATED" ]; then
    echo "REFUSING: $APP is emulator-judged ($APP_EMULATED): an exit status, a panic, a reboot or a" >&2
    echo "  deliberate fault holds its verdict, and no capture carries it (JUDGE=none to waive)" >&2
  elif [ -n "$APP_HUMAN" ]; then
    echo "REFUSING: $APP is human-judged ($APP_HUMAN): a person reads its verdict and no capture" >&2
    echo "  carries it (JUDGE=none to waive)" >&2
  elif [ -n "$APP_VOID" ]; then
    echo "REFUSING: $APP is inapplicable on $PRESET ($APP_VOID): nothing can witness its claim" >&2
    echo "  here (JUDGE=none to waive)" >&2
  else
    echo "REFUSING: no judge for $APP (JUDGE=none to waive)" >&2
  fi
  exit 2
fi

# THE ROW FOR THE IMAGE BEING CAPTURED, passed to the capture so it can run
# tests/integration/check_tap_stream.sh over what the board printed. A selftest image whose name
# is in no row is refused: judging it against another image's arm count is worse than not
# judging it.
EXPECT_ARMS=""
EXPECT_SKIPS=""
EXPECT_PARTIALS=""
EXPECT_FAULTS=""
case $APP in
  selftest*)
    # awk and not grep: `^selftest|` is a literal in a basic regex and an ALTERNATION in an
    # extended one, where it matches every row and `selftest_p3` would silently take the arm
    # count of image 1. The field comparison is an equality and cannot be read two ways.
    MROW=$(awk -F '|' -v app="$APP" '$1 == app { print; exit }' "$MANIFEST")
    [ -n "$MROW" ] || { echo "REFUSING: $MANIFEST carries no row for $APP, so this board's" >&2
      echo "  configure did not emit that image and nothing states how many arms it plans." >&2
      exit 1; }
    IFS='|' read -r _ EXPECT_ARMS EXPECT_SKIPS EXPECT_PARTIALS EXPECT_FAULTS <<MROW
$MROW
MROW
    [ -n "$EXPECT_ARMS" ] || { echo "REFUSING: $MANIFEST's row for $APP states no arm count" >&2; exit 1; }
    ;;
  *) ;;
esac
export EXPECT_ARMS EXPECT_SKIPS EXPECT_PARTIALS EXPECT_FAULTS

BUILD_TARGET="$APP"
if [ "${AMP_PARTITION:-0}" = "1" ]; then
  if [ "$APP" != "ampping_n0" ] || [ "$VARIANT" != "amp2-n0" ]; then
    echo "REFUSING: AMP_PARTITION=1 requires APP=ampping_n0, VARIANT=amp2-n0" >&2
    exit 1
  fi
  BUILD_TARGET=amp_partition
fi
IMG=""
if [ -n "${PACKAGE_PROJECT:-}" ]; then
  [ -f "$PACKAGE_PROJECT/CMakeLists.txt" ] || { echo "REFUSING: PACKAGE_PROJECT $PACKAGE_PROJECT names no CMake project in $TREE" >&2; exit 1; }
  # The install takes every target the package ships, so the whole tree is built first.
  cmake --build "$BUILD" -j8 > /dev/null || exit 1
  PKG_OUT="$PWD/$BUILD/package-$APP"
  rm -rf "$PKG_OUT"
  # Deliberately unquoted: the caller passes one or more -D words.
  # shellcheck disable=SC2086
  IMG=$(sh tests/lib/package_image.sh "$PWD/$BUILD" cmake "$PWD/$PACKAGE_PROJECT" "$APP" \
          "$PKG_OUT" ${PACKAGE_ARGS:-}) || exit 1
else
  cmake --build "$BUILD" -j8 --target "$BUILD_TARGET" > /dev/null || exit 1
fi
if [ "${BUILD_ONLY:-0}" = "1" ]; then
  exit 0
fi

# THE LABEL THAT WENT INTO THIS IMAGE, read out of the stamp the build just wrote rather than
# asked of git here. The two answer differently the moment the tree is touched between the
# build and the capture, and the capture chain compares the board's banner against this: asking
# git would call a good image stale, and on the bench host there is no tree of ours to ask.
STAMP="$BUILD/kickos_build_stamp.cc"
EXPECT_COMMIT=$(sed -n 's|.*kickos_build_commit\[\] = "\(.*\)";.*|\1|p' "$STAMP" | tail -1)
[ -n "$EXPECT_COMMIT" ] || { echo "REFUSING: no commit label in $STAMP, so the capture would
  have nothing to check the board's banner against" >&2; exit 1; }
export EXPECT_COMMIT
# THE ARCH THIS IMAGE WAS CONFIGURED FOR, which decides the rows its report owes. Read off the
# build rather than the board name, so a preset that overrides the board's arch is judged as
# what it built; the capture also holds it against the board's descriptor.
EXPECT_ARCH=$(sed -n 's|^KICKOS_ARCH:[A-Z]*=||p' "$BUILD/CMakeCache.txt" 2>/dev/null | tail -1)
[ -n "$EXPECT_ARCH" ] || { echo "REFUSING: no KICKOS_ARCH in $BUILD/CMakeCache.txt, so the
  capture would have nothing to say which rows the report owes" >&2; exit 1; }
export EXPECT_ARCH

# An own-image AMP node's image loaded alone runs alone, as its manifest row judges it: node 0
# starts whatever an earlier partition left in the other nodes' flash windows, so they go first.
PEER_ERASE=""
if [ "${AMP_PARTITION:-0}" != "1" ]; then
  PEER_ERASE=$(amp_peer_text "$BUILD/generated/kickos_config.cmake") \
    || { echo "REFUSING: $PEER_ERASE" >&2; exit 1; }
  PEER_ERASE=$(printf '%s\n' "$PEER_ERASE" | paste -sd ' ' -)
fi
export PEER_ERASE

# The emitted image base, without extension. Board-specific apps are searched FIRST, the
# same order tools/flash-common.sh uses, so a name collision resolves the same way here.
#
# The last two candidates are the split suite: `selftest_p2` and up are declared by
# user/apps/common/selftest/CMakeLists.txt, so CMake emits them into the SELFTEST directory and
# there is no selftest_p2/ directory to find. tools/flash-common.sh's _app_base has the same
# blind spot.
BASE=${APP%_p[0-9]}
if [ "${AMP_PARTITION:-0}" = "1" ]; then
  IMG="$PWD/$BUILD/kickos-partition"
fi
for d in "$PWD/$BUILD/user/apps/$BOARD/$APP" "$PWD/$BUILD/user/apps/common/$APP" \
         "$PWD/$BUILD/user/apps/$BOARD/$BASE" "$PWD/$BUILD/user/apps/common/$BASE"; do
  [ -n "$IMG" ] && break
  if [ -e "$d/$APP" ] || [ -e "$d/$APP.hex" ]; then
    IMG="$d/$APP"
    break
  fi
done
# Same blind spot, other spelling: faultsurvive_ovf / faultsurvive_off are declared by
# user/apps/common/faultsurvive/CMakeLists.txt with no _p<N> suffix to strip, so the BASE
# rule above cannot reach them either. Search for the emitted file rather than adding a
# third naming rule; still refuses when nothing matches.
if [ -z "$IMG" ]; then
  CAND=$(find "$PWD/$BUILD/user/apps" -mindepth 3 -maxdepth 3 -type f -name "$APP" 2>/dev/null | head -1)
  [ -n "$CAND" ] && IMG="$CAND"
fi
[ -n "$IMG" ] || { echo "REFUSING: $APP built but no image under $BUILD/user/apps/{$BOARD,common}/$APP" >&2; exit 1; }

# An image whose stdout driver is a USB device console blinds the pin UART, so the route is the
# device's own ACM. Derived from the image, not asked for: its composition is what moves the
# console.
CONSOLE_USB_CDC=0
if [ -n "$APP_STDOUT" ] && [ "$APP_STDOUT" != kernel ] && [ "$APP_STDOUT" != - ] \
     && usb_console_image "$IMG"; then
  CONSOLE_USB_CDC=1
fi
# CONSOLE_PIN=1 forces the PIN console back. A device-controller backend that dies before it
# publishes leaves the kernel console on the pin UART, so that cable is the only channel
# carrying the failure; an ACM that never enumerates would report silence for a live board.
if [ "${CONSOLE_PIN:-0}" = "1" ]; then
  CONSOLE_USB_CDC=0
fi

# A USB device console never carries the kernel banner, so the route holds the identity rows
# the image prints once the host configures it to the flashed image: version and diag column from
# the build's cache and board config, commit from its stamp, build and app stamps from the image.
route_identity() {
  [ "$CONSOLE_USB_CDC" = "1" ] || return 0
  local version terse built app why
  version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt" | tail -1)
  terse=$(awk '$1 == "#define" && $2 == "KICKOS_DIAG_TERSE" { print $3; exit }' \
            "$BUILD/generated/include/kickos/board_config.h")
  [ -n "$version" ] && [ -n "$terse" ] || { echo "REFUSING: $BUILD states no version or no \
KICKOS_DIAG_TERSE, so the identity rows cannot be rendered" >&2; return 1; }
  built=$(image_string "$IMG" kickos_build_time)
  [ -n "$built" ] || { echo "REFUSING: $IMG holds no kickos_build_time, so the identity rows \
cannot be held to it" >&2; return 1; }
  app=$(image_string "$IMG" kickos_app_stamp)
  if ! why=$(identity_verdict "$LOG" include/kickos/diag.h "$terse" "$version" "$BOARD" \
               "$EXPECT_COMMIT" "$built" "$app"); then
    echo "REFUSING: $LOG is not a capture of the flashed image: $why" >&2
    return 1
  fi
  echo "=== identity rows name $BOARD at $EXPECT_COMMIT, built $built${app:+, app $app}"
}

# The capture's verdict, from the gate script JUDGE names; none without one.
judge() {
  if [ "$JUDGE" = none ] || [ "$JUDGE" = "$TAP_JUDGE" ]; then
    return 0
  fi
  local wired
  wired=$(rig_wired "$BOARD") || return 2
  echo "=== judging $LOG with $JUDGE${JUDGE_ARGS:+ ($JUDGE_ARGS)}, fittings: ${wired:-none declared}"
  rig_judge "$BOARD" "$LOG" "$PWD/$BUILD" "$JUDGE" "${JUDGE_ARGS:-}"
}

# --- boards here ---------------------------------------------------------------
if [ -z "${BENCH_HOST:-}" ]; then
  # Selecting the mode is BENCH_HOST's job and not the rig config's: a key must not move a
  # flashing run from one machine to another.
  if [ -n "${RIG_BENCH_HOST:-}" ]; then
    echo "NOTE: BENCH_HOST is unset, so this run flashes on THIS BOX, while $RIG_CONF"
    echo "  names $RIG_BENCH_HOST as the bench. tools/bench/bench-present.sh says where"
    echo "  the boards are."
  fi
  # KICKOS_RIG is passed explicitly rather than left to the capture script's own
  # discovery: TREE may be a worktree, which has no .session/ to discover.
  ROOT="$PWD" KICKOS_RIG="$RIG_CONF" PYBIN="${RIG_PYBIN:-${PY:-}}" \
    CONSOLE_USB_CDC="$CONSOLE_USB_CDC" PEER_ERASE="$PEER_ERASE" \
    "$HERE/bench-capture.sh" "$BOARD" "$APP" "$IMG" "$LOG" "$SN" || exit $?
  route_identity || exit $?
  judge || exit $?
  exit 0
fi

# --- boards on the bench host --------------------------------------------------
rig_need RIG_REMOTE_ROOT "the directory on the bench host holding the shipped tree and the run outputs"
RROOT="${RIG_REMOTE_ROOT}/tree"
RRUN="${RIG_REMOTE_ROOT}/run/$TAG-$BOARD-$APP"
RLOG="$RRUN/$TAG-$BOARD-$APP.log"
echo "=== $BENCH_WHERE"

SSH=("${BENCH_SSH[@]}")
RSH="$BENCH_RSH"

# rsync will not create an intermediate destination directory, and .session/ over there
# holds nothing but the shipped rig config.
#
# ssh joins the command and its arguments into ONE string and hands that whole string to
# the remote login shell (zsh) to parse, so an unquoted path carrying a space or a glob
# character would be re-split, or glob-expanded and the whole command aborted, there
# instead of naming the one directory it was given. printf %q quotes each path for that
# remote parse; the operation itself stays in the same bash-on-stdin shape as below.
"${SSH[@]}" bash -s -- "$(printf '%q' "$RROOT/.session")" "$(printf '%q' "$RRUN")" \
    <<'REMOTE' || { echo "REFUSING: cannot create $RRUN on $BENCH_HOST" >&2; exit 1; }
mkdir -p "$1" "$2"
REMOTE

# tools/ and boards/ are the flash recipes, not a second copy of them: the backends read
# boards/<board>/board.cmake for the chip and take the image through FLASH_IMAGE, so the
# bench host runs the same recipe this tree ships. rsync means only the delta travels.
# The capture chain rides along inside tools/bench/, so it is the same tree's copy too.
#
# tests/ travels for the same reason: the capture runs tests/integration/check_tap_stream.sh
# over the stream, and that script sources tests/lib/gate.sh, which itself reads
# tests/lib/panic.ere at source time. Shipped WHOLE rather than file by file, so a helper one
# of them picks up tomorrow cannot be the one that is missing over there.
#
# -s (--secluded-args/--protect-args) sends the remote-side path over rsync's own
# protocol instead of a shell command line, so the destination is never re-parsed by the
# remote login shell.
#
# The lock, because a fleet pass runs one of these per board at once into the same directory, and
# one transfer's --delete removes the temporary files of another.
(
  flock 9
  rsync -a -s --delete -e "$RSH" tools boards tests "$BENCH_HOST:$RROOT/"
) 9> "$SESSION/.bench-ship.lock" || { echo "REFUSING: could not ship tools/, boards/ and tests/" >&2; exit 1; }
# The rig config is the one thing tools/ cannot carry: it is gitignored, and the console
# cable it names is a property of the CABLE, so it is valid wherever that cable is plugged.
rsync -a -s -e "$RSH" "$RIG_CONF" "$BENCH_HOST:$RROOT/.session/rig.conf" || { echo "REFUSING: could not ship the rig config" >&2; exit 1; }

# Every sibling the flashers may want: JLinkExe loads the .hex, st-flash the .bin, esptool
# the .app.bin. Ship whichever exist rather than deciding per board twice.
IMGS=()
for f in "$IMG" "$IMG.hex" "$IMG.bin" "$IMG.app.bin"; do
  [ -e "$f" ] && IMGS+=("$f")
done
[ "${#IMGS[@]}" -gt 0 ] || { echo "REFUSING: no image files to ship for $APP" >&2; exit 1; }
rsync -a -s -e "$RSH" "${IMGS[@]}" "$BENCH_HOST:$RRUN/" || { echo "REFUSING: could not ship the image" >&2; exit 1; }

# The remote login shell is zsh, which does not word-split and ABORTS on an unmatched
# glob, so a command line assembled here would be re-parsed there under different rules.
# Feeding bash a heredoc on stdin and passing the arguments after `--` keeps the parsing
# rules the same on both sides.
#
# EVERY argument is non-empty, and "-" carries "none". ssh joins argv into one string and
# the remote shell re-splits it, so an EMPTY argument does not arrive at all and every
# later positional shifts up one. SN is empty on four of the six boards, so passing it raw
# would hand the capture script a shifted argument list on exactly those boards.
#
# AND `bash -s --` DOES NOT REACH THE SPLIT, ONLY THE PARSE: the bench host's zsh parses the
# joined command string before any `bash` in it runs. printf %q is what survives that parse,
# as for the mkdir above.
ROUT=$(mktemp)
RARGS=()
for _ra in "$BOARD" "$APP" "$RRUN/$(basename "$IMG")" "$RLOG" "${SN:--}" "${CAP_SECS:--}" \
           "$RIG_REMOTE_ROOT" "${RIG_REMOTE_PYBIN:--}" "$CONSOLE_USB_CDC" "$EXPECT_COMMIT" \
           "${EXPECT_ARMS:--}" "${EXPECT_SKIPS:--}" "${EXPECT_PARTIALS:--}" "${EXPECT_FAULTS:--}" \
           "$EXPECT_ARCH" "${PEER_ERASE:--}"; do
  RARGS+=("$(printf '%q' "$_ra")")
done
"${SSH[@]}" bash -s -- "${RARGS[@]}" \
    <<'REMOTE' 2>&1 | tee "$ROUT"
set -u
# uv's esptool and the rfp-cli wrapper live in ~/.local/bin, which a non-interactive ssh
# does not put on PATH. The Espressif capture needs a python carrying pyserial, and the
# only one on this host is named by the rig config, absolute, because a $HOME-relative value
# would arrive here as a literal, since this heredoc is quoted and $8 is not re-expanded.
export PATH="$PATH:$HOME/.local/bin"
export ROOT="$HOME/$7/tree"
export KICKOS_RIG="$ROOT/.session/rig.conf"
SN=$5
CAP=$6
PYBIN=$8
[ "$SN" = "-" ] && SN=""
[ "$CAP" != "-" ] && export CAP_SECS="$CAP"
[ "$PYBIN" != "-" ] && export PYBIN
export CONSOLE_USB_CDC="$9"
# The tree shipped here is a copy with no .git, so the capture cannot derive this and the
# label the image was built with travels with the image.
export EXPECT_COMMIT="${10}"
# What check_tap_stream.sh is owed. `-` carries "none" here as it does for every other argument,
# and an empty permission set means the same thing to that script as an unset one: nothing may
# skip. EXPECT_ARMS is the one that matters, and the capture refuses a selftest image without it.
ARMS=${11}
SKIPS=${12}
PARTIALS=${13}
FAULTS=${14}
[ "$ARMS" != "-" ] && export EXPECT_ARMS="$ARMS"
[ "$SKIPS" != "-" ] && export EXPECT_SKIPS="$SKIPS"
[ "$PARTIALS" != "-" ] && export EXPECT_PARTIALS="$PARTIALS"
[ "$FAULTS" != "-" ] && export EXPECT_FAULTS="$FAULTS"
export EXPECT_ARCH="${15}"
PEER_ERASE=${16}
[ "$PEER_ERASE" != "-" ] && export PEER_ERASE
exec bash "$ROOT/tools/bench/bench-capture.sh" "$1" "$2" "$HOME/$3" "$HOME/$4" "$SN"
REMOTE
RC=${PIPESTATUS[0]}
# THE LOG IS THE ARTIFACT, THE VERDICT IS SEPARATE, SO THE FETCH RUNS EITHER WAY. Nothing about
# a refusal makes the bytes less real, and the refusals most worth reading are the ones with a
# log.
RBYTES=$(sed -n 's/^bytes: *//p' "$ROUT" | head -1)
rm -f "$ROUT"
FETCHED=0
rm -f "$LOG.times"
if rsync -a -s -e "$RSH" "$BENCH_HOST:$RLOG" "$LOG" 2>/dev/null; then
  FETCHED=1
  LBYTES=$(wc -c < "$LOG")
fi
rsync -a -s -e "$RSH" "$BENCH_HOST:$RLOG.times" "$LOG.times" 2>/dev/null || true

if [ "$RC" -ne 0 ]; then
  # The remote refusal stays the headline and keeps its exit code; whether the log survived
  # only changes what there is to read. A refusal that fired before the capture wrote
  # anything legitimately has no log, so a failed fetch here is not a second defect.
  if [ "$FETCHED" -eq 1 ]; then
    echo "log: $LOG  ($LBYTES bytes, fetched despite the refusal; read it before re-running)" >&2
  else
    echo "no log fetched: the refusal fired before $RLOG was written" >&2
  fi
  echo "REFUSING: the remote capture failed rc=$RC (its own refusal is above)" >&2
  exit "$RC"
fi

# On the clean path the transfer is PROVEN, so a truncated fetch cannot read as a pass
# either. The remote byte count is the authority: it was taken where the log was written.
[ "$FETCHED" -eq 1 ] || { echo "REFUSING: could not fetch $RLOG" >&2; exit 1; }
[ -n "$RBYTES" ] || { echo "REFUSING: the remote capture reported no byte count" >&2; exit 1; }
[ "$LBYTES" = "$RBYTES" ] || { echo "REFUSING: fetched $LBYTES bytes, the bench wrote $RBYTES" >&2; exit 1; }
echo "log: $LOG  ($LBYTES bytes, fetched from $BENCH_HOST)"
route_identity || exit $?
judge || exit $?
