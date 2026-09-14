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
# THE SAME EXPOSURE BINDS THE EDITORS THE CONTROLS BELOW ARE BUILT WITH. bench.sh ships this
# script to whichever host the boards are on, so it runs against that host's sed: no `sed -i`,
# which GNU and BSD spell incompatibly, and no `0,/re/` address, which is GNU's alone. Every
# plant is built by printf or by redirection instead.
#
# Env:
#   ROOT        directory holding tools/ and boards/ (the flash recipes). Default: this
#               script's grandparent.
#   KICKOS_RIG  the rig config naming the console cables. Default: $ROOT/.session/rig.conf.
#   PYBIN       a python carrying pyserial. Espressif capture only.
#   CAP_SECS    capture window override.
#   EXPECT_COMMIT  the label cmake/build_stamp.cmake stamped into the image, which the banner
#               must carry. Default: this ROOT's own `git describe --dirty --always`, which is
#               right only when the image was built here.
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
rig_find "$ROOT" || true

# The bench host keeps uv's esptool and the rfp-cli wrapper in ~/.local/bin, which a
# non-interactive ssh does not put on PATH. APPENDED, not prepended: on a box with real
# toolchains those must keep winning.
PATH="$PATH:$HOME/.local/bin"
export PATH

refuse() { printf 'REFUSING: %s\n' "$*" >&2; exit 1; }

[ -x "$ROOT/tools/flash.sh" ] || refuse "no $ROOT/tools/flash.sh: ROOT does not hold the flash recipes"
[ -f "$ROOT/boards/$BOARD/board.cmake" ] || refuse "no $ROOT/boards/$BOARD/board.cmake"
[ -e "$IMG" ] || [ -e "$IMG.hex" ] || refuse "no image at $IMG or $IMG.hex"

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
# flash. The device appears when the image boots and leaves when it ends, and with
# KICKOS_SHUTDOWN_TO_BOOTLOADER=ON that is about as long as a poll loop takes to notice, so
# the reader must already be waiting.
#
# The glob resolves per open, the path not existing to pin when this is armed.
#
# The HEAD of the stream is unrecoverable on this route: the banner is out before the host
# finishes enumerating, so a capture taken this way is read for its body.
arm_waiting_reader() {
  setsid bash -c '
    shopt -s nullglob
    while true; do
      m=($1)
      # AMBIGUITY IS REFUSED HERE TOO, not just in resolve_console. Two boards enumerating
      # this product string at once (a fleet pass with two _usbcdc boards, or a node from
      # the previous capture not yet gone) would otherwise be read in glob order and the
      # capture would be of whichever came first, silently and plausibly.
      if [ "${#m[@]}" -gt 1 ]; then
        printf "REFUSING: %d KickOS consoles match %s; which one is this board is a guess:\n" \
               "${#m[@]}" "$1" >&2
        printf "  %s\n" "${m[@]}" >&2
        exit 1
      fi
      if [ "${#m[@]}" -eq 1 ]; then
        stty -F "${m[0]}" 115200 raw -echo -hupcl clocal min 1 time 0 2>/dev/null
        cat "${m[0]}" 2>/dev/null
      fi
      sleep 0.05
    done' _ "$PATTERN" >> "$LOG" &
  READER=$!
}

# Answered from the LOG, the device having usually gone by now. dmesg_restrict is 1 here, so
# the kernel log cannot separate the two failures either.
check_cdc_capture() {
  if [ -s "$LOG" ]; then
    return 0
  fi
  refuse "the USB CDC console produced nothing (looked for $PATTERN).
  Either it never enumerated, or it enumerated and said nothing, and this bench cannot
  tell those apart. Check the image really links a _usbcdc service list."
}

if [ "${CONSOLE_USB_CDC:-0}" != "1" ]; then
  resolve_console
  echo "=== $BOARD  console $PORT  image $IMG"
else
  echo "=== $BOARD  console <its own USB CDC, after the flash>  image $IMG"
fi
: > "$LOG" || refuse "cannot write $LOG"

READER=""
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
}
stop_wrapped_reader() {
  [ -n "$READER" ] || return 0
  kill -- "-$READER" 2>/dev/null
  sleep 1
  # The holder check needs a pinned $PORT; a self-USB console has none, and is usually off
  # the bus by now.
  [ -n "$PORT" ] || return 0
  if fuser "$PORT" 2>/dev/null; then
    echo "WARNING: $PORT is STILL held after the reader teardown. Kill the PID fuser" >&2
    echo "  reports before the next capture, or it will split the bytes." >&2
  fi
}

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
  f302nucleo|f411disco)
    # One arm, two consoles. f302nucleo's is the ST-Link V2.1's own VCOM; f411disco's probe
    # is a V2 with NO VCP, so its console is a separate FTDI named by RIG_CONSOLE_F411DISCO
    # and the stty re-arm below is what keeps that cable from returning EOF at once.
    # THERE IS NO SEPARATE RESET STEP. Releasing NRST at the end of the write already starts
    # the image, so the write's own boot IS the authoritative run; a reset after it cuts that
    # boot off MID-LINE and starts a second one.
    #
    # The reader is a passive cat on the ST-Link's own VCOM and does not touch SWD, so arming
    # it FIRST is harmless here and is what captures the head of the banner. That is an
    # ST-Link property, not a general one: on a J-Link, arming before the flash yields an empty
    # log or one missing its head.
    #
    # THE WRITE GOES THROUGH tools/flash.sh, so the bench recipe and the hand recipe are one
    # command; tools/flash-stlink.sh carries why that command has no reset step.
    #
    # FLASH_TOOL IS PINNED, not left to the dispatcher: candidates_for() offers "stlink jlink"
    # for stm32f302 and takes the first on PATH, so on a host without stlink-tools this would
    # silently become a J-Link SWD flash, a different recipe reached by accident. Pinning it
    # keeps the missing-tool case a named refusal.
    command -v st-flash > /dev/null || refuse "st-flash not on PATH (apt install stlink-tools)"
    [ -e "$IMG.bin" ] || refuse "no $IMG.bin (st-flash loads the raw binary)"
    stty -F "$PORT" 115200 raw -echo -hupcl clocal min 1 time 0 || refuse "stty failed on $PORT"
    cat "$PORT" >> "$LOG" &
    READER=$!
    sleep 1
    check_reader "on arming"
    # CHECKED, and its output kept: a failed write leaves the PREVIOUS image running, which is
    # the failure that most looks like a pass.
    if ! WOUT=$(FLASH_TOOL=stlink FLASH_IMAGE="$IMG" "$ROOT/tools/flash.sh" "$BOARD" "$APP" 2>&1); then
      kill $READER 2>/dev/null
      printf '%s\n' "$WOUT" | tail -8 >&2
      refuse "the $BOARD write failed (tools/flash.sh -> flash-stlink.sh on $IMG.bin)"
    fi
    sleep "${CAP_SECS:-25}"
    note_reader
    kill $READER 2>/dev/null
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
      if ! POUT=$(FLASH_IMAGE="$IMG" "$ROOT/tools/flash-picotool.sh" "$BOARD" "$APP" 2>&1); then
        stop_wrapped_reader
        printf '%s\n' "$POUT" | tail -8 >&2
        refuse "picotool could not flash $BOARD. Already ran KickOS? Power-cycle it into BOOTSEL."
      fi
    else
      # picotool load -x reboots straight into the app, so the run is over before a reader armed
      # afterwards would exist. The console is a SEPARATE FTDI from the RP2 Boot interface, so
      # arming it first cannot disturb programming. Same FTDI re-arm as rx72m.
      stty -F "$PORT" 115200 raw -echo -hupcl clocal min 1 time 0 || refuse "stty failed on $PORT"
      arm_wrapped_reader "$PORT"
      sleep 1
      check_reader "on arming"
      if ! POUT=$(FLASH_IMAGE="$IMG" "$ROOT/tools/flash-picotool.sh" "$BOARD" "$APP" 2>&1); then
        kill $READER 2>/dev/null
        printf '%s\n' "$POUT" | tail -8 >&2
        refuse "picotool could not flash $BOARD. Already ran KickOS? Power-cycle it into BOOTSEL."
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
    done
    rm -f "$PROBE"
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
    ;;
