#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The fleet silicon pass over the boards enumerated in ALL below. Every image is built first; then
# each board's captures run in order in a worker of its own, boards that share a probe or a console
# cable (board_resources in board-rows.sh) in one worker, so the pass takes as long as its longest
# worker. Each worker's output and findings are merged, in board order, into one summary.
#
#   TAG=<tag> tools/bench/bench-fleet.sh              # everything enumerated
#   TAG=<tag> tools/bench/bench-fleet.sh rx72m xmc4800-relax
#
# Exit status: 0 when every image is captured and nothing is owed; 1 on a failed capture, an absent
# board, an image not run or one no judge names; 3 when every capture passed and the pass still
# owes a witness: an emulator-judged image on a board with no emulator, a human-judged one, or a
# clause a capture judge could not evaluate.
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

ALL="rx72m f302nucleo f411disco esp32c6-wroom esp32-wroom xmc4800-relax frdmk64f"
WANT="${*:-$ALL}"

# WHICH IMAGES A BOARD'S BUILD SHIPS, asked of the tree one board at a time, as bench.sh's
# `<image>|<stdout>|<judge>|<args>` rows. The count is a property of that board's own configure:
# the selftest app cuts its registration list into regions and groups them into as many images as
# the board's flash or code window takes, once under the kernel's console and once under each
# console driver the board composes, and each image names what judges it. A list here would be a
# second authority, and this script carried one: it named two boards that had gone to four images
# and did not name esp32c6-wroom at all, so a fleet pass flashed one image of three on that board.
# TAP numbering RESTARTS at 1 in each image, so a lone first plan line is a FRACTION of a run and
# not a short one, and the pass read green for two splits.
#
# The configure this does is the one the runs below reuse: same tag, same board, same variant,
# so it lands in the same build dir and costs nothing twice.
images_for() { # <board> <stderr out> [variant]
  if [ -n "${3:-}" ]; then
    TAG="$(variant_tag "$3")" VARIANT="$3" LIST_IMAGES=1 "$BENCH" "$1" 2>"$2"
  else
    TAG="$TAG" LIST_IMAGES=1 "$BENCH" "$1" 2>"$2"
  fi
}

# EVERY VARIANT A BOARD DECLARES, read off the tree's presets, `<board>-<variant>` each, or
# status 1 where the tree cannot list them. The default variant is bench.sh's own
# (BENCH_DEFAULT_VARIANT), the one the pass below takes first; every other one is a build of its
# own, and a capture of one of its images lands under that variant's tag, so two builds' logs of
# one image never share a name.
variants_of() { # <board>
  local presets
  presets=$(cd "${TREE:-$RIG_TREE}" && cmake --list-presets=configure 2>/dev/null) || return 1
  printf '%s\n' "$presets" | sed -n "s/^ *\"$1-\([A-Za-z0-9_-]*\)\".*/\1/p"
}
# A partition is the variants `<p>-n0` to `<p>-n<k>`, and its node 0 build assembles the whole
# image: node 0 is captured as the partition, and the others are parts of it.
partition_first() { # <variant>
  case $1 in
    *-n0) return 0 ;;
    *) return 1 ;;
  esac
}
partition_part() { # <variant> <variants>
  local p=${1%-n[0-9]*}
  [ "$p" != "$1" ] && ! partition_first "$1" && printf '%s\n' "$2" | grep -qxF "$p-n0"
}
variant_tag() { # <variant>
  printf '%s%s' "$TAG" "$(printf '%s' "$1" | tr -d '-')"
}
# A variant that turns KICKOS_BENCH on is the microbenchmark's posture, which the bench sweep
# measures; its images are not judged here.
measurement_variant() { # <board> <variant>
  grep -qx 'CONFIG_KICKOS_BENCH=y' "${TREE:-$RIG_TREE}/boards/$1/configs/$2/defconfig" 2>/dev/null
}

