#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# ONE board, ONE already-built image: flash it, capture the console, and refuse rather
# than produce a plausible-looking wrong log. It takes an already-built image and a resolved
# board; bench.sh owns the build and the ssh.
#
#   bench-capture.sh <board> <app> <image-base> <log> [jlink-sn]
#
# <image-base> is the emitted image WITHOUT extension; .hex/.bin/.app.bin derive from it.
#
# THIS SCRIPT IS THE ONE THAT RUNS ON THE BENCH. bench.sh runs it here when the boards
# are here and ships it to the bench host when they are not, so every refusal below
# happens where the hardware is and travels back as output plus an exit code.
#
# THE LOGS THIS SCRIPT WRITES ARE RAW: nothing here strips CR, and nothing here may start. The
# console lowers '\n' to CR+LF on every board but the sim (KICKOS_CONSOLE_CRLF), so a line
# arriving as CR+LF was written through the cooking path and a BARE LF was written by
# arch_console_write_sync, the arch layer speaking under its own steam. That difference is how
# a message is attributed to a layer. tests/lib/gate.sh normalises instead, and the two paths
# are correctly different.
#
# SO ANY PATTERN WRITTEN AGAINST THESE LOGS MUST BE CR-TOLERANT: leave it unanchored, or anchor
# it explicitly CR-optional, NEVER a bare '$'. GNU grep's '$' does not match before a CR while
# the ugrep that shadows `grep` on an interactive shell does, so a bare '$' passes by hand and
# fails where it counts. The one pattern arriving from outside is cap_esp.py's `until` regex,
# taken from its argv, so the rule binds that script's CALLER.
#
# THE SAME EXPOSURE BINDS ITS EDITORS. bench.sh ships this script to whichever host the boards
# are on, so it runs against that host's sed: no `sed -i`, which GNU and BSD spell
# incompatibly, and no `0,/re/` address, which is GNU's alone.
#
# Env:
#   ROOT        directory holding tools/ and boards/ (the flash recipes). Default: this
#               script's grandparent.
#   KICKOS_RIG  the rig config naming the console cables. Default: $ROOT/.session/rig.conf.
#   PYBIN       a python carrying pyserial. Espressif capture only.
#   CAP_SECS    capture window override.
#   EXPECT_COMMIT  the label cmake/build_stamp.cmake stamped into the image, which a banner
#               whose commit row arrived damaged is recovered against. Default: none.
#   EXPECT_ARMS how many arms THIS IMAGE plans, and EXPECT_SKIPS / EXPECT_PARTIALS /
#               EXPECT_FAULTS the permission sets, all read by bench.sh out of the build's own
#               kickos-selftest-manifest.txt. A `selftest*` capture without EXPECT_ARMS is
#               refused: a stream checked only against itself passes whatever it lost.
#   PEER_ERASE  `<from>:<to>` flash windows erased before the image is loaded, which bench.sh
#               derives for an own-image AMP node's image loaded alone. Default: none.
#   TEENSY_LOAD_SECS  per-load bound for teensy41's HalfKay flash. Default 60.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="${ROOT:-$(cd "$HERE/../.." && pwd)}"

BOARD="${1:?usage: bench-capture.sh <board> <app> <image-base> <log> [jlink-sn]}"
APP="${2:?}"
IMG="${3:?}"
LOG="${4:?}"
SN="${5:-}"

# Sourced SOFT: only the cable rows below need it, and a board whose by-id prefix names
# its probe unambiguously must not be held hostage to a config it takes nothing from.
# RIG_CONF is set either way, so the refusal can name the file it looked in.
. "$HERE/rig.sh"
. "$HERE/bench-host.sh"
. "$HERE/board-rows.sh"
. "$HERE/reset-boot.sh"
. "$HERE/stamper.sh"
. "$HERE/banner.sh"
rig_find "$ROOT" || true

# The bench host keeps uv's esptool and the rfp-cli wrapper in ~/.local/bin, which a
# non-interactive ssh does not put on PATH. APPENDED, not prepended: on a box with real
# toolchains those must keep winning.
PATH="$PATH:$HOME/.local/bin"
export PATH

READER=""
READER_GROUP=0
# The reader armed last, and with it every process of its group when it leads one.
stop_reader() {
  [ -n "$READER" ] || return 0
  if [ "$READER_GROUP" = 1 ]; then
    kill -- "-$READER" 2>/dev/null
  else
    kill "$READER" 2>/dev/null
  fi
  READER=""
}
# A refusal leaves no reader holding the port and no stamper running behind it.
stop_capture() {
  stop_reader
  stamper_stop || true
}
refuse() {
  stop_capture
  printf 'REFUSING: %s\n' "$*" >&2
  exit 1
}

picotool_why() {
  local line
  line=$(printf '%s\n' "$1" | grep -v '^flash: +' | grep -m1 -iE 'error|fail|more than one|no accessible|not found|unable|cannot')
  [ -n "$line" ] || line=$(printf '%s\n' "$1" | grep -v '^flash: +' | tail -n 1)
  printf '%s' "${line:-no output}"
}

