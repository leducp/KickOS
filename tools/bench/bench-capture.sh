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
# happens where the hardware is and travels back as output plus an exit code. Splitting
# the sequence across ssh invocations would break it two ways: the armed reader is a
# child of its shell and does not survive to the next invocation, and a remote step that
# refuses while the local script keeps going reads as a pass.
#
# Env:
#   ROOT        directory holding tools/ and boards/ (the flash recipes). Default: this
#               script's grandparent.
#   KICKOS_RIG  the rig config naming the console cables. Default: $ROOT/.session/rig.conf.
#   PYBIN       a python carrying pyserial. Espressif capture only.
#   CAP_SECS    capture window override.
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
# WHICH DEVICE, AND WHERE THAT COMES FROM. A J-Link VCOM carries the probe serial the
# caller resolved live, so the row derives the path outright. The others are cables, and
# a cable is rig data. A row states a by-id pattern where the prefix names the probe on its
# own; the FTDI cables are named by rig.conf instead, because this bench carries a second
# FT232 and a CP210x belonging to other work and a vendor-keyed glob picks whichever
# enumerated first, so the capture that follows is complete, plausible, and the wrong
# board. A board with neither refuses by name. Any row can be pinned in rig.conf, and a pin
# always wins: do that the day a second ST-Link or CH34x joins the bus.
PORT=""
PATTERN=""
case $BOARD in
  xmc4800-relax|frdmk64f) PORT="/dev/serial/by-id/usb-SEGGER_J-Link_${SN}-if00" ;;
  f302nucleo)             PATTERN="/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_*-if02" ;;
  esp32c6-wroom)          PATTERN="/dev/serial/by-id/usb-1a86_USB_Single_Serial_*" ;;
  esp32-wroom)            PATTERN="/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0" ;;
  f411disco|rx72m|picopi|pizero2350|teensy41) ;;
  *) refuse "no console row for $BOARD; add one rather than guessing its probe" ;;
esac
RIGKEY="RIG_CONSOLE_$(printf '%s' "$BOARD" | tr 'a-z-' 'A-Z_')"
if [ -n "${!RIGKEY:-}" ]; then
  PATTERN="${!RIGKEY}"
  PORT=""
fi

# THE CONSOLE IS THE BOARD'S OWN USB: a _usbcdc list blinds the pin UART, so the cable the
# row above names goes quiet and resolving it would capture a silent cable.
#
# Keyed on our OWN product strings, not on 1209:0001, which is pid.codes' shared test pair
# (user/include/kickos/sys/usb_cdc.h) and matches anyone's prototype.
if [ "${CONSOLE_USB_CDC:-0}" = "1" ]; then
  case $BOARD in
    picopi|pizero2350|teensy41) ;;
    *) refuse "CONSOLE_USB_CDC is set but $BOARD has no USB device controller backend" ;;
  esac
  PATTERN="/dev/serial/by-id/usb-KickOS_KickOS_console_*-if00"
  PORT=""
fi