esac

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
# is reported rather than added. The two-image boards run their images as separate invocations
# into separate logs, so one log normally owes exactly one plan line, and more than one means
# the board restarted inside the window.
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
#
# The label itself can arrive damaged: the f302nucleo VCOM drops bytes, and "commit a1220233"
# reached the log as "c a1220233". A lone 8-hex token in a KickOS banner IS the commit, so recover
# it rather than reporting no banner on a capture that carries one.
# `-dirty` is part of the label and MUST survive: without it a capture taken from a tree with
# uncommitted edits reports as if it were taken at the commit, and the witness is unfalsifiable.
#
# THE LABEL ALONE, in the form cmake/build_stamp.cmake stamps it (`git describe --dirty
# --always`), or empty. Split out from the line it is printed on because the bench verdict
# below compares it against the tree that built the image, and must run over a planted log as
# well as over this one.
banner_label() { # <log>
  _bl=$(grep -aoE 'commit +[0-9a-f]{7,}(-dirty)?' "$1" | tail -1)
  if [ -n "$_bl" ]; then
    printf '%s\n' "${_bl##* }"
    return 0
  fi
  # THE SUFFIX MUST BE RECOVERED WITH THE HASH, and its absence must not read as clean.
  # Damage is byte LOSS, so a banner that reached the log as "c 06ffd64f" may have been
  # "commit 06ffd64f-dirty" with the suffix eaten. Recovering a bare hash therefore says
  # UNVERIFIED rather than nothing.
  #
  # THE SEARCH IS HELD TO BANNER-BLOCK LINES, WHICH CARRY NO COLON. Eight hex digits is a
  # short enough shape to occur in the report proper, where `cycle counter: 84000000 Hz`
  # matches it, and a boot whose commit line was dropped WHOLE then recovers a rate and
  # reports a damaged label instead of an absent one. Every banner field is `name  value`; every app and
  # report line is `label: value`.
  _br=$(grep -av ':' "$1" \
    | grep -aoE '(^|[^0-9a-z])[0-9a-f]{8}(-dirty)?([^0-9a-z]|$)' \
    | grep -oE '[0-9a-f]{8}(-dirty)?' | tail -1)
  if [ -z "$_br" ]; then
    return 0
  fi
  case "$_br" in
    *-dirty) printf '%s\n' "$_br" ;;
    *) printf '%s-UNVERIFIED\n' "$_br" ;;
  esac
}

LABEL=$(banner_label "$LOG")
BANNER=""
case "$LABEL" in
  "") ;;
  *-UNVERIFIED) BANNER="commit $LABEL (banner damaged in transit; -dirty could not be confirmed)" ;;
  *)
    BANNER="commit $LABEL"
    # A label recovered with its `-dirty` intact is verified; it still arrived damaged and the
    # line says so, which the UNVERIFIED spelling above already carries for the other case.
    if ! grep -aqE 'commit +[0-9a-f]{7,}(-dirty)?' "$LOG"; then
      BANNER="$BANNER (banner damaged in transit)"
    fi
    ;;
esac

# --- what a bench capture owes ------------------------------------------------
#
# THE MARKERS ARE THE APP'S UNCONDITIONAL EMISSIONS ONLY (user/apps/common/bench/main.cc), and
# they fall into two classes because a boot prints one of some and one PER REPORT WINDOW of the
# others. The SWEPT distribution rows are in neither class: they sit behind a nonzero sample
# count and a board with no cycle counter prints none of them, so demanding one would refuse
# exactly the boards whose wall-clock figures are the point.
#
# THE ONCE-PER-BOOT SET IS ALSO WHAT PROVES THE SLICE. Each of these is printed exactly once by
# a boot that runs to the end, so a slice holding two of any of them spans a reboot the banner
# did not reveal, and a slice holding none of one is a boot that never reached that section.
bench_boot_markers() {
  printf '%s\n' \
    'microbenchmark: context-switch' \
    'cycle counter: ' \
    'phase table (' \
    'bench: done'
}

# One per report window, so they are counted per window below and owed at least once here.
bench_window_markers() {
  printf '%s\n' \
    '  throughput: ' \
    '  switch:   '
}

# The line the LAST boot begins at. The banner BLOCK is the first thing an image prints, and its
# title line carries no commit, so a console that drops the commit line whole still leaves an
# anchor the label cannot reach. Where the title went too the commit line stands in, and the
# one-per-boot counts inside the slice are what then refuse a slice spanning two boots.
bench_boot_start() { # <log>
  _bs_t=$(grep -anE 'KickOS +[0-9]+\.' "$1" | tail -1 | cut -d: -f1)
  _bs_c=$(grep -anE 'commit +[0-9a-f]{7,}(-dirty)?' "$1" | tail -1 | cut -d: -f1)
  if [ -z "$_bs_c" ]; then
    # The same banner-block restriction banner_label recovers under, and it has to be the same
    # one: a boot start found on a line that label search will not read leaves the slice
    # beginning after its own banner.
    _bs_c=$(grep -anv ':' "$1" \
      | grep -aE '(^|[^0-9a-z])[0-9a-f]{8}(-dirty)?([^0-9a-z]|$)' | tail -1 | cut -d: -f1)
  fi
  if [ -z "$_bs_t" ]; then
    printf '%s\n' "$_bs_c"
    return 0
  fi
  if [ -z "$_bs_c" ]; then
    printf '%s\n' "$_bs_t"
    return 0
  fi
  if [ "$_bs_c" -gt "$_bs_t" ]; then
    printf '%s\n' "$_bs_c"
    return 0
  fi
  printf '%s\n' "$_bs_t"
}

# <log> <slice out>. Cuts the log to its last boot. Nonzero, and an empty slice, where no boot
# start is recognisable at all.
bench_slice() { # <log> <out>
  _bsl_from=$(bench_boot_start "$1")
  if [ -z "$_bsl_from" ]; then
    : > "$2"
    return 1
  fi
  sed -n "${_bsl_from},\$p" "$1" > "$2"
  return 0
}