[ -x "$ROOT/tools/flash.sh" ] || refuse "no $ROOT/tools/flash.sh: ROOT does not hold the flash recipes"
[ -f "$ROOT/boards/$BOARD/board.cmake" ] || refuse "no $ROOT/boards/$BOARD/board.cmake"
[ -e "$IMG" ] || [ -e "$IMG.hex" ] || refuse "no image at $IMG or $IMG.hex"
# Only the picotool loads erase the peers' flash windows; any other route would leave them to run.
if [ -n "${PEER_ERASE:-}" ]; then
  case $BOARD in
    picopi|pizero2350) ;;
    *) refuse "PEER_ERASE names flash windows to erase ($PEER_ERASE), and $BOARD's load erases none" ;;
  esac
fi

mkdir -p "$(dirname "$LOG")" || refuse "cannot create the log directory for $LOG"

# Resolve the console by SERIAL, never by a ttyACM/ttyUSB number: flashing re-enumerates
# a probe, so a number resolved before the flash can name a different device after it.
#
# The row itself is tools/bench/board-rows.sh, which bench-present.sh reads too, so what a
# capture opens and what an operator is told it would open cannot drift apart.
console_row "$BOARD" "$SN" "${CONSOLE_USB_CDC:-0}"
case $? in
  1) refuse "no console row for $BOARD; add one to tools/bench/board-rows.sh rather than
  guessing its probe. tools/bench/bench-present.sh consoles says what every row this tree
  carries resolves to right now." ;;
  2) refuse "CONSOLE_USB_CDC is set but $BOARD has no USB device controller backend" ;;
esac
PORT="$CONSOLE_PORT"
PATTERN="$CONSOLE_PATTERN"
RIGKEY="$CONSOLE_RIGKEY"

# Resolve $PATTERN to exactly one device. Split out because a self-USB console does not
# exist until the image boots, so that route calls this after the flash.
resolve_console() {
  if [ -n "$PORT" ]; then
    # A VCOM path derived from a probe serial exists only if that serial is a probe that is
    # really attached. Unchecked, a wrong serial reads as a resolved console and the failure
    # surfaces as an empty log, which is what a silent board looks like.
    [ -e "$PORT" ] || refuse "$BOARD's console $PORT is not on this bus: the probe serial it
  derives from names no attached probe. tools/bench/bench-present.sh $BOARD says which
  probes are here."
    return 0
  fi
  [ -n "$PATTERN" ] || refuse "$BOARD's console cable is not named: set $RIGKEY in $RIG_CONF
  (see tools/bench/rig.conf.example). There is deliberately no vendor-pattern fallback
  here; it resolves to somebody else's cable and the capture still looks right.
  tools/bench/bench-present.sh consoles says which cables this bus carries."
  mapfile -t MATCHES < <(console_expand_one "$PATTERN")
  # More than one match is the same wrong-cable failure wearing a different hat, so it is
  # refused rather than resolved by taking the first.
  if [ "${#MATCHES[@]}" -gt 1 ]; then
    printf 'REFUSING: %s matches %d devices, so which one is this board is a guess:\n' \
           "$PATTERN" "${#MATCHES[@]}" >&2
    printf '  %s\n' "${MATCHES[@]}" >&2
    echo "  Pin $RIGKEY in $RIG_CONF to the one that is the console." >&2
    exit 1
  fi
  PORT="${MATCHES[0]:-}"
  [ -n "$PORT" ] && [ -e "$PORT" ] || refuse "no console for $BOARD (looked for ${PATTERN:-$PORT})"
  # Nothing else may hold the port. An orphaned reader from an earlier run writes at its own
  # offset and the two logs interleave into something that still looks complete.
  if fuser "$PORT" 2>/dev/null; then
    refuse "$PORT is held. Kill the PID fuser reports, never pkill on the port.
  tools/bench/bench-present.sh holders names it from here."
  fi
}

# A reader for a console that DOES NOT EXIST YET, spinning on the path from before the
# flash. The device appears when the image boots and leaves when it ends, and for an app that
# ends in the bootloader that is about as long as a poll loop takes to notice, so the reader
# must already be waiting.
#
# The glob resolves per open, the path not existing to pin when this is armed.
#
# The kernel banner never reaches this route. The identity rows the driver opens each host
# configuration with stand in for it (bench.sh route_identity).
arm_waiting_reader() {
  [ -n "${PYBIN:-}" ] || refuse "PYBIN is unset: a USB CDC capture reads through cap_cdc.py (RIG_PYBIN here, RIG_REMOTE_PYBIN on the bench host)"
  [ -f "$HERE/cap_cdc.py" ] || refuse "no $HERE/cap_cdc.py"
  setsid bash -c '
    shopt -s nullglob
    while true; do
      m=($1)
      # AMBIGUITY IS REFUSED HERE TOO, not just in resolve_console. Two boards enumerating
      # this product string at once (a fleet pass with two USB-console boards, or a node from
      # the previous capture not yet gone) would otherwise be read in glob order and the
      # capture would be of whichever came first, silently and plausibly.
      if [ "${#m[@]}" -gt 1 ]; then
        printf "REFUSING: %d KickOS consoles match %s; which one is this board is a guess:\n" \
               "${#m[@]}" "$1" >&2
        printf "  %s\n" "${m[@]}" >&2
        exit 1
      fi
      # ONE OPEN, held across the raw setting and the read: cdc_acm discards what it received
      # when the last opener closes, and the first bytes the device sends are its identity
      # rows. The device holds its output until a line coding, which cdc_acm sends from inside
      # the call that makes the tty raw; cap_cdc.py makes sure that call carries one.
      if [ "${#m[@]}" -eq 1 ]; then
        "$2" "$3" "${m[0]}" 2>/dev/null
      fi
      sleep 0.05
    done' _ "$PATTERN" "$PYBIN" "$HERE/cap_cdc.py" >> "$LOG" &
  READER=$!
  READER_GROUP=1
}