# The board's AMP partition is the image a node 0 build of its amp2 preset assembles.
AMP_VARIANT=amp2-n0
# An image only the board's flat build ships (one that reads what enforcement would refuse it) is
# captured from that build; an image both builds ship is the enforcing build's, except a
# <board>:<image> FLAT_ALSO names, which each build's capture witnesses.
FLAT_VARIANT=flat
FLAT_ALSO="rx72m:fpclass"
flat_also() { # <board> <image>
  printf '%s\n' "$FLAT_ALSO" | tr ' ' '\n' | grep -qxF "$1:$2"
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
record() {
  RESULTS="${RESULTS}$(printf '%-16s %s' "$1" "$2")
"
}

# The judge bench.sh names for a selftest image: the capture runs it over the TAP stream.
TAP_JUDGE=tests/integration/check_tap_stream.sh

# The environment bench.sh takes for one image, in VARS. With <variant> set, the image is that
# variant's build's; with <amp> 1 as well, the run is that variant's AMP partition rather than one
# image.
bench_vars() { # <app> <variant> <amp>
  VARS=(TAG="$TAG" APP="$1")
  if [ -n "$2" ]; then
    VARS=(TAG="$(variant_tag "$2")" APP="$1" VARIANT="$2")
  fi
  if [ "$3" = "1" ]; then
    VARS+=(AMP_PARTITION=1)
  fi
}

# Builds the image bench_one flashes, and flashes nothing. bench.sh's output lands in <out>.
build_one() { # <board> <app> <sn> <variant> <amp> <out>
  bench_vars "$2" "$4" "$5"
  env "${VARS[@]}" BUILD_ONLY=1 "$BENCH" "$1" ${3:+"$3"} > "$6" 2>&1 < /dev/null
}

# Runs bench.sh for ONE board and ONE image. The serial, when a board needs one, is
# passed as its own argument.
bench_one() {
  NOT_EVALUATED=""
  local board=$1 app=$2 sn=$3 label=$4 judge=$5 variant=${6:-} amp=${7:-0} out rc
  bench_vars "$app" "$variant" "$amp"
  out=$(mktemp)
  env "${VARS[@]}" "$BENCH" "$board" ${sn:+"$sn"} > "$out" 2>&1 < /dev/null
  rc=$?
  if [ $rc -ne 0 ]; then
    record "$label" "FAILED rc=$rc: $(grep -m1 -E 'REFUSING|FAIL' "$out" || echo 'see log below')"
    grep -E 'REFUSING|FAIL|Error|error:' "$out" | head -5 | sed 's/^/    /'
    rm -f "$out"
    return 1
  fi
  if [ "$judge" != "$TAP_JUDGE" ]; then
    NOT_EVALUATED=$(sed -n 's/^NOT EVALUATED: //p' "$out" | paste -sd ';' -)
    rm -f "$out"
    if [ -n "$NOT_EVALUATED" ]; then
      record "$label" "PASS ($judge), partly owed: $NOT_EVALUATED"
    else
      record "$label" "PASS ($judge)"
    fi
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

# Files one image of <board> by its judge: a mark is reported apart, and a judged image is
# captured, or named in a dry run.
take_image() { # <board> <image> <label> <judge> <args> [variant]
  local board=$1 img=$2 label=$3 judge=$4 args=$5 variant=${6:-}
  case $judge in
    -)
      UNJUDGED="$UNJUDGED$board $label
"
      return
      ;;
    emulator)
      EMULATED="$EMULATED$board $label
"
      return
      ;;
    emulator-owed)
      EMULATOR_OWED="$EMULATOR_OWED$board $label
"
      return
      ;;
    human)
      HUMAN_OWED="$HUMAN_OWED$board|$label|$args
"
      return
      ;;
    inapplicable)
      VOID="$VOID$board|$label|$args
"
      return
      ;;
    *) ;;
  esac
  capture_image "$board" "$img" "$label" "$judge" "$variant" 0
}

