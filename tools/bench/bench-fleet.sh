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
TAG="${TAG:-m475}"
OUTDIR="$RIG_SESSION/logs"
mkdir -p "$OUTDIR"

ALL="rx72m f302nucleo esp32c6-wroom esp32-wroom xmc4800-relax frdmk64f"
WANT="${*:-$ALL}"

# WHICH IMAGES A BOARD'S BUILD SHIPS, asked of the tree one board at a time, as bench.sh's
# `<image>|<stdout>|<judge>` rows. The count is a property of that board's own configure: the
# selftest app cuts its registration list into regions and groups them into as many images as
# the board's flash or code window takes, once under the kernel's console and once under each
# console driver the board composes, and each board app names the gate script that judges it. A list here would be a second authority, and this script carried
# one: it named two boards that had gone to four images and did not name esp32c6-wroom at all,
# so a fleet pass flashed one image of three on that board. TAP numbering RESTARTS at 1 in each
# image, so a lone first plan line is a FRACTION of a run and not a short one, and the pass read
# green for two splits.
#
# The configure this does is the one the runs below reuse: same TAG, same board, same variant,
# so it lands in the same build dir and costs nothing twice.
images_for() { # <board> <stderr out> [variant]
  if [ -n "${3:-}" ]; then
    TAG="$TAG" VARIANT="$3" LIST_IMAGES=1 "$BENCH" "$1" 2>"$2"
  else
    TAG="$TAG" LIST_IMAGES=1 "$BENCH" "$1" 2>"$2"
  fi
}

# The board's AMP partition is the image a node 0 build of its amp2 preset assembles, so a board
# has one exactly where the tree declares that preset.
AMP_VARIANT=amp2-n0
has_variant() { # <board> <variant>
  (cd "${TREE:-$RIG_TREE}" && cmake --list-presets=configure 2>/dev/null) | grep -qF "\"$1-$2\""
}
# An image only the board's flat build ships (one that reads what enforcement would refuse it) is
# captured from that build; an image both builds ship is the enforcing build's.
FLAT_VARIANT=flat

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
record() {
  RESULTS="${RESULTS}$(printf '%-16s %s' "$1" "$2")
"
}

# The judge bench.sh names for a selftest image: the capture runs it over the TAP stream.
TAP_JUDGE=tests/integration/check_tap_stream.sh