# <slice>. Every per-window clause, over EVERY window in the slice. A window
# opens at the row each report begins with and runs to the next one or to the end.
#
# THE CLAUSES ARE PER WINDOW BECAUSE THE REPORT IS. Read globally, one accounted window covers
# every later window that dropped its accounting, and a sweep short by the passes nobody
# counted reads as a full one whichever window it was.
#
# THE SWITCH ROW IS THE CONDITION THE CLAUSES BELOW READ, SO A WINDOW OWES EXACTLY ONE. The
# distribution print writes it whatever it sampled, so a window carrying none lost a whole line
# and answers for nothing: every clause guarded on the sweep having run is then guarded on a
# sample count of zero. Two of them is the same defect from the other side, nothing saying which
# count the end-to-end block below was conditioned on.
#
# `e2e-passes` IS A COMPLETENESS MARKER AND NOT A FIGURE, AND IT IS OWED CONDITIONALLY. The app
# skips the end-to-end block whole where the distribution print hands back a switch count of
# zero, so the line is owed exactly where THIS window's switch row sampled, which is the
# condition the app reads. `closed` and `dropped` count spans that opened; a pass whose waiter
# never parked opens none, so neither counter moves and the rows report the passes that did run
# as though they were all of them.
#
# THE WHOLE END-TO-END BODY IS OWED UNDER THAT ONE CONDITION, NOT THE ACCOUNTING LINE ALONE.
# The block is one print: probe, accounting, then a row per population that sampled. So a
# window that owes it owes exactly one probe and, where that probe closed spans, a row to carry
# them.
#
# AN UNPARSEABLE ACCOUNTING LINE IS NOT AN ABSENT ONE AND MAY NOT RESOLVE TO SATISFIED: a line
# whose fields the reader cannot read decides nothing, and skipping it silently is the same
# hole as not requiring it.
#
# No `$` anchor and no reliance on the field after the last one read: every line here ends CRLF.
bench_windows() { # <slice>
  awk '
      # The CR goes before any field is read. Every console line here ends CRLF, so the last
      # field on a line carries one and a numeric match on it fails against a value that is
      # there. Nothing below distinguishes the two line endings, which is what the rest of
      # this file keeps them for.
      { gsub(/\r/, "") }
      function fail(msg) { printf "  window %d: %s\n", win, msg; rc = 1 }
      function num(s) { sub(/^[a-z]+=/, "", s); return s + 0 }
      function close_window(   tot) {
          if (win == 0) { return }
          if (nsw == 0) {
              fail("it carries no [  switch: ] row, so its sample count reads zero and " \
                   "every clause conditioned on the sweep having run is decided by a row " \
                   "this window never printed")
          }
          if (nsw > 1) {
              fail("it carries " nsw " switch rows, so nothing says which of them the " \
                   "end-to-end block below was conditioned on")
          }
          if (swn > 0 && npass == 0) {
              fail("its switch row sampled " swn " and it carries no " \
                   "[  e2e-passes: asked=] line, so its end-to-end rows " \
                   "answer to no denominator")
          }
          if (npass > 1) {
              fail("it carries " npass " accounting lines, so nothing says " \
                   "which sweep the rows below belong to")
          }
          if (npass == 1 && parsed == 0) {
              fail("its accounting line does not read as asked= and raised= " \
                   "counts, so the denominator is present and undecidable")
          }
          if (npass == 1 && parsed == 1 && raised < asked) {
              fail("asked=" asked " raised=" raised ". The sweep abandoned " \
                   (asked - raised) " pass(es) whose waiter never published a " \
                   "park, and an abandoned pass opens no span, so its end-to-end " \
                   "rows are a sweep of " raised " reported as one of " asked)
          }
          if (npass == 1 && parsed == 1 && raised > asked) {
              fail("asked=" asked " raised=" raised ". The kernel accepted " \
                   "more raises than the app ran passes, so one of the two " \
                   "counts is not counting what it names")
          }
          if (swn > 0 && nprobe == 0) {
              fail("its switch row sampled " swn " and it carries no " \
                   "[  e2e-probe: ] line, so the end-to-end block it owes is " \
                   "gone whole and what accounting it kept answers for rows " \
                   "this window never printed")
          }
          if (nprobe > 1) {
              fail("it carries " nprobe " probe lines, so nothing says which " \
                   "of them the end-to-end rows below were closed by")
          }
          if (npass == 1 && parsed == 1 && nprobe == 1 && closed != raised) {
              fail("raised=" raised " and the probe closed " closed ". A raise " \
                   "that opened a span and did not close it is in neither row")
          }
          if (nprobe == 1 && dropped > 0) {
              fail("the probe dropped " dropped " sample(s), which opened a " \
                   "span and entered no row")
          }
          tot = nloc + ncross
          if (nprobe == 1 && loc_rows > 0 && tot != closed) {
              fail("its locality rows total " tot " against " closed " closed, " \
                   "so the two populations are not the spans the sweep reports")
          }
          if (nprobe == 1 && closed > 0 && loc_rows == 0) {
              fail("the probe closed " closed " span(s) and the window carries " \
                   "no e2e-local or e2e-cross row, so every span it reports " \
                   "closing is in no population at all")
          }
      }
      /^  throughput: / {
          close_window()
          win++
          swn = 0; nsw = 0; npass = 0; nprobe = 0; parsed = 0
          asked = 0; raised = 0; closed = 0; dropped = 0
          nloc = 0; ncross = 0; loc_rows = 0
          next
      }
      /^  switch: / {
          nsw++
          if (match($0, /n=[0-9]+\)/)) { swn = substr($0, RSTART + 2, RLENGTH - 3) + 0 }
          next
      }
      /^  e2e-probe: / {
          nprobe++
          for (i = 1; i <= NF; i++) {
              if ($i ~ /^closed=[0-9]+$/) { closed = num($i) }
              if ($i ~ /^dropped=[0-9]+$/) { dropped = num($i) }
          }
          next
      }
      /^  e2e-passes: / {
          npass++
          seen_a = 0; seen_r = 0
          for (i = 1; i <= NF; i++) {
              if ($i ~ /^asked=[0-9]+$/) { asked = num($i); seen_a = 1 }
              if ($i ~ /^raised=[0-9]+$/) { raised = num($i); seen_r = 1 }
          }
          if (seen_a == 1 && seen_r == 1) { parsed = 1 }
          else { parsed = 0 }
          next
      }
      /^  e2e-local: / {
          loc_rows++
          if (match($0, /n=[0-9]+\)/)) { nloc = substr($0, RSTART + 2, RLENGTH - 3) + 0 }
          next
      }
      /^  e2e-cross: / {
          loc_rows++
          if (match($0, /n=[0-9]+\)/)) { ncross = substr($0, RSTART + 2, RLENGTH - 3) + 0 }
          next
      }
      END { close_window(); exit rc }' "$1"
}

