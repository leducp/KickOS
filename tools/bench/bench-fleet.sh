#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The fleet silicon pass: every board that is ON THE BUS RIGHT NOW, one at a time.
#
#   TAG=<tag> tools/bench/bench-fleet.sh              # everything enumerated
#   TAG=<tag> tools/bench/bench-fleet.sh rx72m xmc4800-relax
#
# REMOTE MODE, when the bench is not on this box:
#   BENCH_HOST=<bench-host> BENCH_PORT=<port> TAG=<tag> tools/bench/bench-fleet.sh
#
# WHY THIS EXISTS, and the rule it enforces: a caller must NEVER pair a board with a
# probe serial by hand. Writing `for b in "xmc4800-relax 000591165808"; do bench.sh $b`
# works in bash and silently does NOT in zsh, which does not word-split: the whole
# string arrives as one board name, the cmake preset is malformed, and bench.sh exits
# at its configure line BEFORE printing anything. Two boards then look skipped rather
# than failed. That is exactly how it bit on 2026-08-06. Here the serial is resolved
# INSIDE the script and passed as its own quoted argument, so there is no pair for a
# caller to mis-split.
#
# Serials are resolved LIVE from the bus, never taken from a note: there is more than
# one physical XMC and K64F in rotation and the serials are not desk facts. In remote
# mode they are resolved ON THE BENCH HOST, because this box's bus says nothing about
# which boards are plugged into that one. A board that is absent is REPORTED as absent,
# not silently skipped.
set -u

# readlink, because .session/ carries a SYMLINK to this script for muscle memory: without
# it $0's directory is .session/ and bench.sh is not there.
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
BENCH="$HERE/bench.sh"
. "$HERE/rig.sh"
rig_load "$(cd "$HERE/../.." && pwd)"
rig_need RIG_SESSION "the session directory receiving logs/"
rig_need RIG_TREE "the tree to build when the caller sets no TREE"
# The tree the service-list providers are declared in; lists_for greps it.
ROOT="${TREE:-$RIG_TREE}"
TAG="${TAG:-m475}"
OUTDIR="$RIG_SESSION/logs"
mkdir -p "$OUTDIR"

ALL="rx72m f302nucleo esp32c6-wroom esp32-wroom xmc4800-relax frdmk64f"
WANT="${*:-$ALL}"

# Boards whose suite outgrew a 64 KiB flash and ships as two images. TAP numbering
# RESTARTS at 1 in each, so a lone first plan line is HALF a run, not a short one.
two_image() {
  case $1 in
    f302nucleo|bluepill-c8) return 0 ;;
    *) return 1 ;;
  esac
}

# One enumeration of the bus, taken once, wherever the boards are. Every presence and
# serial question below reads THIS, so the local and the remote answer come from the
# same code rather than from two implementations that can drift.
ENUM_SCRIPT='
for d in /sys/bus/usb/devices/*/; do
  [ -r "$d/idVendor" ] && [ -r "$d/idProduct" ] || continue
  s=""
  [ -r "$d/serial" ] && s=$(cat "$d/serial")
  printf "%s:%s %s\n" "$(cat "$d/idVendor")" "$(cat "$d/idProduct")" "$s"
done
'
if [ -n "${BENCH_HOST:-}" ]; then
  echo "=== bench host ${BENCH_HOST}:${BENCH_PORT:-${RIG_BENCH_PORT:-22}}"
  # bash -s with the script on stdin: the remote login shell is zsh, which aborts on an
  # unmatched glob, and /sys/bus/usb/devices/*/ is exactly that on a box with no bus.
  BUS=$(ssh -p "${BENCH_PORT:-${RIG_BENCH_PORT:-22}}" -o BatchMode=yes "$BENCH_HOST" bash -s <<< "$ENUM_SCRIPT") || {
    echo "REFUSING: could not enumerate the bus on $BENCH_HOST" >&2
    exit 2
  }
else
  BUS=$(bash -c "$ENUM_SCRIPT")
fi
[ -n "$BUS" ] || { echo "REFUSING: the bus enumeration came back empty" >&2; exit 2; }

usb_present() {
  printf '%s\n' "$BUS" | grep -q "^$1 "
}