# Answered from the LOG, the device having usually gone by now. dmesg_restrict is 1 here, so
# the kernel log cannot separate the two failures either.
check_cdc_capture() {
  if [ -s "$LOG" ]; then
    return 0
  fi
  refuse "the USB CDC console produced nothing (looked for $PATTERN).
  Either it never enumerated, or it enumerated and said nothing, and this bench cannot
  tell those apart. Check the image's composition really names the USB console driver."
}

if [ "${CONSOLE_USB_CDC:-0}" != "1" ]; then
  resolve_console
  echo "=== $BOARD  console $PORT  image $IMG"
else
  echo "=== $BOARD  console <its own USB CDC, after the flash>  image $IMG"
fi
: > "$LOG" || refuse "cannot write $LOG"
rm -f "$LOG.bus"
# Every route's lines are stamped on arrival here, for a judge that times the board against this
# host. It stops at the capture window's end, before a route cuts the log's head away.
stamper_start "$HERE" "$LOG" "$$" || refuse "$STAMPER_WHY"
stop_stamper() {
  stamper_stop || refuse "$STAMPER_WHY"
}

# A reader armed before the flash must still be alive after it. An FTDI reverts min/time
# when the last opener closes, and a dead reader leaves a 0-byte log that reads exactly
# like a board that printed nothing.
check_reader() {
  [ -n "$READER" ] || return 0
  ps -p "$READER" > /dev/null || refuse "the reader died $1"
}

# AFTER the capture window a dead reader is NOT a failure, and refusing on it is a false
# negative: a bare cat exits at EOF when the board goes quiet and the VCOM hangs up, which on
# the K64F happens once the suite has finished and every byte is already in the log. What
# actually matters is whether the LOG is complete, and the plan and count checks at the bottom
# decide that.
note_reader() {
  stop_stamper
  [ -n "$READER" ] || return 0
  ps -p "$READER" > /dev/null && return 0
  echo "NOTE: the reader exited before the window closed (EOF on a quiet port). The counts" >&2
  echo "  below decide whether the capture is complete." >&2
}

# The FTDI-style reader is a LOOP around cat, so killing the loop leaves the running cat
# alive and REPARENTED, still holding the port, and it then splits the bytes of the next
# capture with that capture's own reader. `pkill -P` does not reap it and a pattern kill
# would match its own command line. So the loop runs in its OWN PROCESS GROUP and the whole
# group is signalled.
arm_wrapped_reader() {
  setsid bash -c 'while true; do cat "$1"; sleep 0.2; done' _ "$1" >> "$LOG" &
  READER=$!
  READER_GROUP=1
}
stop_wrapped_reader() {
  [ -n "$READER" ] || return 0
  stop_reader
  sleep 1
  # The holder check needs a pinned $PORT; a self-USB console has none, and is usually off
  # the bus by now.
  [ -n "$PORT" ] || return 0
  if fuser "$PORT" 2>/dev/null; then
    echo "WARNING: $PORT is STILL held after the reader teardown. Kill the PID fuser" >&2
    echo "  reports before the next capture, or it will split the bytes." >&2
  fi
}

# Per presence row of the board (board_probe_rows, less its `seen` ones),
# `<vid:pid>|<identity>|<what>`: the device this capture selects, by the serial it was given for an
# `sn` row and otherwise by the port the row's one device sits on before the flash, or
# `none: <why>` where neither establishes one.
BUS_IDENTITY=$(
  _bus_read=0
  if bench_bus_read; then
    _bus_read=1
  fi
  board_probe_rows "$BOARD" | while IFS='|' read -r _bus_id _bus_kind _bus_what; do
    if [ "$_bus_kind" = seen ]; then
      continue
    fi
    if [ "$_bus_kind" = sn ]; then
      if [ -n "$SN" ]; then
        printf '%s|serial %s|%s\n' "$_bus_id" "$SN" "$_bus_what"
      else
        printf '%s|none: no serial was given to select it by|%s\n' "$_bus_id" "$_bus_what"
      fi
      continue
    fi
    if [ "$_bus_read" = 0 ]; then
      printf '%s|none: the bus could not be read before the flash|%s\n' "$_bus_id" "$_bus_what"
      continue
    fi
    _bus_ports=$(usb_ports_of "$_bus_id")
    _bus_n=$(printf '%s\n' "$_bus_ports" | grep -c .)
    if [ "$_bus_n" -eq 1 ]; then
      printf '%s|port %s|%s\n' "$_bus_id" "$_bus_ports" "$_bus_what"
    else
      printf '%s|none: %s devices at %s before the flash|%s\n' "$_bus_id" "$_bus_n" "$_bus_id" "$_bus_what"
    fi
  done
)