# Resolve $PATTERN to exactly one device. Split out because a self-USB console does not
# exist until the image boots, so that route calls this after the flash.
resolve_console() {
  if [ -n "$PORT" ]; then
    return 0
  fi
  [ -n "$PATTERN" ] || refuse "$BOARD's console cable is not named: set $RIGKEY in $RIG_CONF
  (see tools/bench/rig.conf.example). There is deliberately no vendor-pattern fallback
  here; it resolves to somebody else's cable and the capture still looks right."
  shopt -s nullglob
  MATCHES=($PATTERN)
  shopt -u nullglob
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
    refuse "$PORT is held. Kill the PID fuser reports, never pkill on the port."
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
# decide that. Measured 2026-08-10: a complete 95-ok K64F capture was refused this way.
note_reader() {
  [ -n "$READER" ] || return 0
  ps -p "$READER" > /dev/null && return 0
  echo "NOTE: the reader exited before the window closed (EOF on a quiet port). The counts" >&2
  echo "  below decide whether the capture is complete." >&2
}

# The FTDI-style reader is a LOOP around cat, so killing the loop leaves the running cat
# alive and REPARENTED, still holding the port. Three such orphans accumulated on 2026-08-10
# and split the bytes of the next capture between them, which is the silent-clobber trap:
# `pkill -P` does not reap it and a pattern kill would match its own command line. So the
# loop runs in its OWN PROCESS GROUP and the whole group is signalled.
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
    # THERE IS NO SEPARATE RESET STEP, AND REMOVING IT IS WHAT MADE THE f302 A RELIABLE
    # INSTRUMENT (measured 2026-08-13). Releasing NRST at the end of the write already starts
    # the image, so the write's own boot IS the authoritative run. This branch used to follow
    # the write with `st-flash reset` to guarantee a single boot; what that actually did was cut
    # the correct boot off MID-LINE and start a second one that then STALLED at the first
    # `[fs]`. Every capture came back truncated at a different point, and the board was written
    # up for weeks as having an unreliable post-fault console. It never did.
    #   with the reset:    2 boots, 355-412 bytes, survivor line cut ("...after the fa")
    #   without it:        1 boot,  300 bytes, "[fs] survivor ran after the fault" complete
    # Proven not to be firmware by a write-only capture of the PRE-drain-fix image (c0e3c835),
    # which carries its survivor line complete: same 300 bytes, same single boot.
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
    # THE FIRST LOAD FAILS AND THE SECOND SUCCEEDS, reliably enough that the operator had
    # been doing it by hand every time. Retried here rather than left to a human, because a
    # bench step that needs a known second try is a step an unattended pass cannot take.
    # Cause NOT established; do not infer one from the retry working.
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
    # SPEED IS TRIED DESCENDING, and 4000 alone was a MISDIAGNOSIS ENGINE. Measured
    # 2026-08-13 on frdmk64f SN 000621000000: at `-speed 4000` the connect stops right after
    # InitTarget() and never prints "identified.", while at 1000 the SAME probe on the SAME
    # boot identifies the core. The old refusal blamed the licence notice for that, which is a
    # tool asserting a cause it had not established, and it would have cost a
    # physically-present operator for a board that was working. There is more than one
    # physical K64F across desks, so treat the usable speed as a per-UNIT fact, not a constant.
    JOUT=""
    SWD_SPEED=""
    PROBE=$(mktemp)
    printf 'connect\nq\n' > "$PROBE"
    for _sp in 4000 1000 400; do
      JOUT=$(timeout 30 JLinkExe -nogui 1 -SelectEmuBySN "$SN" -device "$DEV" -if SWD -speed "$_sp" \
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
    JLINK_SN=$SN JLINK_SPEED=$SWD_SPEED FLASH_IMAGE="$IMG" "$ROOT/tools/flash-jlink.sh" "$BOARD" "$APP" > /dev/null 2>&1
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
    JLinkExe -nogui 1 -SelectEmuBySN "$SN" -device "$DEV" -if SWD -speed "$SWD_SPEED" \
             -CommanderScript "$RESET" > /dev/null 2>&1 || true
    rm -f "$RESET"
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
# clean pass, measured on f302nucleo as 51 ok against a 1..51 plan, which is exactly right for
# ONE run and was in fact a truncated boot plus a complete one. So the authoritative run is the
# LAST plan line to end of file, and a precursor is reported rather than added. The two-image
# boards run their images as separate invocations into separate logs, so one log normally owes
# exactly one plan line, and more than one means the board restarted inside the window.
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
BANNER=$(grep -aoE 'commit +[0-9a-f]{7,}(-dirty)?' "$LOG" | tail -1)
if [ -z "$BANNER" ]; then
  # THE SUFFIX MUST BE RECOVERED WITH THE HASH, and its absence must not read as clean.
  # Damage is byte LOSS, so a banner that reached the log as "c 06ffd64f" may have been
  # "commit 06ffd64f-dirty" with the suffix eaten. Measured on f302nucleo 2026-08-15: a
  # dirty-tree pass reported `commit 06ffd64f` while all seven other captures in the same
  # run reported `-dirty`, so a modified tree produced a capture that read as a witness.
  # Recovering a bare hash therefore says UNVERIFIED rather than nothing.
  RECOVERED=$(grep -aoE '(^|[^0-9a-z])[0-9a-f]{8}(-dirty)?([^0-9a-z]|$)' "$LOG" \
    | grep -oE '[0-9a-f]{8}(-dirty)?' | tail -1)
  if [ -n "$RECOVERED" ]; then
    case "$RECOVERED" in
      *-dirty) BANNER="commit $RECOVERED (banner damaged in transit)" ;;
      *) BANNER="commit $RECOVERED-UNVERIFIED (banner damaged in transit; -dirty could not be confirmed)" ;;
    esac
  fi
fi
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
# A TAP VERDICT IS OWED ONLY BY A TAP APP. The diagnostic apps announce no plan by design, so
# demanding one refused every one of them, and the refusal propagated far enough to skip the
# caller's log FETCH, throwing away a capture that was complete and correct. The EXPECTATION
# comes from the app name; the verdict still comes from the log, so a non-TAP app that does
# emit a plan is judged on it anyway.
case $APP in
  selftest*) WANT_TAP=1 ;;
  *)         WANT_TAP=0 ;;
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
