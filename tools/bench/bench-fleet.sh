#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The fleet silicon pass: the boards enumerated in ALL below, one at a time.
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
# than failed. Here the serial is resolved INSIDE the script and passed as its own
# quoted argument, so there is no pair for a caller to mis-split.
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
. "$HERE/bench-host.sh"
. "$HERE/board-rows.sh"
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

# WHICH IMAGES A BOARD'S SUITE SHIPS AS, asked of the tree one board at a time. The count is a
# property of that board's own configure: the selftest app cuts its registration list into
# regions and groups them into as many images as the board's flash or code window takes, and it
# publishes the names it emitted. A list here would be a second authority, and this script
# carried one: it named two boards that had gone to four images and did not name esp32c6-wroom
# at all, so a fleet pass flashed one image of three on that board. TAP numbering RESTARTS at 1
# in each image, so a lone first plan line is a FRACTION of a run and not a short one, and the
# pass read green for two splits.
#
# The configure this does is the one the runs below reuse: same TAG, same board, same variant,
# same service list, so it lands in the same build dir and costs nothing twice.
images_for() { # <board> <service list> <stderr out>
  TAG="$TAG" SERVICE_LIST="$2" LIST_IMAGES=1 "$BENCH" "$1" 2>"$3"
}

# ONE enumeration of the bus, taken once, wherever the boards are.
bench_host_select "${BENCH_HOST:-}"
# Selecting the mode is BENCH_HOST's job and not the rig config's: a key must not move a
# flashing run from one machine to another.
if [ -z "${BENCH_HOST:-}" ] && [ -n "${RIG_BENCH_HOST:-}" ]; then
  echo "NOTE: BENCH_HOST is unset, so this pass reads THIS BOX, while $RIG_CONF names"
  echo "  $RIG_BENCH_HOST as the bench. tools/bench/bench-present.sh says where the boards are."
fi
echo "=== $BENCH_WHERE"
# DRY_RUN=1 asks WHICH IMAGES A PASS WOULD FLASH and flashes none. That is a question about the
# configured build and not about the bus, so no board is asked for either: the enumeration and
# the presence checks below are what a real pass owes, and every line this mode prints says it
# witnessed nothing.
DRY_RUN="${DRY_RUN:-0}"
if [ "$DRY_RUN" = "1" ]; then
  echo "=== DRY RUN: no board is asked for, nothing is flashed, and nothing below is a witness."
else
  bench_bus_read
  case $? in
    1) echo "REFUSING: could not enumerate the bus on $BENCH_WHERE. Run tools/bench/bench-present.sh reach." >&2
       exit 2 ;;
    2) echo "REFUSING: the bus enumeration came back empty" >&2
       exit 2 ;;
  esac
fi

RESULTS=""
# THE SERVICE LISTS A BOARD OWES A FULL PASS, derived from the tree rather than listed here:
# a provider is named kickos_services_<board-ish>[_variant], so the tree IS the declaration and a
# provider added tomorrow is owed tomorrow. Prints the DEFAULT list first (empty string, meaning
# "whatever the preset defaults to") then every variant.
#
# This exists because a fleet pass that runs only the default list reports a clean sweep while
# saying nothing about the lists it never ran.
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
ABSENT=0
COVERED=""
for board in $WANT; do
  SN=""
  if [ "$DRY_RUN" != "1" ]; then
    ROWS=$(board_probe_rows "$board")
    case $? in
      1) record "$board" "REFUSED (no row; add one to tools/bench/board-rows.sh rather than guessing its probe)"
         FAILED=1
         continue ;;
      2) record "$board" "REFUSED ($ROWS)"
         FAILED=1
         continue ;;
    esac
    MISS=""
    while IFS='|' read -r id flag what; do
      [ -n "$id" ] || continue
      if ! usb_present "$id"; then
        MISS="$id is not on the bus: $what"
        break
      fi
      if [ "$flag" = "sn" ]; then
        SN=$(usb_serial_of "${id%%:*}" "${id##*:}") || { MISS="$id carries no serial descriptor: $what"; break; }
      fi
    done <<EOF
$ROWS
EOF
    if [ -n "$MISS" ]; then
      record "$board" "ABSENT ($MISS)"
      ABSENT=1
      continue
    fi
  fi

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
    LERR=$(mktemp)
    IMAGES=$(TAG="$ltag" images_for "$board" "$list" "$LERR")
    if [ -z "$IMAGES" ]; then
      record "$llabel" "REFUSED (the tree was not able to say which images this board ships): $(grep -m1 REFUSING "$LERR" || echo 'see the configure output')"
      grep -E 'REFUSING|Error|error:' "$LERR" | head -5 | sed 's/^/    /'
      rm -f "$LERR"
      FAILED=1
      continue
    fi
    rm -f "$LERR"
    # EVERY IMAGE, AND THE LABEL SAYS WHICH ONE. Each carries its own plan starting at 1, so a
    # figure in the table below belongs to an image rather than to the board.
    for img in $IMAGES; do
      if [ "$DRY_RUN" = "1" ]; then
        record "$llabel/$img" "WOULD FLASH (dry run; no capture, no verdict)"
        continue
      fi
      TAG="$ltag" SERVICE_LIST="$list" bench_one "$board" "$img" "$SN" "$llabel/$img" || FAILED=1
    done
    COVERED="$COVERED$board ${list:-@default}
"
  done
done

echo
echo "=== fleet pass, TAG=$TAG"
printf '%s' "$RESULTS"
echo "logs: $OUTDIR/$TAG*-*.log"
if [ "$ABSENT" -ne 0 ]; then
  echo "an ABSENT board is absent from $BENCH_WHERE, and nowhere else was asked."
  echo "  tools/bench/bench-present.sh reports the whole bus, probe serials and consoles."
fi

# COVERAGE, stated rather than assumed. A pass that skipped a list is not a pass over that
# board.
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