case $BOARD in
  esp32c6-wroom|esp32-wroom)
    # esptool finishes by resetting into the app, so the run is over before a separate
    # reader could be armed. cap_esp.py opens the port ONCE, pulses RTS itself and reads.
    # FLASH_PORT is not optional: the backend otherwise picks the first ttyACM it finds,
    # which on this bench is another board's probe VCOM.
    [ -n "${PYBIN:-}" ] || refuse "PYBIN is unset: the Espressif capture needs a python carrying pyserial (RIG_PYBIN here, RIG_REMOTE_PYBIN on the bench host)"
    [ -x "${PYBIN}" ] || refuse "PYBIN=$PYBIN is not executable"
    [ -f "$HERE/cap_esp.py" ] || refuse "no $HERE/cap_esp.py"
    FLASH_PORT="$PORT" FLASH_IMAGE="$IMG" "$ROOT/tools/flash.sh" "$BOARD" "$APP" || refuse "the flash failed"
    "$PYBIN" "$HERE/cap_esp.py" "$PORT" "$LOG" "${CAP_SECS:-40}" '^1\.\.[0-9]+' > /dev/null 2>&1
    ;;
  f411disco)
    # The probe is an ST-Link V2 with NO VCP, so the console is a separate FTDI named by
    # RIG_CONSOLE_F411DISCO and the stty re-arm below is what keeps that cable from returning
    # EOF at once.
    #
    # THE WRITE'S OWN BOOT IS NOT READ: it starts without a reset and under halting debug, and
    # in some runs faults at once and halts on a vector catch. A reset under the armed reader
    # starts the boot this capture reads.
    #
    # THE WRITE AND THE RESET GO THROUGH tools/flash.sh, so the bench recipe and the hand
    # recipe are one command.
    #
    # FLASH_TOOL IS PINNED, not left to the dispatcher: candidates_for() offers "stlink jlink"
    # for stm32f411 and takes the first on PATH, so on a host without stlink-tools this would
    # silently become a J-Link SWD flash, a different recipe reached by accident. Pinning it
    # keeps the missing-tool case a named refusal.
    command -v st-flash > /dev/null || refuse "st-flash not on PATH (apt install stlink-tools)"
    [ -e "$IMG.bin" ] || refuse "no $IMG.bin (st-flash loads the raw binary)"
    # CHECKED, and its output kept: a failed write leaves the PREVIOUS image running, which is
    # the failure that most looks like a pass.
    if ! WOUT=$(FLASH_TOOL=stlink FLASH_IMAGE="$IMG" "$ROOT/tools/flash.sh" "$BOARD" "$APP" 2>&1); then
      printf '%s\n' "$WOUT" | tail -8 >&2
      refuse "the $BOARD write failed (tools/flash.sh -> flash-stlink.sh on $IMG.bin)"
    fi
    stty -F "$PORT" 115200 raw -echo -hupcl clocal min 1 time 0 || refuse "stty failed on $PORT"
    cat "$PORT" >> "$LOG" &
    READER=$!
    READER_GROUP=0
    sleep 1
    check_reader "on arming"
    stamper_truncate "$HERE" "$LOG" "$$" || refuse "$STAMPER_WHY"
    # Taken BEFORE the reset: the core restarts at the read's detach, ahead of st-flash's exit,
    # so the boot's first lines are already in the log when the command returns. What the earlier
    # image prints past this until the connect stops it carries no banner title.
    _reset_bytes=$(wc -c < "$LOG")
    if ! WOUT=$(FLASH_STLINK_RESET=1 FLASH_TOOL=stlink FLASH_IMAGE="$IMG" \
        "$ROOT/tools/flash.sh" "$BOARD" "$APP" 2>&1); then
      printf '%s\n' "$WOUT" | tail -8 >&2
      refuse "the $BOARD reset after the write failed"
    fi
    sleep "${CAP_SECS:-25}"
    note_reader
    stop_reader
    _why=$(reset_boot_cut "$LOG" "$_reset_bytes") || refuse "$_why"
    ;;
  picopi|pizero2350)
    command -v picotool > /dev/null || refuse "picotool not on PATH"
    # A board that has already run KickOS once needs a POWER CYCLE, not a reset: J-Link finds the
    # SW-DP and then fails to power up the DAP, and BOOTSEL is the only way back. So a second run
    # in one session fails HERE, and picotool's own words are what say so.
    if [ "${CONSOLE_USB_CDC:-0}" = "1" ]; then
      # Armed FIRST, though what it reads does not exist yet (see arm_waiting_reader).
      # BOOTSEL and the image's console are different devices, never on the bus together.
      arm_waiting_reader
      sleep 1
      check_reader "on arming"
      if ! POUT=$(PICOTOOL_SN="$SN" FLASH_ERASE_RANGES="${PEER_ERASE:-}" FLASH_IMAGE="$IMG" "$ROOT/tools/flash-picotool.sh" "$BOARD" "$APP" 2>&1); then
        stop_wrapped_reader
        printf '%s\n' "$POUT" | tail -8 >&2
        refuse "picotool could not flash $BOARD: $(picotool_why "$POUT"). Already ran KickOS? Power-cycle it into BOOTSEL."
      fi
    else
      # picotool load -x reboots straight into the app, so the run is over before a reader armed
      # afterwards would exist. The console is a SEPARATE FTDI from the RP2 Boot interface, so
      # arming it first cannot disturb programming. Same FTDI re-arm as rx72m.
      stty -F "$PORT" 115200 raw -echo -hupcl clocal min 1 time 0 || refuse "stty failed on $PORT"
      arm_wrapped_reader "$PORT"
      sleep 1
      check_reader "on arming"
      if ! POUT=$(PICOTOOL_SN="$SN" FLASH_ERASE_RANGES="${PEER_ERASE:-}" FLASH_IMAGE="$IMG" "$ROOT/tools/flash-picotool.sh" "$BOARD" "$APP" 2>&1); then
        printf '%s\n' "$POUT" | tail -8 >&2
        refuse "picotool could not flash $BOARD: $(picotool_why "$POUT"). Already ran KickOS? Power-cycle it into BOOTSEL."
      fi
    fi
    sleep "${CAP_SECS:-25}"
    note_reader
    stop_wrapped_reader
    if [ "${CONSOLE_USB_CDC:-0}" = "1" ]; then
      check_cdc_capture
    fi
    ;;
  teensy41)
    # HalfKay reboots into the app the moment the load completes, so a reader armed
    # afterwards misses the run. The console is an FTDI on LPUART6 (Serial1), a different
    # USB device from the HalfKay HID, so arming it first cannot disturb programming and
    # is what captures the banner. Same FTDI re-arm as rx72m.
    command -v teensy_loader_cli > /dev/null || refuse "teensy_loader_cli not on PATH"
    if [ "${CONSOLE_USB_CDC:-0}" = "1" ]; then
      # Armed FIRST; see arm_waiting_reader. HalfKay and the image's CDC are never on the
      # bus together.
      arm_waiting_reader
      sleep 1
      check_reader "on arming"
    else
      stty -F "$PORT" 115200 raw -echo -hupcl clocal min 1 time 0 || refuse "stty failed on $PORT"
      arm_wrapped_reader "$PORT"
      sleep 1
      check_reader "on arming"
    fi
    # THE FIRST LOAD FAILS AND THE SECOND SUCCEEDS, reliably enough that an unattended pass
    # cannot leave the retry to a human. Cause NOT established; do not infer one from the
    # retry working.
    # BOTH LOADS BOUNDED: -w blocks until a HalfKay device appears, and a load that lands
    # takes HalfKay away. A first attempt failing cosmetically on a flash that worked would
    # leave the retry waiting on a device only a button press brings back.
    if ! timeout "${TEENSY_LOAD_SECS:-60}" env FLASH_IMAGE="$IMG" \
           "$ROOT/tools/flash.sh" teensy41 "$APP" > /dev/null 2>&1; then
      echo "note: the first HalfKay load failed, which is usual on this board; retrying" >&2
      sleep 2
      if ! TOUT=$(timeout "${TEENSY_LOAD_SECS:-60}" env FLASH_IMAGE="$IMG" \
                    "$ROOT/tools/flash.sh" teensy41 "$APP" 2>&1); then
        stop_wrapped_reader
        printf '%s\n' "$TOUT" | tail -8 >&2
        refuse "the teensy41 load failed TWICE. Is the board in HalfKay? Tap its button:
  teensy_loader_cli cannot enter it (-s needs the Teensyduino serial stack, -r the
  rebootor hardware), so a board running KickOS has to be put there by hand."
      fi
    fi
    sleep "${CAP_SECS:-30}"
    note_reader
    stop_wrapped_reader
    if [ "${CONSOLE_USB_CDC:-0}" = "1" ]; then
      check_cdc_capture
    fi
    ;;
  rx72m)
    # rfp-cli -run releases reset and the suite is over in about a second, so a reader
    # armed after the flash captures nothing. The console is an FTDI on a DIFFERENT USB
    # device from the E2 Lite, so arming it first cannot disturb programming.
    # The reader is wrapped: when the last opener closes the port the FTDI driver reverts
    # min/time to 0, the next read returns 0 bytes and a bare cat takes that as EOF.
    command -v rfp-cli > /dev/null || refuse "rfp-cli not on PATH"
    stty -F "$PORT" 115200 raw -echo -hupcl clocal min 1 time 0 || refuse "stty failed on $PORT"
    arm_wrapped_reader "$PORT"
    sleep 1
    check_reader "on arming"
    if ! FLASH_PORT="$PORT" FLASH_IMAGE="$IMG" "$ROOT/tools/flash.sh" rx72m "$APP"; then
      stop_wrapped_reader
      # The E2 Lite is a libusb device, not a tty, so dialout does not reach it. Its udev
      # rule guards on ACTION=="add", which a plain `udevadm trigger` (a change event)
      # skips outright, which is why the rule can be installed and the node still be
      # root:root 664.
      refuse "the rx72m flash failed. If rfp-cli could not open the programmer, its USB
  node is not writable: sudo udevadm trigger --action=add --attr-match=idVendor=045b"
    fi
    sleep "${CAP_SECS:-30}"
    note_reader
    stop_wrapped_reader
    ;;
  *)
    case $BOARD in
      frdmk64f)      DEV=MK64FN1M0xxx12 ;;
      xmc4800-relax) DEV=XMC4800-2048 ;;
      blackpill)     DEV=STM32F411CE ;;
      *) refuse "no -device row for $BOARD; add one rather than passing an empty -device" ;;
    esac
    [ -n "$SN" ] || refuse "$BOARD needs its probe serial: two J-Links on one bench and JLinkExe grabs whichever it likes"
    command -v JLinkExe > /dev/null || refuse "JLinkExe not on PATH"
    # SEGGER's J-Link OpenSDA firmware shows a licence notice ONCE PER CALENDAR DAY, and
    # the acknowledgement is date-stamped in
    # ~/.config/SEGGER/SEGGER_REG_HKEY_CURRENT_USER.xml as LicenseOpenSDA_DontShowAgainToday.
    # The first JLinkExe of a day waits on it, and every J-Link call here sends output to
    # /dev/null, so it surfaces as a hang or as "Failed to halt CPU" and reads as dead
    # silicon. The descending-speed probe below turns that into a refusal that names the
    # cure, and catches a genuinely absent probe BEFORE the flash instead of after the
    # capture is spent. It is bounded, not quick.
    # SPEED IS TRIED DESCENDING. At `-speed 4000` the connect can stop right after
    # InitTarget() and never print "identified.", while at 1000 the SAME probe on the SAME
    # boot identifies the core. There is more than one physical K64F across desks, so treat
    # the usable speed as a per-UNIT fact, not a constant.
    # One bound for every JLinkExe call on this path: a Commander that never returns stalls
    # an unattended campaign.
    JLINK_CALL_SECS=30
    JOUT=""
    SWD_SPEED=""
    PROBE=$(mktemp)
    printf 'connect\nq\n' > "$PROBE"
    for _sp in 4000 1000 400; do
      JOUT=$(timeout "$JLINK_CALL_SECS" JLinkExe -nogui 1 -SelectEmuBySN "$SN" -device "$DEV" -if SWD -speed "$_sp" \
                      -CommanderScript "$PROBE" < /dev/null 2>&1)
      if printf '%s\n' "$JOUT" | grep -q 'identified\.'; then
        SWD_SPEED=$_sp
        break
      fi
      _vt=$(printf '%s\n' "$JOUT" | sed -n 's/.*VTref *= *\([0-9.]*\) *V.*/\1/p' | head -1)
      if [ -n "$_vt" ] && awk -v v="$_vt" 'BEGIN { exit !(v + 0 == 0) }'; then
        VTREF_ZERO=1
        break
      fi
    done
    rm -f "$PROBE"
    if [ "${VTREF_ZERO:-0}" -eq 1 ]; then
      {
        echo "REFUSING: the probe reads VTref 0 V: the target is unpowered or the probe's cable is not on it."
        echo "  SN $SN, so no other SWD speed was tried."
        printf '%s\n' "$JOUT" | grep -vE '^[[:space:]]*$' | tail -5
      } >&2
      stop_capture
      exit 1
    fi
    if [ -z "$SWD_SPEED" ]; then
      {
        echo "REFUSING: no SWD speed in 4000/1000/400 reached a halted core on SN $SN."
        echo "  Speed is already ruled out, so the cause is one of these and the output below"
        echo "  is what separates them:"
        echo "  - no 1366:* in lsusb at all -> the wedge. Replug, or hold the reset button"
        echo "    through a connect-under-reset."
        echo "  - present but no 'identified.' and the run HUNG -> the OpenSDA licence notice."
        echo "    Run JLinkExe once interactively, accept it, retry. Not a wedge."
        printf '%s\n' "$JOUT" | grep -vE '^[[:space:]]*$' | tail -5
      } >&2
      stop_capture
      exit 1
    fi
    if [ "$SWD_SPEED" != 4000 ]; then
      echo "note: SWD at $SWD_SPEED kHz; 4000 did not identify the core on this unit." >&2
    fi
    # THE WRITE'S STATUS IS THE WHOLE ATTRIBUTION. A failed reflash leaves the PREVIOUSLY
    # loaded image on the part, and at the same commit that image stamps the banner this
    # capture is checked against, so the run reads as a clean witness of a build the board
    # never took.
    if ! JOUT=$(JLINK_SN=$SN JLINK_SPEED=$SWD_SPEED FLASH_IMAGE="$IMG" \
                "$ROOT/tools/flash-jlink.sh" "$BOARD" "$APP" 2>&1); then
      printf '%s\n' "$JOUT" | grep -vE '^[[:space:]]*$' | tail -10 >&2
      refuse "the $BOARD flash failed on SN $SN. The part still holds whatever was loaded
  before, which at this commit stamps the banner this capture would be checked against."
    fi
    sleep 12
    [ -e "$PORT" ] || refuse "$PORT vanished after the flash (the probe re-enumerated)"
    stty -F "$PORT" 115200 raw -echo -hupcl clocal min 1 time 0 || refuse "stty failed on $PORT"
    # Wrapped, like the FTDI readers: the JLinkExe reset makes the OpenSDA VCOM hang up, a bare
    # cat then takes that as EOF and ends the capture mid-run. One cat is alive at a time, so
    # this is not the two-readers clobber.
    arm_wrapped_reader "$PORT"
    sleep 1
    check_reader "on arming"
    # A cable stays open across the reset and heard the flash's own boot, so the log is cut to
    # the reset's. The probe's own VCOM hangs up at the reset and is not cut.
    _reset_bytes=""
    case $PORT in
      */usb-SEGGER_J-Link_*)
        ;;
      *)
        stamper_truncate "$HERE" "$LOG" "$$" || refuse "$STAMPER_WHY"
        _reset_bytes=$(wc -c < "$LOG")
        ;;
    esac
    RESET=$(mktemp)
    printf 'r\ng\nq\n' > "$RESET"
    # AND THE RESET IS WHAT STARTS THE RUN, so discarding its status buys a capture window over
    # a part that never left halt. An empty log then reads as a silent board.
    JOUT=$(timeout "$JLINK_CALL_SECS" JLinkExe -nogui 1 -SelectEmuBySN "$SN" -device "$DEV" -if SWD -speed "$SWD_SPEED" \
             -CommanderScript "$RESET" < /dev/null 2>&1)
    JRC=$?
    rm -f "$RESET"
    if [ "$JRC" -eq 124 ]; then
      stop_wrapped_reader
      printf '%s\n' "$JOUT" | grep -vE '^[[:space:]]*$' | tail -10 >&2
      refuse "the $BOARD reset-and-go on SN $SN did not return within ${JLINK_CALL_SECS}s and was
  killed. The part was not started, and an unattended campaign would have waited on it."
    fi
    if [ "$JRC" -ne 0 ]; then
      stop_wrapped_reader
      printf '%s\n' "$JOUT" | grep -vE '^[[:space:]]*$' | tail -10 >&2
      refuse "the $BOARD reset-and-go failed on SN $SN, so the part was not started and the
  window below would capture a halted core."
    fi
    sleep "${CAP_SECS:-25}"
    note_reader
    stop_wrapped_reader
    if [ -n "$_reset_bytes" ]; then
      _why=$(reset_boot_cut "$LOG" "$_reset_bytes") || refuse "$_why"
    fi
    ;;
