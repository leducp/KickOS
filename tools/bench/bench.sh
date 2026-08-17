#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Silicon pass. ONE board per invocation, on the tree as committed. TAG names the
# milestone and keys both the build dir and the log, so two milestones never share either.
#
#   tools/bench/bench.sh xmc4800-relax <jlink-sn>
#   tools/bench/bench.sh frdmk64f      <jlink-sn>
#   tools/bench/bench.sh f302nucleo
#   tools/bench/bench.sh rx72m
#   tools/bench/bench.sh esp32c6-wroom
#   tools/bench/bench.sh esp32-wroom
#
# Serials are never quoted from a note: there is more than one physical XMC and K64F in
# rotation, so resolve the SN live, or let tools/bench/bench-fleet.sh do it.
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
# port and paths, come from .session/rig.conf. See tools/bench/rig.conf.example.
#
# The -st variant states the enforcing posture itself; there is no posture flag.
set -u
# readlink, because .session/ carries a SYMLINK to this script for muscle memory: without
# it $0's directory is .session/ and the sibling scripts below are not there.
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# TREE, not a hardcoded main repo. A caller working in a git worktree would otherwise build
# and flash the MAIN tree and read the result as its own: a green that means nothing, which
# is worse than a failure. The rig assets stay where rig.conf points either way; only the
# tree under build/ and the sources move.
. "$HERE/rig.sh"
rig_load "$(cd "$HERE/../.." && pwd)"
rig_need RIG_SESSION "the session directory holding env.sh and receiving logs/"
rig_need RIG_TREE "the tree to build when the caller sets no TREE"
SESSION="$RIG_SESSION"
[ -f "$SESSION/env.sh" ] || { echo "REFUSING: no $SESSION/env.sh -- the cross toolchains are not on the default PATH and the build would fail as a missing compiler" >&2; exit 2; }
source "$SESSION/env.sh" >/dev/null 2>&1
TREE="${TREE:-$RIG_TREE}"
[ -e "$TREE/CMakePresets.json" ] || { echo "REFUSING: $TREE is not a KickOS tree" >&2; exit 2; }
cd "$TREE" || exit 2
echo "=== tree $TREE"

TAG="${TAG:-m475}"
BOARD="${1:?usage: bench.sh <board> [jlink-sn]}"
SN="${2:-}"
APP="${APP:-selftest}"
# The preset variant. `st` states the enforcing posture and the selftest syscalls; `bench`
# states the same posture plus the microbench. The variant is part of the BUILD DIR so a
# bench capture and a selftest capture at one TAG cannot share a tree.
VARIANT="${VARIANT:-st}"
BUILD="build/$TAG-$BOARD-$VARIANT"
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
# SERVICE_LIST is how a DRIVER gets witnessed at all. Most -st presets default to
# kickos_services_none (rx72m, esp32-wroom, esp32c6-wroom all do), so their selftest image
# contains NO driver service and a green run says nothing about one. frdmk64f and
# xmc4800-relax are the exceptions: their defaults carry the polled console plus SPI. The
# IRQ UART services live only in the *_uartirq providers and are never a default.
if [ -n "${SERVICE_LIST:-}" ]; then
  EXTRA+=(-DKICKOS_SERVICE_LIST="$SERVICE_LIST")
fi
# EXTRA_CMAKE reaches the configure verbatim.
if [ -n "${EXTRA_CMAKE:-}" ]; then
  # Deliberately unquoted: the caller passes one or more -D words.
  # shellcheck disable=SC2206
  EXTRA+=($EXTRA_CMAKE)
fi
# Every -D above lands in the build dir's CACHE and survives there, so reusing the dir at the
# same TAG-BOARD-VARIANT would measure an EXTRA_CMAKE or SERVICE_LIST this invocation never
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

# A _usbcdc list publishes the console onto the board's own USB and blinds the pin UART, so
# the route is the device's own ACM. Derived from the list, not asked for: the provider is
# what moves the console.
CONSOLE_USB_CDC=0
case "${SERVICE_LIST:-}" in
  *_usbcdc) CONSOLE_USB_CDC=1 ;;