# <log> <expected label, empty for none> <slice out>. Every finding is printed; 0 means the file
# is a complete bench report from the expected tree. The slice is written to the third argument
# and left there, because the cycle judgement downstream reads the same one boot.
#
# THE SLICE IS CUT ONCE AND EVERY ARM READS IT, the cycle judgement included. A board that
# reboots inside the capture window leaves a complete run followed by a cut one at the same
# commit; read across the two, the banner comes from one boot while the markers, the phase
# table, the report windows and the cycle rows are borrowed from the other, and a run that
# never finished reports as a witness.
#
# `bench: done` IS THE TRUNCATION TEST: it is the last line main writes and the reporter loop is
# bounded, so its absence says the console window closed before the app finished. A truncated
# capture holds every earlier marker and reads as complete on any of them.
bench_verdict() { # <log> <want> <slice out>
  _bv_whole=$1
  _bv_want=$2
  _bv_log=$3
  _bv_rc=0

  if ! bench_slice "$_bv_whole" "$_bv_log"; then
    echo "  no boot start: the capture carries neither a banner title nor a commit label, so" >&2
    echo "    nothing says where the run being read began and no arm below has a boot to read" >&2
    return 1
  fi

  # THE SLICE MUST HOLD ONE BOOT AND THE COUNTS ARE WHAT SAY SO. Zero is a boot that never
  # reached that section; more than one is a boot boundary the anchor did not find, and reading
  # across it is the whole defect the slice exists to stop.
  while read -r _bv_m; do
    _bv_c=$(grep -acF -e "$_bv_m" "$_bv_log")
    if [ "$_bv_c" -eq 0 ]; then
      echo "  no [$_bv_m] line: the report is missing a section it always prints" >&2
      _bv_rc=1
    elif [ "$_bv_c" -gt 1 ]; then
      echo "  $_bv_c [$_bv_m] lines in one boot: the capture holds a reboot whose banner this" >&2
      echo "    chain could not find, so the arms below read across two runs" >&2
      _bv_rc=1
    fi
  done <<EOF
$(bench_boot_markers)
EOF

  while read -r _bv_m; do
    if ! grep -aqF -e "$_bv_m" "$_bv_log"; then
      echo "  no [$_bv_m] line: the report is missing a section it always prints" >&2
      _bv_rc=1
    fi
  done <<EOF
$(bench_window_markers)
EOF

  # DIRTY IS NOT THE REFUSAL, DISAGREEING IS. A capture taken from a modified tree is a normal
  # thing to take and says so; what may not happen is a banner that does not match the tree
  # that built the image, and the `-dirty` suffix is part of that label. So the arm is one
  # equality over the whole label, and the suffix rides in it. The label is read out of the
  # SLICE, so a last boot whose own banner is gone has none rather than the previous boot's.
  _bv_got=$(banner_label "$_bv_log")
  if [ -z "$_bv_got" ]; then
    echo "  no banner in the last boot: the run being read carries no commit of its own, so it" >&2
    echo "    belongs to no tree and an earlier boot's label may not stand in for it" >&2
    _bv_rc=1
  else
    case "$_bv_got" in
      *-UNVERIFIED)
        # The recovery path could not tell a clean label from a truncated `-dirty` one, so the
        # equality below cannot be decided either way and passing it would decide it wrongly.
        echo "  banner [$_bv_got]: the label arrived damaged and the -dirty suffix could not be" >&2
        echo "    read off it, so a capture taken from a modified tree cannot be told from one" >&2
        echo "    taken at the commit" >&2
        _bv_rc=1
        ;;
      *)
        if [ "$_bv_got" != "$_bv_want" ]; then
          echo "  banner [$_bv_got] against [$_bv_want] built here: the image on the board is" >&2
          echo "    not the one this tree built, so the figures below measure another tree" >&2
          _bv_rc=1
        fi
        ;;
    esac
  fi

  _bv_win=$(bench_windows "$_bv_log")
  if [ -n "$_bv_win" ]; then
    printf '%s\n' "$_bv_win" >&2
    _bv_rc=1
  fi

  # THE SECOND TRUNCATION TEST, and it is independent of the sentinel: the table header
  # declares its own row count and the console refuses a line it cannot take whole, so a table
  # that lost rows to a busy console still ends with every line after it. A report whose header
  # declares no count comes from an instrument that cannot say, which is the same silence.
  #
  # No `$` anchor and no field past the first: every line here ends CR+LF.
  _bv_rows=$(awk '
      /phase table \(/ && want == 0 {
          if (match($0, /\([0-9]+ rows/)) { want = substr($0, RSTART + 1, RLENGTH - 6) + 0 }
          else { want = -1 }
          intable = 1
          next
      }
      intable == 1 && /^    [A-Z]/ { got++; next }
      intable == 1 && got > 0 { intable = 2 }
      END { printf "%d %d\n", want + 0, got + 0 }' "$_bv_log")
  read -r _bv_want_rows _bv_got_rows <<EOF
$_bv_rows
EOF
  if [ "$_bv_want_rows" -lt 0 ]; then
    echo "  the phase table header declares no row count, so a table that lost rows to a busy" >&2
    echo "    console reads as a shorter build" >&2
    _bv_rc=1
  elif [ "$_bv_want_rows" -ne "$_bv_got_rows" ]; then
    echo "  the phase table declares $_bv_want_rows rows and printed $_bv_got_rows: the console" >&2
    echo "    dropped what it could not take whole, and every figure read out of that table is" >&2
    echo "    a subset" >&2
    _bv_rc=1
  fi

  return $_bv_rc
}
# THE CYCLE ROWS ARE JUDGED HERE BECAUSE NO EMULATOR GATE REACHES THEM. The dead-counter
# refusal (tests/integration/check_bench_cyccnt.sh) registers through kickos_add_qemu_test,
# which registers nothing for a board that has no emulator, and every board this script
# flashes is one.
#
# NO BOARD IS NAMED HERE; the report carries every fact this needs. A board that converts no
# reading prints `cycle counter: 0 Hz` and says so, and its zeros settle nothing. A board that
# NAMES a rate and then reports a span of zero cycles over samples it counted has contradicted
# itself. A part whose counter is known to glitch is not exempt: declaring that keeps MIN as
# its headline, which says which statistic to trust, not that zero is a reading.
#
# This refuses the CAPTURE, never the board. The log is written and printed above and the
# caller fetches it past a refusal, so the wall-clock figures stand and stay readable: the
# round-trip and throughput lines, and the end-to-end nanoseconds. What does not survive is
# the capture reading as clean.
#
# A MISSING RATE LINE IS NOT A BOARD WITH NO COUNTER. The bench prints the line unconditionally
# and prints 0 where nothing converts a reading, so `0 Hz` is a declaration and its zeros are
# stated and left unjudged. NO line at all is a report that never reached the point of saying,
# and everything below it then runs on a log that was never checked to hold cycle rows in the
# first place.
#
# THE LIVENESS IS JUDGED PER REPORT WINDOW, for the same reason the completeness clauses are.
# Summed over a boot, one window whose rows spanned answers for every window after it, so a
# counter that stopped partway through the run reports as a live one. Every cycle row a boot
# prints lies inside a window: the phase table, the ns-probe and the sat-probe come ahead of the
# first one and none of them carries a `lo/avg/max cyc` row, so the group before window 1 is
# empty on a bench report and is judged only for a log that opens no window at all.
#
# The `cyc:` line stays a reading of the whole boot. It is what the capture reports rather than
# what it decides, and the refusal below names the window.
#
# <log>. 0 where the cycle rows of that log are a live counter's. Prints its own reading either
# way, and prints the refusal to stderr. The caller hands it ONE BOOT: read over a whole file a
# first boot whose rows moved answers for a last boot whose counter had stopped.
bc_close_window() {
  if [ "$_bc_wsampled" -gt 0 ] && [ "$_bc_wmoving" -eq 0 ] && [ "$_bc_frozen" -lt 0 ]; then
    _bc_frozen=$_bc_win
    _bc_frozenn=$_bc_wsampled
  fi
  _bc_wsampled=0
  _bc_wmoving=0
}
bench_cycles() { # <log>
  _bc_log=$1
  _bc_hz=$(sed -n 's|^ *cycle counter: \([0-9]\{1,\}\) Hz.*|\1|p' "$_bc_log" | tail -1)
  if [ -z "$_bc_hz" ]; then
    return 0
  fi
  # Every cycle row in the log as "lo max n label", with the row each report window opens with
  # marked so the rows can be grouped by window. The label goes LAST because the per-core rows
  # carry a space inside theirs and read would split it across the fields. A row's first field
  # is always a number, so the marker cannot be one.
  # NO `$` ANCHOR, for the reason given at the head of this file.
  _bc_rows=$(sed -n \
    -e 's|^  throughput: .*|@window|p' \
    -e 's|^ *\(.\{1,\}\) \{1,\}\([0-9]\{1,\}\)/[0-9]\{1,\}/\([0-9]\{1,\}\) cyc .*n=\([0-9]\{1,\}\)).*|\2 \3 \4 \1|p' \
    "$_bc_log")
  _bc_sampled=0
  _bc_dead=0
  _bc_moving=0
  _bc_deadrows=""
  _bc_win=0
  _bc_wsampled=0
  _bc_wmoving=0
  _bc_frozen=-1
  _bc_frozenn=0
  while read -r _bc_lo _bc_hi _bc_n _bc_label; do
    if [ "${_bc_lo:-}" = "@window" ]; then
      bc_close_window
      _bc_win=$((_bc_win + 1))
      continue
    fi
    if [ -z "${_bc_n:-}" ]; then
      continue
    fi
    if [ "$_bc_n" -eq 0 ]; then
      continue
    fi
    _bc_sampled=$((_bc_sampled + 1))
    _bc_wsampled=$((_bc_wsampled + 1))
    if [ "$_bc_hi" -eq 0 ]; then
      _bc_dead=$((_bc_dead + 1))
      case "$_bc_deadrows" in
        *"  $_bc_label "*) ;;
        *) _bc_deadrows="$_bc_deadrows  $_bc_label n=$_bc_n
" ;;
      esac
    fi
    if [ "$_bc_hi" -gt "$_bc_lo" ]; then
      _bc_moving=$((_bc_moving + 1))
      _bc_wmoving=$((_bc_wmoving + 1))
    fi
  done <<EOF
$_bc_rows
EOF
  bc_close_window

  if [ "$_bc_hz" -eq 0 ]; then
    printf 'cyc:    no rate declared, so %s sampled row(s) are cycles only and unjudged\n' \
           "$_bc_sampled"
    return 0
  fi
  if [ "$_bc_sampled" -eq 0 ]; then
    printf 'cyc:    %s Hz declared, no sampled row: the report carries no cycle reading\n' "$_bc_hz"
    return 0
  fi
  printf 'cyc:    %s Hz declared, %s sampled row(s), %s dead, %s moving\n' \
         "$_bc_hz" "$_bc_sampled" "$_bc_dead" "$_bc_moving"
  if [ "$_bc_dead" -gt 0 ]; then
    printf 'REFUSING: %s names a rate of %s Hz and then reports a span of ZERO cycles over\n' \
           "$LOG" "$_bc_hz" >&2
    echo "  samples it counted. A counter that reads a constant is absent, frozen, or" >&2
    echo "  unpowered; the rows below are cycles the run did not measure:" >&2
    printf '%s' "$_bc_deadrows" >&2
    echo "  The wall-clock figures in this log are taken from a different counter and stand." >&2
    return 1
  fi
  if [ "$_bc_frozen" -ge 0 ]; then
    _bc_where="in report window $_bc_frozen"
    if [ "$_bc_frozen" -eq 0 ]; then
      _bc_where="ahead of its first report window"
    fi
    printf 'REFUSING: %s names a rate of %s Hz and not one of the %s sampled row(s) %s\n' \
           "$LOG" "$_bc_hz" "$_bc_frozenn" "$_bc_where" >&2
    echo "  spans more than a single value. A live counter does not read one constant across a" >&2
    echo "  whole distribution: this one is frozen." >&2
    echo "  The wall-clock figures in this log are taken from a different counter and stand." >&2
    return 1
  fi
  return 0
}