esac
stop_stamper

# <log>.bus: each BUS_IDENTITY row as the bus reads once the window is over,
# `<vid:pid>|<state>|<identity>|<what>`, the state present or absent for the selected device,
# ambiguous where its identity matches more than one and unidentified where it has none. A board
# that ends in its loader is present by the loader's row, which is what
# tests/integration/check_reboot.sh reads.
if bench_bus_read; then
  printf '%s\n' "$BUS_IDENTITY" | while IFS='|' read -r _bus_id _bus_ident _bus_what; do
    if [ -z "$_bus_id" ]; then
      continue
    fi
    case $_bus_ident in
      "serial "*)
        _bus_n=$(usb_ports_of "$_bus_id" "${_bus_ident#serial }" | grep -c .)
        ;;
      "port "*)
        _bus_n=$(usb_ports_of "$_bus_id" | grep -cxF -- "${_bus_ident#port }")
        ;;
      *)
        _bus_n=none
        ;;
    esac
    case $_bus_n in
      none) _bus_state=unidentified ;;
      0) _bus_state=absent ;;
      1) _bus_state=present ;;
      *) _bus_state=ambiguous ;;
    esac
    printf '%s|%s|%s|%s\n' "$_bus_id" "$_bus_state" "$_bus_ident" "$_bus_what"
  done > "$LOG.bus"