esac
# CONSOLE_PIN=1 forces the PIN console back. A device-controller backend that dies before it
# publishes leaves the kernel console on the pin UART, so that cable is the only channel
# carrying the failure; an ACM that never enumerates would report silence for a live board.
if [ "${CONSOLE_PIN:-0}" = "1" ]; then
  CONSOLE_USB_CDC=0
fi

# A TAG can COLLIDE with a build dir an earlier session left behind, and then the generator loads
# that dir's stale generated/.config and refuses a symbol this tree does not declare. It reads as a
# broken preset. Measured 2026-08-10: TAG=m481r hit a 2026-08-07 dir whose .config still carried a
# knob since removed. That knob is deliberately not spelled here, because this file is tracked and
# doc_names would take a dead name from it into its valid set and stop reporting it in the docs.
if ! CFGOUT=$(cmake --preset "$BOARD-$VARIANT" -B "$BUILD" "${EXTRA[@]+"${EXTRA[@]}"}" 2>&1); then
  printf '%s\n' "$CFGOUT" | tail -20 >&2
  if printf '%s\n' "$CFGOUT" | grep -q 'no such symbol'; then
    echo "REFUSING: $BUILD holds a stale generated/.config from an earlier session." >&2
    echo "  This is a TAG collision, not a broken tree: rm -rf $BUILD and retry." >&2
  fi
  exit 1
fi
printf '%s\n' "$EXTRA_WANT" > "$EXTRA_STAMP"
cmake --build "$BUILD" -j8 --target "$APP" > /dev/null || exit 1

# The emitted image base, without extension. Board-specific apps are searched FIRST, the
# same order tools/flash-common.sh uses, so a name collision resolves the same way here.
#
# The last two candidates are the two-image split: `selftest_p2` is declared by
# user/apps/common/selftest/CMakeLists.txt, so CMake emits it into the SELFTEST directory and
# there is no selftest_p2/ directory to find. tools/flash-common.sh's _app_base has the same
# blind spot, which is why the old f302nucleo branch hardcoded its .bin path.
BASE=${APP%_p[0-9]}
IMG=""
for d in "$PWD/$BUILD/user/apps/$BOARD/$APP" "$PWD/$BUILD/user/apps/common/$APP" \
         "$PWD/$BUILD/user/apps/$BOARD/$BASE" "$PWD/$BUILD/user/apps/common/$BASE"; do
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

# --- boards here ---------------------------------------------------------------
if [ -z "${BENCH_HOST:-}" ]; then
  # KICKOS_RIG is passed explicitly rather than left to the capture script's own
  # discovery: TREE may be a worktree, which has no .session/ to discover.
  ROOT="$PWD" KICKOS_RIG="$RIG_CONF" PYBIN="${RIG_PYBIN:-${PY:-}}" \
    CONSOLE_USB_CDC="$CONSOLE_USB_CDC" \
    exec "$HERE/bench-capture.sh" "$BOARD" "$APP" "$IMG" "$LOG" "$SN"
fi

# --- boards on the bench host --------------------------------------------------
rig_need RIG_REMOTE_ROOT "the directory on the bench host holding the shipped tree and the run outputs"
PORT="${BENCH_PORT:-${RIG_BENCH_PORT:-22}}"
RROOT="${RIG_REMOTE_ROOT}/tree"
RRUN="${RIG_REMOTE_ROOT}/run/$TAG-$BOARD-$APP"
RLOG="$RRUN/$TAG-$BOARD-$APP.log"
echo "=== bench host $BENCH_HOST:$PORT"

SSH=(ssh -p "$PORT" -o BatchMode=yes "$BENCH_HOST")
RSH="ssh -p $PORT -o BatchMode=yes"

# rsync will not create an intermediate destination directory, and .session/ over there
# holds nothing but the shipped rig config.
"${SSH[@]}" "mkdir -p $RROOT/.session $RRUN" || { echo "REFUSING: cannot create $RRUN on $BENCH_HOST" >&2; exit 1; }