# Queues one judged image for its board's worker, or names it in a dry run.
capture_image() { # <board> <image> <label> <judge> <variant> <amp>
  local board=$1 img=$2 label=$3 judge=$4 variant=$5 amp=$6
  OWED="$OWED$board $label
"
  if [ "$DRY_RUN" = "1" ]; then
    record "$board/$label" "WOULD FLASH (dry run; judge $judge)"
    return
  fi
  JOBS="$JOBS$board|$img|$label|$judge|$variant|$amp|$SN
"
}

# The worker a board's captures run in: the one already holding a probe or a console the board
# holds, else its own. A board holding what two workers hold joins them into one.
declare -A WORKER_OF=() HOLDER=()
join_worker() { # <board> <probe-serial>
  local w=$1 r k
  WORKER_OF[$1]=$1
  while IFS= read -r r; do
    if [ -n "${HOLDER[$r]:-}" ] && [ "${HOLDER[$r]}" != "$w" ]; then
      for k in "${!WORKER_OF[@]}"; do
        [ "${WORKER_OF[$k]}" != "$w" ] || WORKER_OF[$k]=${HOLDER[$r]}
      done
      for k in "${!HOLDER[@]}"; do
        [ "${HOLDER[$k]}" != "$w" ] || HOLDER[$k]=${HOLDER[$r]}
      done
      w=${HOLDER[$r]}
    fi
    HOLDER[$r]=$w
  done < <(board_resources "$1" "$2")
}

# One worker's captures, in the order they were queued. Its findings go to $WORK/<worker>.<name>,
# since a background worker's variables die with it.
worker() { # <worker>
  local board img label judge variant amp sn
  RESULTS=""
  COVERED=""
  PARTLY=""
  FAILED=0
  while IFS='|' read -r board img label judge variant amp sn <&4; do
    [ -n "$board" ] && [ "${WORKER_OF[$board]}" = "$1" ] || continue
    if bench_one "$board" "$img" "$sn" "$board/$label" "$judge" "$variant" "$amp"; then
      COVERED="$COVERED$board $label
"
      if [ -n "$NOT_EVALUATED" ]; then
        PARTLY="$PARTLY$board|$label|$NOT_EVALUATED
"
      fi
    else
      FAILED=1
    fi
  done 4<<JOBS
$BUILT
JOBS
  printf '%s' "$RESULTS" > "$WORK/$1.RESULTS"
  printf '%s' "$COVERED" > "$WORK/$1.COVERED"
  printf '%s' "$PARTLY" > "$WORK/$1.PARTLY"
  printf '%s' "$FAILED" > "$WORK/$1.FAILED"
}

FAILED=0
ABSENT=0
COVERED=""
OWED=""
UNJUDGED=""
EMULATED=""
EMULATOR_OWED=""
HUMAN_OWED=""
VOID=""
PARTLY=""
MEASURED=""
JOBS=""
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
      ABSENT=$((ABSENT + 1))
      continue
    fi
  fi

  echo "=== $board${SN:+  SN $SN}"
  join_worker "$board" "$SN"
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
  while IFS='|' read -r img _stdout judge args <&4; do
    [ -n "$img" ] || continue
    take_image "$board" "$img" "$img" "$judge" "$args"
  done 4<<ROWS
$IMAGES
ROWS
  if ! VARIANTS=$(variants_of "$board"); then
    record "$board" "REFUSED (the tree was not able to list its presets, so this board's variants are unknown)"
    FAILED=1
    continue
  fi
  for variant in $VARIANTS; do
    if [ "$variant" = "$BENCH_DEFAULT_VARIANT" ] || partition_part "$variant" "$VARIANTS"; then
      continue
    fi
    if partition_first "$variant"; then
      img="amp_partition ($variant)"
      if [ "$variant" = "$AMP_VARIANT" ]; then
        img=amp_partition
      fi
      LERR=$(mktemp)
      AMP_IMAGES=$(images_for "$board" "$LERR" "$variant")
      if [ -z "$AMP_IMAGES" ]; then
        record "$board/$variant" "REFUSED (the tree was not able to say which images this board's $variant build ships): $(grep -m1 REFUSING "$LERR" || echo 'see the configure output')"
        FAILED=1
        rm -f "$LERR"
        continue
      fi
      rm -f "$LERR"
      judge=$(printf '%s\n' "$AMP_IMAGES" | awk -F '|' '$1 == "ampping_n0" { print $3; exit }')
      if [ -z "$judge" ] || [ "$judge" = "-" ]; then
        UNJUDGED="$UNJUDGED$board $img