else
  echo "NOTE: the bus could not be read after the window, so $LOG.bus is not written" >&2
fi

# Every `KickOS: ` line the log holds, echoed before this script's own verdict: an image that
# refuses by name says why it produced no plan line.
#
# UNANCHORED: a backend refusal goes out through arch_console_write_sync, a raw writer that
# skips kconsole_write's CRLF cook, so these lines end in a bare LF where every other console
# line ends CRLF.
say_kickos_lines() {
  _sk=$(grep -aF 'KickOS: ' "$LOG" | tail -5)
  if [ -n "$_sk" ]; then
    echo "the image refused by name:" >&2
    printf '%s\n' "$_sk" | sed 's/^/  /' >&2
  fi
}

# A capture that produced nothing must FAIL. An empty log and a board that printed
# nothing are indistinguishable, and an exit code of 0 turns either into a pass.
BYTES=$(wc -c < "$LOG")
[ "$BYTES" -gt 0 ] || refuse "$LOG is 0 bytes: the capture produced nothing"

# NEVER count across plan lines. A log holding two runs sums into something that reads as one
# clean pass, so the authoritative run is the LAST plan line to end of file, and a precursor
# is reported rather than added. A split board runs each of its images as its own invocation
# into its own log, so one log normally owes exactly one plan line, and more than one means the
# board restarted inside the window.
RUNS=$(grep -acE '^1\.\.[0-9]+' "$LOG")
LAST=$(grep -anE '^1\.\.[0-9]+' "$LOG" | tail -1 | cut -d: -f1)
RUN="$LOG"
if [ -n "$LAST" ] && [ "$RUNS" -gt 1 ]; then
  RUN=$(mktemp)
  sed -n "${LAST},\$p" "$LOG" > "$RUN"