# tools/ and boards/ are the flash recipes, not a second copy of them: the backends read
# boards/<board>/board.cmake for the chip and take the image through FLASH_IMAGE, so the
# bench host runs the same recipe this tree ships. rsync means only the delta travels.
# The capture chain rides along inside tools/bench/, so it is the same tree's copy too.
rsync -a --delete -e "$RSH" tools boards "$BENCH_HOST:$RROOT/" || { echo "REFUSING: could not ship tools/ and boards/" >&2; exit 1; }
# The rig config is the one thing tools/ cannot carry: it is gitignored, and the console
# cable it names is a property of the CABLE, so it is valid wherever that cable is plugged.
rsync -a -e "$RSH" "$RIG_CONF" "$BENCH_HOST:$RROOT/.session/rig.conf" || { echo "REFUSING: could not ship the rig config" >&2; exit 1; }

# Every sibling the flashers may want: JLinkExe loads the .hex, st-flash the .bin, esptool
# the .app.bin. Ship whichever exist rather than deciding per board twice.
IMGS=()
for f in "$IMG" "$IMG.hex" "$IMG.bin" "$IMG.app.bin"; do
  [ -e "$f" ] && IMGS+=("$f")
done
[ "${#IMGS[@]}" -gt 0 ] || { echo "REFUSING: no image files to ship for $APP" >&2; exit 1; }
rsync -a -e "$RSH" "${IMGS[@]}" "$BENCH_HOST:$RRUN/" || { echo "REFUSING: could not ship the image" >&2; exit 1; }

# The remote login shell is zsh, which does not word-split and ABORTS on an unmatched
# glob, so a command line assembled here would be re-parsed there under different rules.
# Feeding bash a heredoc on stdin and passing the arguments after `--` keeps the parsing
# rules the same on both sides.
#
# EVERY argument is non-empty, and "-" carries "none". ssh joins argv into one string and
# the remote shell re-splits it, so an EMPTY argument does not arrive at all and every
# later positional shifts up one. SN is empty on four of the six boards, so passing it raw
# would hand the capture script a shifted argument list on exactly those boards.
ROUT=$(mktemp)
"${SSH[@]}" bash -s -- "$BOARD" "$APP" "$RRUN/$APP" "$RLOG" "${SN:--}" "${CAP_SECS:--}" \
    "$RIG_REMOTE_ROOT" "${RIG_REMOTE_PYBIN:--}" "$CONSOLE_USB_CDC" <<'REMOTE' 2>&1 | tee "$ROUT"
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
exec bash "$ROOT/tools/bench/bench-capture.sh" "$1" "$2" "$HOME/$3" "$HOME/$4" "$SN"
REMOTE
RC=${PIPESTATUS[0]}
# THE LOG IS THE ARTIFACT, THE VERDICT IS SEPARATE, SO THE FETCH RUNS EITHER WAY. Exiting on
# a nonzero verdict here used to strand the capture on the bench host, where a later hand
# rsync was the only way to read a run that had in fact completed. Nothing about a refusal
# makes the bytes less real, and the refusals most worth reading are the ones with a log.
RBYTES=$(sed -n 's/^bytes: *//p' "$ROUT" | head -1)
rm -f "$ROUT"
FETCHED=0
if rsync -a -e "$RSH" "$BENCH_HOST:$RLOG" "$LOG" 2>/dev/null; then
  FETCHED=1
  LBYTES=$(wc -c < "$LOG")
fi

if [ "$RC" -ne 0 ]; then
  # The remote refusal stays the headline and keeps its exit code; whether the log survived
  # only changes what there is to read. A refusal that fired before the capture wrote
  # anything legitimately has no log, so a failed fetch here is not a second defect.
  if [ "$FETCHED" -eq 1 ]; then
    echo "log: $LOG  ($LBYTES bytes, fetched despite the refusal -- read it before re-running)" >&2
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