# Echo the serial of the first device matching vid:pid, or fail if it has none.
usb_serial_of() {
  local s
  s=$(printf '%s\n' "$BUS" | awk -v k="$1:$2" '$1 == k && $2 != "" { print $2; exit }')
  [ -n "$s" ] || return 1
  echo "$s"
}

RESULTS=""
# THE SERVICE LISTS A BOARD OWES A FULL PASS, derived from the tree rather than listed here:
# a provider is named kickos_services_<board-ish>[_variant], so the tree IS the declaration and a
# provider added tomorrow is owed tomorrow. Prints the DEFAULT list first (empty string, meaning
# "whatever the preset defaults to") then every variant.
#
# This exists because a fleet pass that runs only the default list reports a clean sweep while
# saying nothing about the lists it never ran, and a scheduler regression lived in exactly that
# silence: green on every default-list board, broken only under an IRQ-driven UART.
lists_for() { # <board>
  local board=$1 key stem
  # TWO spellings, because the providers use both: the board with its dash removed
  # (xmc4800-relax -> kickos_services_xmc4800relax_*) and the board's first dash-segment
  # (esp32-wroom -> kickos_services_esp32_*). Deduped, since a dashless board matches both.
  key=$(printf '%s' "$board" | tr -d '-')
  stem=$(printf '%s' "$board" | cut -d- -f1)
  # A NAMED sentinel through printf, never an empty line and never a bare `-`. The caller reads
  # this through $(...), which word-splits, so an empty entry vanishes and the DEFAULT list is
  # silently skipped, by the very mechanism that exists to stop a list being silently skipped.
  # A bare `-` disappears too: echo ate it here, which is why this is printf and a word.
  printf '@default\n'
  {
    grep -rhoE "kickos_services_${key}_[a-z0-9_]+" "$ROOT" --include=CMakeLists.txt 2>/dev/null
    grep -rhoE "kickos_services_${stem}_[a-z0-9_]+" "$ROOT" --include=CMakeLists.txt 2>/dev/null
  } | sort -u
}

record() {
  RESULTS="${RESULTS}$(printf '%-16s %s' "$1" "$2")
"
}

# Runs bench.sh for ONE board and ONE image. The serial, when a board needs one, is
# passed as its own argument here and nowhere else.
bench_one() {
  local board=$1 app=$2 sn=$3 label=$4 out rc
  out=$(mktemp)
  if [ -n "$sn" ]; then
    TAG="$TAG" APP="$app" SERVICE_LIST="${SERVICE_LIST:-}" "$BENCH" "$board" "$sn" > "$out" 2>&1
  else
    TAG="$TAG" APP="$app" SERVICE_LIST="${SERVICE_LIST:-}" "$BENCH" "$board" > "$out" 2>&1
  fi
  rc=$?
  if [ $rc -ne 0 ]; then
    record "$label" "FAILED rc=$rc: $(grep -m1 REFUSING "$out" || echo 'see log below')"
    grep -E 'REFUSING|Error|error:' "$out" | head -5 | sed 's/^/    /'
    rm -f "$out"
    return 1
  fi
  # bench-capture.sh already prints the counts; fold them onto one line for the table.
  local okc notokc plan skipc partc banner mpu runs
  okc=$(sed -n 's/^ok: *//p'     "$out")
  notokc=$(sed -n 's/^not ok: *//p' "$out")
  plan=$(sed -n 's/^plan: *//p'  "$out")
  skipc=$(sed -n 's/^skip: *//p' "$out")
  partc=$(sed -n 's/^part: *//p' "$out")
  runs=$(sed -n 's/^runs: *//p'  "$out")
  banner=$(sed -n 's/^banner: *//p' "$out" | tr -s ' ')
  mpu=$(sed -n 's/^mpu: *//p'    "$out" | tr -s ' ')
  rm -f "$out"
  # A capture holding TWO plan lines ran the suite twice inside one window, so every
  # count is a sum over both runs, so it reads as a pass with inflated numbers. That is
  # how f302nucleo's counts were inflated twice. bench-capture.sh refuses on it, which
  # lands in the rc branch above; this states the count so the table never carries a
  # sum silently even if that refusal is ever loosened.
  record "$label" "$plan runs=$runs ok=$okc notok=$notokc skip=$skipc part=$partc [$mpu] $banner"
  [ "${runs:-0}" = "1" ] || { echo "    WARNING: $label captured $runs plan lines, not 1"; return 1; }
  # A plan line with zero ok is a capture that produced nothing, which reads as a pass
  # in a bare exit code.
  [ "${okc:-0}" -gt 0 ] || { echo "    WARNING: $label captured no ok lines"; return 1; }
  [ "${notokc:-0}" -eq 0 ]
}