fi

OKC=$(grep -acE '^ok ' "$RUN")
NOTOKC=$(grep -acE '^not ok ' "$RUN")
# Banner and posture are taken from the WHOLE log with tail, not from the run slice: they are
# printed BEFORE the plan line, so slicing from the last plan line cuts them off. The LAST banner
# in the file belongs to the last boot, which is the run being counted.

LABEL=$(banner_label "$LOG" "${EXPECT_COMMIT:-}")
BANNER=""
case "$LABEL" in
  "") ;;
  *-UNVERIFIED) BANNER="commit $LABEL (banner damaged in transit; -dirty could not be confirmed)" ;;
  *)
    BANNER="commit $LABEL"
    # A label recovered with its `-dirty` intact is verified; it still arrived damaged and the
    # line says so, which the UNVERIFIED spelling above already carries for the other case.
    if ! grep -aqE "$BANNER_COMMIT_RE" "$LOG"; then
      BANNER="$BANNER (banner damaged in transit)"
    fi
    ;;
esac

# The banner's posture line. NO `$` ANCHOR. Every console line here ends CRLF, and GNU grep counts the CR as part of the
# line while this box's grep (ugrep) does not, so an anchored pattern passes a local test and
# fails on the bench host against byte-identical input.
MPU=$(grep -aoE '(mpu +(enforce|off)|mmu +enforce)' "$LOG" | tail -1)