# Same damage, same recovery: the posture line reaches the f302nucleo log as "m off".
# NO `$` ANCHOR. Every console line here ends CRLF, and GNU grep counts the CR as part of the
# line while this box's grep (ugrep) does not, so an anchored pattern passes a local test and
# fails on the bench host against byte-identical input.
MPU=$(grep -aoE 'mpu +(enforce|off)' "$LOG" | tail -1)
if [ -z "$MPU" ]; then
  MPU=$(grep -aoE '^m (enforce|off)' "$LOG" | tail -1)
fi

printf 'bytes:  %s\n' "$BYTES"
printf 'runs:   %s\n' "$RUNS"
printf 'ok:     %s\n' "$OKC"
printf 'not ok: %s\n' "$NOTOKC"
printf 'plan:   %s\n' "$(grep -aoE '^1\.\.[0-9]+' "$RUN" | tail -1)"
printf 'skip:   %s\n' "$(grep -acE '# SKIP' "$RUN")"
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
#
# A BENCH APP OWES A BENCH REPORT. The two expectations are separate: `bench` announces no
# plan AND owes every marker below.
case $APP in
  selftest*) WANT_TAP=1; WANT_BENCH=0 ;;
  bench*)    WANT_TAP=0; WANT_BENCH=1 ;;
  *)         WANT_TAP=0; WANT_BENCH=0 ;;
esac
if [ -z "$LAST" ]; then
  if [ "$WANT_TAP" -eq 1 ] && [ "${CONSOLE_USB_CDC:-0}" = "1" ]; then
    # A console that IS the device cannot deliver its own head, so the plan line is gone
    # and demanding one refuses every capture taken this way. The verdict falls back to the
    # ok count alone; reconcile the arm total by eye against the count
    # user/apps/common/selftest/CMakeLists.txt hands the gates.
    if [ "$OKC" -eq 0 ]; then
      say_kickos_lines
      refuse "$LOG carries no plan line AND no ok lines: nothing of the suite arrived"
    fi
    echo "NOTE: no plan line; a USB CDC console loses the head of every capture, this one" >&2
    echo "  included. $OKC ok line(s) and the arms below the first one are NOT accounted for;" >&2
    echo "  derive the expected count and check it by hand." >&2
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