"
      else
        capture_image "$board" ampping_n0 "$img" "$judge" "$variant" 1
      fi
      continue
    fi
    if measurement_variant "$board" "$variant"; then
      MEASURED="$MEASURED$board $variant
"
      continue
    fi
    LERR=$(mktemp)
    VAR_IMAGES=$(images_for "$board" "$LERR" "$variant")
    if [ -z "$VAR_IMAGES" ]; then
      record "$board/$variant" "REFUSED (the tree was not able to say which images this board's $variant build ships): $(grep -m1 REFUSING "$LERR" || echo 'see the configure output')"
      FAILED=1
    fi
    rm -f "$LERR"
    while IFS='|' read -r img _stdout judge args <&4; do
      [ -n "$img" ] || continue
      if [ "$variant" = "$FLAT_VARIANT" ] \
        && printf '%s\n' "$IMAGES" | awk -F '|' -v i="$img" '$1 == i { f = 1 } END { exit !f }' \
        && ! flat_also "$board" "$img"; then
        continue
      fi
      take_image "$board" "$img" "$img ($variant)" "$judge" "$args" "$variant"
    done 4<<ROWS
$VAR_IMAGES
ROWS
  done
done

# EVERY IMAGE IS BUILT BEFORE THE FIRST FLASH, so no capture waits on the compiler. An image that
# does not build is a failed capture, and its worker never flashes it.
BUILT=""
while IFS='|' read -r board img label judge variant amp sn <&4; do
  [ -n "$board" ] || continue
  echo "=== building $board/$label"
  out=$(mktemp)
  if build_one "$board" "$img" "$sn" "$variant" "$amp" "$out"; then
    BUILT="$BUILT$board|$img|$label|$judge|$variant|$amp|$sn
"
  else
    record "$board/$label" "FAILED to build: $(grep -m1 -E 'REFUSING|FAIL|error' "$out" || echo 'see the build output')"
    grep -E 'REFUSING|FAIL|Error|error:' "$out" | head -5 | sed 's/^/    /'
    FAILED=1
  fi
  rm -f "$out"
done 4<<JOBS
$JOBS
JOBS

WORK=$(mktemp -d)
WORKERS=$(printf '%s' "$BUILT" | while IFS='|' read -r board _; do
  printf '%s\n' "${WORKER_OF[$board]}"
done | awk '!seen[$0]++')
for w in $WORKERS; do
  worker "$w" > "$WORK/$w.out" 2>&1 &
done
wait
# A worker that left no verdict behind failed, and its images read NOT RUN below.
for w in $WORKERS; do
  members=""
  for board in $WANT; do
    [ "${WORKER_OF[$board]:-}" != "$w" ] || members="$members $board"
  done
  echo "=== worker:$members"
  cat "$WORK/$w.out"
  for part in RESULTS COVERED PARTLY; do
    if [ -s "$WORK/$w.$part" ]; then
      printf -v "$part" '%s%s\n' "${!part}" "$(cat "$WORK/$w.$part")"
    fi
  done
  [ "$(cat "$WORK/$w.FAILED" 2>/dev/null)" = 0 ] || FAILED=1
done
rm -rf "$WORK"

echo
echo "=== fleet pass, TAG=$TAG"
printf '%s' "$RESULTS"
echo "logs: $OUTDIR/$TAG*-*.log (a variant's under its own tag, $TAG<variant>)"
if [ "$ABSENT" -ne 0 ]; then
  echo "an ABSENT board is absent from $BENCH_WHERE, and nowhere else was asked."
  echo "  tools/bench/bench-present.sh reports the whole bus, probe serials and consoles."