printf 'bytes:  %s\n' "$BYTES"
printf 'runs:   %s\n' "$RUNS"
printf 'ok:     %s\n' "$OKC"
printf 'not ok: %s\n' "$NOTOKC"
printf 'plan:   %s\n' "$(grep -aoE '^1\.\.[0-9]+' "$RUN" | tail -1)"
# `skip:` counts EVERY SKIP directive, the vacuity ones included, so a reader who does not know
# the second category still sees them. `vac:` is how many of them are an arm declining a timing
# window it did not get on this run rather than a board that cannot host it.
printf 'skip:   %s\n' "$(grep -acE '# SKIP' "$RUN")"
printf 'vac:    %s\n' "$(grep -acE '# SKIP VACUOUS ' "$RUN")"
printf 'part:   %s\n' "$(grep -acE '# PARTIAL' "$RUN")"
printf 'banner: %s\n' "$BANNER"
printf 'mpu:    %s\n' "$MPU"
echo "log: $LOG"

if [ "$RUNS" -gt 1 ]; then
  echo "NOTE: $RUNS plan lines. The counts above are the LAST run only; the earlier one(s)" >&2
  echo "  are a board restart inside the capture window, not extra arms." >&2
fi
# A TAP VERDICT IS OWED ONLY BY A TAP APP. The diagnostic apps announce no plan by design. The
# EXPECTATION comes from the app name; the verdict still comes from the log, so a non-TAP app
# that does emit a plan is judged on it anyway.
case $APP in
  selftest*) WANT_TAP=1 ;;
  *)         WANT_TAP=0 ;;
esac
if [ -z "$LAST" ]; then
  if [ "$WANT_TAP" -eq 1 ] && [ "${CONSOLE_USB_CDC:-0}" = "1" ]; then
    # A console that IS the device delivers only what its ring held until the host opened it,
    # so the plan line can be gone and demanding one refuses such a capture. The TAP verdict
    # below needs a plan, so this route reaches NO verdict at all: what is left is the ok count,
    # which is a count of the lines that arrived. Reconcile the arm total by eye against the
    # manifest.
    if [ "$OKC" -eq 0 ]; then
      say_kickos_lines
      refuse "$LOG carries no plan line AND no ok lines: nothing of the suite arrived"
    fi
    echo "NOTE: no plan line; a USB CDC console loses what its ring could not hold before the" >&2
    echo "  host opened it, this plan line included. $OKC ok line(s) and the arms below the" >&2
    echo "  first one are NOT accounted for," >&2
    echo "  and the TAP verdict needs the plan, so THIS CAPTURE IS NOT JUDGED: derive the" >&2
    echo "  expected count and check it by hand." >&2
  elif [ "$WANT_TAP" -eq 1 ]; then
    say_kickos_lines
    refuse "$LOG has no plan line at all: the suite never announced itself"
  else
    echo "note: $APP announces no TAP plan, so no arm counts are owed. Read the log." >&2
  fi
elif [ "$OKC" -eq 0 ]; then
  say_kickos_lines
  refuse "the last run in $LOG carries a plan line but no ok lines"
fi

# THE PRODUCER'S OWN VERDICT, NOT THIS SCRIPT'S COUNT OF THE LINES THAT SURVIVED. Everything
# printed above is a grep over the capture, so a console that dropped whole lines under producer
# pressure shrinks `ok:` and `part:` with the loss while the harness's trailer, computed inside
# the image before anything went missing, still says what happened. Read on their own the two
# agree, and the reader silently outvotes the producer: `ok: 123` against a plan of `1..125` with
# `# 1 test(s) failed` in the same log exited 0 here. check_tap_stream.sh reconciles the trailer,
# the plan, the case count, the numbering and every directive against the arm count the build
# recorded, and it is the same script the sim and QEMU gates run.
if [ "$WANT_TAP" -eq 1 ] && [ -n "$LAST" ]; then
  TAPGATE="$ROOT/tests/integration/check_tap_stream.sh"
  [ -r "$TAPGATE" ] || refuse "no $TAPGATE: this ROOT does not carry the TAP verdict, so the
  stream above is a count of the lines that arrived and nothing checked it against the suite
  the build planned. bench.sh ships tests/ beside tools/ and boards/."
  # An arm count is not optional and there is no defaulting to the plan the capture announced:
  # a suite that lost an arm at BUILD time shrinks the plan and the case count in lockstep, and
  # only a number from outside the image can see it.
  [ -n "${EXPECT_ARMS:-}" ] || refuse "$APP is a TAP app and EXPECT_ARMS is unset, so nothing
  says how many arms this image plans and the stream can only be checked against itself.
  bench.sh reads it out of the build's own kickos-selftest-manifest.txt."
  echo "left out at build: ${SELFTEST_LEFT_OUT:-none}"
  if ! EXPECT_SKIPS="${EXPECT_SKIPS:-}" EXPECT_PARTIALS="${EXPECT_PARTIALS:-}" \
       EXPECT_FAULTS="${EXPECT_FAULTS:-}" \
       sh "$TAPGATE" "$BOARD/$APP" "$EXPECT_ARMS" < "$RUN"; then
    say_kickos_lines
    refuse "$LOG is not a clean run of $APP on $BOARD; the findings are above."
  fi
fi