# Runs bench.sh for ONE board and ONE image. The serial, when a board needs one, is
# passed as its own argument here and nowhere else.
# With <variant> set, the image is that variant's build's; with <amp> 1 as well, the run is that
# variant's AMP partition rather than one image.
bench_one() {
  local board=$1 app=$2 sn=$3 label=$4 judge=$5 variant=${6:-} amp=${7:-0} out rc
  local -a vars=(TAG="$TAG" APP="$app")
  if [ -n "$variant" ]; then
    vars+=(VARIANT="$variant")
  fi
  if [ "$amp" = "1" ]; then
    vars+=(AMP_PARTITION=1)
  fi
  out=$(mktemp)
  if [ -n "$sn" ]; then
    env "${vars[@]}" "$BENCH" "$board" "$sn" > "$out" 2>&1 < /dev/null
  else
    env "${vars[@]}" "$BENCH" "$board" > "$out" 2>&1 < /dev/null
  fi
  rc=$?
  if [ $rc -ne 0 ]; then
    record "$label" "FAILED rc=$rc: $(grep -m1 -E 'REFUSING|FAIL' "$out" || echo 'see log below')"
    grep -E 'REFUSING|FAIL|Error|error:' "$out" | head -5 | sed 's/^/    /'
    rm -f "$out"
    return 1
  fi
  if [ "$judge" != "$TAP_JUDGE" ]; then
    rm -f "$out"
    record "$label" "PASS ($judge)"
    return 0
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
OWED=""
UNJUDGED=""
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
  LERR=$(mktemp)
  IMAGES=$(images_for "$board" "$LERR")
  if [ -z "$IMAGES" ]; then
    record "$board" "REFUSED (the tree was not able to say which images this board ships): $(grep -m1 REFUSING "$LERR" || echo 'see the configure output')"
    grep -E 'REFUSING|Error|error:' "$LERR" | head -5 | sed 's/^/    /'
    rm -f "$LERR"
    FAILED=1
    continue
  fi
  rm -f "$LERR"
  # EVERY IMAGE, AND THE LABEL SAYS WHICH ONE. Each selftest image carries its own plan starting
  # at 1, so a figure in the table below belongs to an image rather than to the board. A console
  # driver is only in the images whose composition names it, so a green kernel-console run says
  # nothing about the driver.
  while IFS='|' read -r img _stdout judge <&4; do
    [ -n "$img" ] || continue
    if [ "$judge" = "-" ]; then
      UNJUDGED="$UNJUDGED$board $img
"
      continue
    fi
    OWED="$OWED$board $img
"
    if [ "$DRY_RUN" = "1" ]; then
      record "$board/$img" "WOULD FLASH (dry run; judge $judge)"
      continue
    fi
    if bench_one "$board" "$img" "$SN" "$board/$img" "$judge"; then
      COVERED="$COVERED$board $img
"
    else
      FAILED=1
    fi
  done 4<<ROWS
$IMAGES
ROWS
  if has_variant "$board" "$FLAT_VARIANT"; then
    LERR=$(mktemp)
    FLAT_IMAGES=$(images_for "$board" "$LERR" "$FLAT_VARIANT")
    if [ -z "$FLAT_IMAGES" ]; then
      record "$board/$FLAT_VARIANT" "REFUSED (the tree was not able to say which images this board's $FLAT_VARIANT build ships): $(grep -m1 REFUSING "$LERR" || echo 'see the configure output')"
      FAILED=1
    fi
    rm -f "$LERR"
    while IFS='|' read -r img _stdout judge <&4; do
      [ -n "$img" ] || continue
      if printf '%s\n' "$IMAGES" | awk -F '|' -v i="$img" '$1 == i { f = 1 } END { exit !f }'; then
        continue
      fi
      label="$img ($FLAT_VARIANT)"
      if [ "$judge" = "-" ]; then
        UNJUDGED="$UNJUDGED$board $label
"
        continue
      fi
      OWED="$OWED$board $label
"
      if [ "$DRY_RUN" = "1" ]; then
        record "$board/$label" "WOULD FLASH (dry run; judge $judge)"
        continue
      fi
      if bench_one "$board" "$img" "$SN" "$board/$label" "$judge" "$FLAT_VARIANT"; then
        COVERED="$COVERED$board $label
"
      else
        FAILED=1
      fi
    done 4<<ROWS
$FLAT_IMAGES
ROWS
  fi
  if has_variant "$board" "$AMP_VARIANT"; then
    img=amp_partition
    LERR=$(mktemp)
    judge=$(images_for "$board" "$LERR" "$AMP_VARIANT" | awk -F '|' '$1 == "ampping_n0" { print $3; exit }')
    rm -f "$LERR"
    if [ -z "$judge" ] || [ "$judge" = "-" ]; then
      UNJUDGED="$UNJUDGED$board $img
"
    else
      OWED="$OWED$board $img
"
      if [ "$DRY_RUN" = "1" ]; then
        record "$board/$img" "WOULD FLASH (dry run; judge $judge)"
      elif bench_one "$board" ampping_n0 "$SN" "$board/$img" "$judge" "$AMP_VARIANT" 1; then
        COVERED="$COVERED$board $img
"
      else
        FAILED=1
      fi
    fi
  fi
done

echo
echo "=== fleet pass, TAG=$TAG"
printf '%s' "$RESULTS"
echo "logs: $OUTDIR/$TAG*-*.log"
if [ "$ABSENT" -ne 0 ]; then
  echo "an ABSENT board is absent from $BENCH_WHERE, and nowhere else was asked."
  echo "  tools/bench/bench-present.sh reports the whole bus, probe serials and consoles."
fi

# COVERAGE, stated rather than assumed. A pass that skipped an image is not a pass over that
# board.
echo
echo "=== image coverage"
UNCOVERED=0
while read -r board img; do
  [ -n "$board" ] || continue
  if printf '%s' "$COVERED" | grep -qxF "$board $img"; then
    printf '  %-16s %-38s captured\n' "$board" "$img"
  else
    printf '  %-16s %-38s NOT RUN\n' "$board" "$img"
    UNCOVERED=$((UNCOVERED + 1))
  fi
done <<OWED
$OWED
OWED
while read -r board img; do
  [ -n "$board" ] || continue
  printf '  %-16s %-38s NO JUDGE\n' "$board" "$img"
  UNCOVERED=$((UNCOVERED + 1))
done <<UNJUDGED
$UNJUDGED
UNJUDGED
if [ "$UNCOVERED" -ne 0 ]; then
  echo
  echo "INCOMPLETE: $UNCOVERED image(s) a board ships were not captured, or no judge names them, so"
  echo "  this pass does not cover those boards."
  exit 1
fi
exit $FAILED