if [ "$WANT_BENCH" -eq 1 ]; then
  # THE TREE THAT BUILT THE IMAGE, and it is not necessarily the tree this script is running
  # from: bench.sh builds where the toolchains are and ships the image, so it passes the label
  # it stamped. The local fallback is for a board on this box. A bench capture with NEITHER is
  # refused rather than judged on its own say-so: a banner nothing compares against says only
  # that some tree printed it.
  if [ -z "${EXPECT_COMMIT:-}" ]; then
    EXPECT_COMMIT=$(git -C "$ROOT" describe --dirty --always 2>/dev/null || true)
  fi
  [ -n "$EXPECT_COMMIT" ] || refuse "no EXPECT_COMMIT and $ROOT is not a git tree, so the
  banner in $LOG can be read but not checked against anything. Pass the label
  cmake/build_stamp.cmake stamped into the image."

  # THE VERDICT IS PROVEN BEFORE IT IS TRUSTED. Each plant below is a complete report that
  # differs from the one above it in one way, and a check that does not fire on its own plant
  # reports every real capture clean. CRLF throughout, because that is what a console under
  # KICKOS_CONSOLE_CRLF writes and a pattern anchored against it is the failure this file's
  # header exists to prevent.
  CTL=$(mktemp -d) || refuse "cannot create a directory for the verdict's own controls"
  # The fourth argument is the commit row's own prefix, which the damaged plant eats into.
  ctl_head() { # <file> <banner label> <declared cycle rate> [commit row prefix]
    {
      printf '  ==============================================\r\n'
      printf '   KickOS 0.5.1  -  microkernel RTOS\r\n'
      printf '  ==============================================\r\n'
      printf '   board   control\r\n'
      printf '%s%s\r\n' "${4:-   commit  }" "$2"
      printf 'microbenchmark: context-switch throughput (all arches) + per-switch cost\r\n'
      printf 'cycle counter: %s Hz (0 = no rate converts a reading; cycles only)\r\n' "$3"
      printf '  phase table (2 rows; cyc avg/max, min last and a floor):\r\n'
      printf '    NULL             1/1  min=1  n=220000\r\n'
      printf '    ARCH_SWITCH      22/23  min=21  n=480039\r\n'
    } > "$1"
  }
  # One report window, which is what the app prints once per report: the row it opens with, the
  # switch row, then the end-to-end block and its accounting.
  ctl_window() { # <file> <asked> <raised>
    {
      printf '  throughput: 25086 ctx-sw/s  (39861 ns/sw avg over 40000 switches / 1594 ms)\r\n'
      printf '  switch:    80/80/229 cyc  952/952/2726 ns  (p50/p99/max, n=40002)\r\n'
      printf '  e2e-probe: line=7 closed=%s dropped=0 tare=4000/4122 ns  (min/avg, n=64)\r\n' "$3"
      printf '  e2e-passes: asked=%s raised=%s\r\n' "$2" "$3"
      printf '  e2e-local: 28672/30720/31857 ns  (p50/p99/max, n=%s)\r\n' "$3"
    } >> "$1"
  }
  # The same window with a switch row that reads one value, which is a counter that stopped.
  ctl_frozen_window() { # <file>
    {
      printf '  throughput: 25086 ctx-sw/s  (39861 ns/sw avg over 40000 switches / 1594 ms)\r\n'
      printf '  switch:    80/80/80 cyc  952/952/2726 ns  (p50/p99/max, n=40002)\r\n'
      printf '  e2e-probe: line=7 closed=50 dropped=0 tare=4000/4122 ns  (min/avg, n=64)\r\n'
      printf '  e2e-passes: asked=50 raised=50\r\n'
      printf '  e2e-local: 28672/30720/31857 ns  (p50/p99/max, n=50)\r\n'
    } >> "$1"
  }
  ctl_report() { # <file> <banner label>
    ctl_head "$1" "$2" 84000000
    ctl_window "$1" 50 50
  }
  # THE DIRTY PLANT FLIPS THE SUFFIX rather than adding one: the arm is an equality over the
  # whole label, so the plant that tests it must disagree with the tree whichever state the
  # tree is in. A plant that merely appended `-dirty` would BE the expected label on a dirty
  # tree and would pass for the right reason, proving nothing.
  _flip="${EXPECT_COMMIT%-dirty}"
  if [ "$_flip" = "$EXPECT_COMMIT" ]; then
    _flip="$EXPECT_COMMIT-dirty"
  fi
  ctl_report "$CTL/good" "$EXPECT_COMMIT"
  printf 'bench: done\r\n' >> "$CTL/good"
  # TWO WINDOWS, BOTH ACCOUNTED, which is the shape every real capture of this campaign has and
  # the one the per-window clauses must not refuse.
  ctl_report "$CTL/twowin" "$EXPECT_COMMIT"
  ctl_window "$CTL/twowin" 50 50
  printf 'bench: done\r\n' >> "$CTL/twowin"
  ctl_report "$CTL/cut" "$EXPECT_COMMIT"
  ctl_report "$CTL/flip" "$_flip"
  printf 'bench: done\r\n' >> "$CTL/flip"
  ctl_report "$CTL/other" "0000000f"
  printf 'bench: done\r\n' >> "$CTL/other"
  # The banner as the f302nucleo VCOM delivers it: the word eaten, a bare hash left. Built from
  # the CLEAN label, since a damaged label that still carries `-dirty` is a label the recovery
  # can verify; what it cannot verify is a bare hash, and that is the case this plants.
  ctl_head "$CTL/damaged" "${EXPECT_COMMIT%-dirty}" 84000000 "   c "
  ctl_window "$CTL/damaged" 50 50
  printf 'bench: done\r\n' >> "$CTL/damaged"
  # A table the console could not take whole: every line after it still arrives, so the
  # sentinel is there and only the header's own count says anything is missing.
  sed '/^    ARCH_SWITCH/d' "$CTL/good" > "$CTL/dropped"
  # THE ACCOUNTING GONE WHERE THE SWEEP RAN: a complete report at the right commit, its switch
  # row sampling 40002, whose end-to-end rows answer to no denominator.
  sed '/^  e2e-passes:/d' "$CTL/good" > "$CTL/nopasses"
  # THE SECOND WINDOW ALONE LOSING ITS ACCOUNTING. The first window is accounted in full, so a
  # rule asking the slice for one such line anywhere is satisfied by it while the second
  # window's rows answer to nothing.
  ctl_report "$CTL/winlost" "$EXPECT_COMMIT"
  {
    printf '  throughput: 25086 ctx-sw/s  (39861 ns/sw avg over 40000 switches / 1594 ms)\r\n'
    printf '  switch:    80/80/229 cyc  952/952/2726 ns  (p50/p99/max, n=40002)\r\n'
    printf '  e2e-probe: line=7 closed=50 dropped=0 tare=4000/4122 ns  (min/avg, n=64)\r\n'
    printf '  e2e-local: 28672/30720/31857 ns  (p50/p99/max, n=50)\r\n'
    printf 'bench: done\r\n'
  } >> "$CTL/winlost"
  # THE SECOND WINDOW'S END-TO-END BLOCK GONE WHOLE, its accounting line kept and valid. The
  # first window carries a probe and a row, so a rule asking the slice for a probe anywhere is
  # satisfied by it while the second window accounts for rows nobody printed.
  ctl_report "$CTL/winbody" "$EXPECT_COMMIT"
  {
    printf '  throughput: 25086 ctx-sw/s  (39861 ns/sw avg over 40000 switches / 1594 ms)\r\n'
    printf '  switch:    80/80/229 cyc  952/952/2726 ns  (p50/p99/max, n=40002)\r\n'
    printf '  e2e-passes: asked=50 raised=50\r\n'
    printf 'bench: done\r\n'
  } >> "$CTL/winbody"
  # THREE ACCOUNTED WINDOWS, which is the shape every capture of this campaign has, so the
  # per-window clauses must refuse none of them.
  ctl_report "$CTL/threewin" "$EXPECT_COMMIT"
  ctl_window "$CTL/threewin" 50 50
  ctl_window "$CTL/threewin" 50 50
  printf 'bench: done\r\n' >> "$CTL/threewin"
  # THE SECOND WINDOW OF THREE LOSING ITS SWITCH ROW, the whole line a console drops. Its
  # sample count then reads zero, so every clause conditioned on the sweep having run is skipped
  # and the window owes nothing, while the first and third satisfy any rule asking the slice for
  # a switch row anywhere.
  ctl_head "$CTL/winnosw" "$EXPECT_COMMIT" 84000000
  ctl_window "$CTL/winnosw" 50 50
  {
    printf '  throughput: 25086 ctx-sw/s  (39861 ns/sw avg over 40000 switches / 1594 ms)\r\n'
    printf '  e2e-probe: line=7 closed=50 dropped=0 tare=4000/4122 ns  (min/avg, n=64)\r\n'
    printf '  e2e-passes: asked=50 raised=50\r\n'
    printf '  e2e-local: 28672/30720/31857 ns  (p50/p99/max, n=50)\r\n'
  } >> "$CTL/winnosw"
  ctl_window "$CTL/winnosw" 50 50
  printf 'bench: done\r\n' >> "$CTL/winnosw"
  # ONE BOOT WHOSE COUNTER STOPPED BETWEEN WINDOWS: the first window's rows span and the two
  # after it read one constant over samples they counted. Summed over the boot the first
  # window's spans answer for them and a counter that stopped mid-run reports as a live one.
  # The report is complete, so the counter is the only finding.
  ctl_head "$CTL/winfrozen" "$EXPECT_COMMIT" 84000000
  ctl_window "$CTL/winfrozen" 50 50
  ctl_frozen_window "$CTL/winfrozen"
  ctl_frozen_window "$CTL/winfrozen"
  printf 'bench: done\r\n' >> "$CTL/winfrozen"
  # TWO PROBES IN ONE WINDOW. Each reports its own closes, and the row beneath them belongs to
  # one of the two with nothing saying which.
  ctl_head "$CTL/twoprobe" "$EXPECT_COMMIT" 84000000
  {
    printf '  throughput: 25086 ctx-sw/s  (39861 ns/sw avg over 40000 switches / 1594 ms)\r\n'
    printf '  switch:    80/80/229 cyc  952/952/2726 ns  (p50/p99/max, n=40002)\r\n'
    printf '  e2e-probe: line=7 closed=50 dropped=0 tare=4000/4122 ns  (min/avg, n=64)\r\n'
    printf '  e2e-probe: line=7 closed=50 dropped=0 tare=4000/4122 ns  (min/avg, n=64)\r\n'
    printf '  e2e-passes: asked=50 raised=50\r\n'
    printf '  e2e-local: 28672/30720/31857 ns  (p50/p99/max, n=50)\r\n'
    printf 'bench: done\r\n'
  } >> "$CTL/twoprobe"
  # THE LOCALITY ROWS GONE AND THE PROBE KEPT: 50 spans closed and no row carries any of them,
  # which the total arm skips because it has no row to total.
  sed '/^  e2e-local:/d' "$CTL/good" > "$CTL/noloc"
  # THE OTHER SIDE OF THAT CONDITION: the switch bracket sampled nothing, so the app skipped
  # the end-to-end block whole and the line it would have printed is absent by design. A
  # complete report, and the one the conditional arm exists to stop refusing.
  ctl_head "$CTL/nocyc" "$EXPECT_COMMIT" 0
  {
    printf '  throughput: 25086 ctx-sw/s  (39861 ns/sw avg over 40000 switches / 1594 ms)\r\n'
    printf '  switch:    0/0/0 cyc  (p50/p99/max, n=0)\r\n'
    printf 'bench: done\r\n'
  } >> "$CTL/nocyc"
  # THE CONDITION ITSELF UNREADABLE: with no switch row nothing says whether the sweep ran, and
  # a requirement that cannot be decided may not resolve to satisfied.
  sed '/^  switch: /d' "$CTL/good" > "$CTL/noswitch"
  # THE ACCOUNTING PRESENT AND SHORT: the sweep ran 50 passes and the kernel let 37 through, so
  # the rows below carry 37 and read as a full sweep.
  sed 's|^  e2e-passes: asked=50 raised=50|  e2e-passes: asked=50 raised=37|' "$CTL/good" \
    > "$CTL/shortsweep"
  # THE ACCOUNTING PRESENT AND LONG: more raises accepted than passes run, so one of the two
  # counts is not counting what it names and the arm may not be an inequality one way.
  sed 's|^  e2e-passes: asked=50 raised=50|  e2e-passes: asked=50 raised=64|' "$CTL/good" \
    > "$CTL/longsweep"
  # THE ACCOUNTING PRESENT AND UNREADABLE, which a reader that only matches well-formed fields
  # skips in silence, leaving the requirement satisfied by a line that decides nothing.
  sed 's|^  e2e-passes: asked=50 raised=50|  e2e-passes: asked=50 raised=|' "$CTL/good" \
    > "$CTL/malformed"
  # THE LOCALITY ROWS NOT THE SPANS THE PROBE CLOSED: 50 closed, and a row carrying 37.
  sed 's|(p50/p99/max, n=50)|(p50/p99/max, n=37)|' "$CTL/good" > "$CTL/locshort"
  # THE BANNER BLOCK TITLE DROPPED AND THE COMMIT LINE SURVIVING, the other whole line a
  # console can lose out of that block. Nothing then matches the title the boot start is
  # normally cut at and the commit line stands in; a boot start coming back empty here would
  # refuse a complete capture outright.
  grep -av 'KickOS' "$CTL/good" > "$CTL/notitle"
  # TWO BOOTS IN ONE WINDOW, at the same commit, the first complete and the second not. Every
  # marker the second boot never printed is in the first, and so is a full phase table, so a
  # verdict reading the whole file borrows them and calls a run that never finished a witness.
  cat "$CTL/good" "$CTL/cut" > "$CTL/reboot"
  cat "$CTL/good" "$CTL/dropped" > "$CTL/rebootdropped"
  # THE SECOND BOOT'S BANNER DROPPED WHOLE, which is what a console that emits whole lines or
  # nothing does to it. The boot start anchors on the banner block TITLE rather than the label,
  # so the slice does begin at the second boot and what this plants is the label then missing
  # from it, the first boot's being the only one in the file: a last boot carrying no label of
  # its own is refused rather than lent an earlier boot's. It is also the low side of the
  # boot-start comparison, the surviving commit line sitting ABOVE the title the slice is cut
  # at, where every complete report is the high side.
  sed '/^   commit  /d' "$CTL/cut" > "$CTL/cutnobanner"
  cat "$CTL/good" "$CTL/cutnobanner" > "$CTL/rebootdamaged"
  # A MOVING BOOT FOLLOWED BY A FROZEN ONE. The last boot is complete and its rows read one
  # constant over samples it counted; judged over the whole file the first boot's live rows
  # answer for it, and a counter that stopped reports as a live one.
  sed 's|  switch:    80/80/229 cyc|  switch:    80/80/80 cyc|' "$CTL/good" > "$CTL/frozenboot"
  cat "$CTL/good" "$CTL/frozenboot" > "$CTL/movingthenfrozen"
  # THE MIRROR OF THAT PAIR, and the one direction the per-window rule leaves to the slice: a
  # frozen boot under a LIVE one. Read whole the first boot's window refuses; read as the last
  # boot, which is what the caller hands the judgement, it is a live counter and a capture this
  # bench legitimately takes.
  cat "$CTL/frozenboot" "$CTL/good" > "$CTL/frozenthenmoving"

  for _c in good twowin threewin nocyc notitle; do
    bench_verdict "$CTL/$_c" "$EXPECT_COMMIT" "$CTL/$_c.slice" 2>/dev/null \
      || { rm -rf "$CTL"; refuse "the bench verdict refuses a planted '$_c' report, which is a
  complete one, so it would refuse captures this bench legitimately takes"; }
  done
  for _c in cut flip other damaged dropped nopasses winlost winbody winnosw twoprobe noloc \
            shortsweep longsweep malformed locshort noswitch reboot rebootdropped \
            rebootdamaged; do
    if bench_verdict "$CTL/$_c" "$EXPECT_COMMIT" "$CTL/$_c.slice" 2>/dev/null; then
      rm -rf "$CTL"
      refuse "the bench verdict passes a planted '$_c' report, so it cannot see that class and
  would report a capture carrying it as a complete witness"
    fi
  done

  # A REBOOT IS ALSO A WINDOW BOUNDARY, so this plant is refused sliced or whole. Its two boots
  # print a report window each and the liveness groups reset at every window, so the first
  # boot's spans cannot answer for the second's.
  bench_verdict "$CTL/movingthenfrozen" "$EXPECT_COMMIT" "$CTL/mtf.slice" 2>/dev/null \
    || { rm -rf "$CTL"; refuse "the bench verdict refuses a planted moving-then-frozen report,
  whose last boot is complete; the counter is the finding there and the completeness arms may
  not take it"; }
  if bench_cycles "$CTL/movingthenfrozen" >/dev/null 2>&1; then
    rm -rf "$CTL"
    refuse "the cycle judgement passes a planted moving-then-frozen report read WHOLE, so a
  counter that stopped is answered for by the boot that ran before it"
  fi
  if bench_cycles "$CTL/mtf.slice" >/dev/null 2>&1; then
    rm -rf "$CTL"
    refuse "the cycle judgement passes the last boot of a planted moving-then-frozen report, so
  a counter that stopped between two boots reports as a live one"
  fi
  bench_verdict "$CTL/frozenthenmoving" "$EXPECT_COMMIT" "$CTL/ftm.slice" 2>/dev/null \
    || { rm -rf "$CTL"; refuse "the bench verdict refuses a planted frozen-then-moving report,
  whose last boot is complete and at the expected commit"; }
  if bench_cycles "$CTL/frozenthenmoving" >/dev/null 2>&1; then
    rm -rf "$CTL"
    refuse "the cycle judgement passes a planted frozen-then-moving report read WHOLE, so a boot
  whose counter was constant is answered for by the boot that ran after it"
  fi
  bench_cycles "$CTL/ftm.slice" >/dev/null 2>&1 \
    || { rm -rf "$CTL"; refuse "the cycle judgement refuses the last boot of a planted
  frozen-then-moving report, whose counter is live; the slice is what the caller hands it and a
  boot the board has already left behind may not condemn the one it ran last"; }
  bench_cycles "$CTL/good.slice" >/dev/null 2>&1 \
    || { rm -rf "$CTL"; refuse "the cycle judgement refuses a planted live counter, so it would
  refuse every capture this bench takes"; }
  bench_cycles "$CTL/threewin.slice" >/dev/null 2>&1 \
    || { rm -rf "$CTL"; refuse "the cycle judgement refuses a planted live counter read over
  three report windows, so it would refuse every capture this bench takes"; }

  # ONE BOOT IS NOT ONE JUDGEMENT. The counter stops between two windows of the same boot, which
  # the slice cannot separate, so the plant must refuse on a report the completeness arms pass.
  bench_verdict "$CTL/winfrozen" "$EXPECT_COMMIT" "$CTL/winfrozen.slice" 2>/dev/null \
    || { rm -rf "$CTL"; refuse "the bench verdict refuses a planted report whose counter stopped
  between windows, which is a complete one; the counter is the finding there and the
  completeness arms may not take it"; }
  if bench_cycles "$CTL/winfrozen.slice" >/dev/null 2>&1; then
    rm -rf "$CTL"
    refuse "the cycle judgement passes a planted report live in its first window and constant in
  every window after it, so a counter that stopped mid-run reports as a live one"
  fi

  rm -rf "$CTL"
  echo "control: the bench verdict passes a complete report at the expected commit, one" >&2
  echo "  carrying two accounted windows, one carrying three, one whose switch row sampled" >&2
  echo "  nothing and carries no end-to-end denominator, and one whose banner title was lost" >&2
  echo "  and whose commit line stands in for it; it refuses a truncated one, one whose dirty" >&2
  echo "  state disagrees, one from another commit, one whose banner arrived damaged, one whose" >&2
  echo "  phase table lost a row, one carrying no end-to-end denominator beside a switch row" >&2
  echo "  that sampled, one whose SECOND window lost its accounting, one whose SECOND window" >&2
  echo "  lost its end-to-end block whole and kept the accounting, one whose SECOND window of" >&2
  echo "  three lost its switch row, one carrying two probes in one window, one whose locality" >&2
  echo "  rows are gone beside a probe that closed spans, one whose denominator is short of the" >&2
  echo "  sweep it ran and one above it, one whose denominator does not read as counts at all," >&2
  echo "  one whose locality rows are not the spans the probe closed, one with no switch row to" >&2
  echo "  read the condition off, and a complete report followed by a second boot that is cut" >&2
  echo "  short, lost a row, or lost its banner whole at the same commit. The cycle judgement" >&2
  echo "  passes a live counter over three windows, refuses one live in its first window and" >&2
  echo "  constant in the windows after it, refuses a moving-then-frozen report read whole and" >&2
  echo "  read as its last boot alone, and refuses a frozen-then-moving one read whole while" >&2
  echo "  passing the live last boot it is handed" >&2

  # RECORDED, NOT RAISED YET. The cycle judgement below is the more specific refusal
  # and it names the board's counter; a capture that is both truncated and dead-countered must
  # still refuse for the counter, so the completeness verdict is held until after it and the
  # two findings both reach the caller.
  BENCH_INCOMPLETE=0
  CYCLOG=$(mktemp) || refuse "cannot create a file for the capture's last boot"
  # Every refusal below this point leaves through exit, including the cycle judgement's own.
  trap 'rm -f "$CYCLOG"' EXIT
  if ! bench_verdict "$LOG" "$EXPECT_COMMIT" "$CYCLOG"; then
    BENCH_INCOMPLETE=1
    say_kickos_lines
  else
    printf 'bench:  complete report at %s\n' "$EXPECT_COMMIT"
  fi
  if [ ! -s "$CYCLOG" ]; then
    cp "$LOG" "$CYCLOG"
  fi
fi

# The cycle judgement reads the last boot for a bench capture and the whole log otherwise,
# there being no bench report to slice in that case.
if [ -z "${CYCLOG:-}" ]; then
  CYCLOG=$LOG
fi
if [ "$WANT_BENCH" -eq 1 ] && ! grep -aqE '^ *cycle counter: [0-9]+ Hz' "$CYCLOG"; then
  refuse "$LOG declares no cycle rate. The bench prints that line before anything else it
  measures, and a board that converts no reading prints 0 rather than nothing, so a report
  without it is one whose cycle rows were never judged."
fi
if ! bench_cycles "$CYCLOG"; then
  exit 1
fi

if [ "${BENCH_INCOMPLETE:-0}" -eq 1 ]; then
  refuse "$LOG is not a complete bench report of $EXPECT_COMMIT; the findings are above.
  A capture missing 'bench: done' holds every earlier line and stops short, so its numbers are
  whatever the window caught and not the sweep the app ran."
fi