fi

# COVERAGE, stated rather than assumed. A pass that skipped an image is not a pass over that
# board.
echo
echo "=== image coverage"
UNCOVERED=0
CLAUSES=0
while read -r board img; do
  [ -n "$board" ] || continue
  partly=$(printf '%s' "$PARTLY" | KOS_BOARD="$board" KOS_LABEL="$img" awk -F '|' '
    $1 == ENVIRON["KOS_BOARD"] && $2 == ENVIRON["KOS_LABEL"] {
      sub(/^[^|]*[|][^|]*[|]/, "")
      print
      exit
    }')
  if [ -n "$partly" ]; then
    printf '  %-16s %-38s captured, partly owed: %s\n' "$board" "$img" "$partly"
    CLAUSES=$((CLAUSES + $(printf '%s\n' "$partly" | tr ';' '\n' | grep -c .)))
  elif printf '%s' "$COVERED" | grep -qxF "$board $img"; then
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
while read -r board img; do
  [ -n "$board" ] || continue
  printf '  %-16s %-38s emulator-judged\n' "$board" "$img"
done <<EMULATED
$EMULATED
EMULATED
NOWITNESS=0
while read -r board img; do
  [ -n "$board" ] || continue
  printf '  %-16s %-38s emulator-judged, no emulator: owed\n' "$board" "$img"
  NOWITNESS=$((NOWITNESS + 1))
done <<EMULATOR_OWED
$EMULATOR_OWED
EMULATOR_OWED
while IFS='|' read -r board img why; do
  [ -n "$board" ] || continue
  printf '  %-16s %-38s inapplicable (%s)\n' "$board" "$img" "$why"
done <<VOID
$VOID
VOID
while read -r board variant; do
  [ -n "$board" ] || continue
  printf '  %-16s %-38s measurement posture, taken by the bench sweep\n' "$board" "variant $variant"
done <<MEASURED
$MEASURED
MEASURED
HUMANS=0
while IFS='|' read -r board img what; do
  [ -n "$board" ] || continue
  printf '  %-16s %-38s human-judged (%s): owed\n' "$board" "$img" "$what"
  HUMANS=$((HUMANS + 1))
done <<HUMAN_OWED
$HUMAN_OWED
HUMAN_OWED
if [ "$NOWITNESS" -ne 0 ]; then
  echo
  echo "OWED: $NOWITNESS emulator-judged image(s) have no emulator on their board, so nothing has"
  echo "  witnessed them there and this pass covers none of them."
fi
if [ "$HUMANS" -ne 0 ]; then
  echo
  echo "OWED: $HUMANS human-judged image(s) carry a verdict only a person reads, which this pass"
  echo "  cannot see and covers none of."
fi
if [ "$CLAUSES" -ne 0 ]; then
  echo
  echo "OWED: $CLAUSES clause(s) of captured images' verdicts: an exit status or the system ending,"
  echo "  which a capture does not carry, or a reply resting on a bench fitting the rig does not"
  echo "  declare (RIG_WIRED_<BOARD>)."
fi
if [ "$ABSENT" -ne 0 ]; then
  echo
  echo "INCOMPLETE: $ABSENT board(s) were absent, so this pass captured none of their images."
fi
if [ "$UNCOVERED" -ne 0 ]; then
  echo
  echo "INCOMPLETE: $UNCOVERED image(s) a board ships were not captured, or no judge names them, so"
  echo "  this pass does not cover those boards."
fi
if [ $((ABSENT + UNCOVERED)) -ne 0 ]; then
  exit 1
fi
if [ "$FAILED" -ne 0 ]; then
  exit 1
fi
if [ $((NOWITNESS + HUMANS + CLAUSES)) -ne 0 ]; then
  echo
  echo "OWED: every capture passed, and the pass still owes the witnesses above."
  exit 3
fi
exit 0