FAILED=0
COVERED=""
for board in $WANT; do
  SN=""
  case $board in
    xmc4800-relax)
      SN=$(usb_serial_of 1366 1024) || { record "$board" "ABSENT (no J-Link idProduct 1024)"; continue; }
      ;;
    frdmk64f)
      SN=$(usb_serial_of 1366 1015) || { record "$board" "ABSENT (no J-Link idProduct 1015)"; continue; }
      ;;
    rx72m)
      usb_present 045b:82a0 || { record "$board" "ABSENT (no E2 Lite)"; continue; }
      usb_present 0403:6001 || { record "$board" "ABSENT (no FTDI for the SCI6 console)"; continue; }
      ;;
    f302nucleo)
      usb_present 0483:374b || { record "$board" "ABSENT (no ST-Link V2.1)"; continue; }
      ;;
    esp32c6-wroom)
      usb_present 1a86:55d3 || { record "$board" "ABSENT (no CH343P)"; continue; }
      ;;
    esp32-wroom)
      usb_present 1a86:7523 || { record "$board" "ABSENT (no CH340)"; continue; }
      ;;
    *)
      record "$board" "REFUSED (no row; add one rather than guessing its probe)"
      FAILED=1
      continue
      ;;
  esac

  echo "=== $board${SN:+  SN $SN}"
  # EVERY LIST THE BOARD OWES, not just the default. A pass that ran only the default list is
  # not a pass over this board: a driver is only in the image if the service list puts it there,
  # so a green default run says nothing about the driver, and a regression can live entirely in
  # the list nobody ran. Each list gets its OWN TAG, because TAG keys the log and two captures
  # of one app at one tag overwrite each other.
  for list in $(lists_for "$board"); do
    if [ "$list" = "@default" ]; then
      list=""
      ltag="$TAG"; llabel="$board/<default>"
    else
      ltag="$TAG$(printf '%s' "${list#kickos_services_}" | tr -d '_')"; llabel="$board/$list"
    fi
    if two_image "$board"; then
      TAG="$ltag" SERVICE_LIST="$list" bench_one "$board" selftest    "$SN" "$llabel/p1" || FAILED=1
      TAG="$ltag" SERVICE_LIST="$list" bench_one "$board" selftest_p2 "$SN" "$llabel/p2" || FAILED=1
    else
      TAG="$ltag" SERVICE_LIST="$list" bench_one "$board" selftest "$SN" "$llabel" || FAILED=1
    fi
    COVERED="$COVERED$board ${list:-@default}
"
  done
done

echo
echo "=== fleet pass, TAG=$TAG"
printf '%s' "$RESULTS"
echo "logs: $OUTDIR/$TAG*-*.log"

# COVERAGE, stated rather than assumed. A pass that skipped a list is not a pass over that
# board, and the whole reason this section exists is that the skip used to be SILENT: the
# summary above reported green boards while saying nothing about the lists never run.
echo
echo "=== service-list coverage"
UNCOVERED=0
for board in $WANT; do
  for list in $(lists_for "$board"); do
    shown=$list
    if [ "$list" = "@default" ]; then
      shown="<default>"
    fi
    if printf '%s' "$COVERED" | grep -qxF "$board $list"; then
      printf '  %-16s %-38s captured\n' "$board" "$shown"
    else
      printf '  %-16s %-38s NOT RUN\n' "$board" "$shown"
      UNCOVERED=$((UNCOVERED + 1))
    fi
  done
done
if [ "$UNCOVERED" -ne 0 ]; then
  echo
  echo "INCOMPLETE: $UNCOVERED declared service list(s) were not run, so this pass does not"
  echo "  cover those boards. A driver is only in the image if the service list puts it there,"
  echo "  so a green default-list run says nothing about the driver."
  exit 1
fi
exit $FAILED
