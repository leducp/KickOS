#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A golden system run (docs/design-m10-target.md, section 8): install the build's package in a
# scratch prefix, configure examples/composition against it with the package's toolchain file,
# build sensor_system and run it until the app has printed three distinct readings. Once the run
# has ended the whole capture is checked for no failure line and no panic or fault banner.
#
#   check_golden_system.sh <kickos-build> <kickos-source> <cmake> [<example-dir>]
#
# <example-dir> defaults to <kickos-source>/examples/composition, the golden as written. With
# GOLDEN_LINK_ONLY=1, on a board no emulator runs, it stops once sensor_system has linked. With
# KOS_CAPTURE=<log>, the last boot in a silicon capture of the system is judged instead. Each
# packaged driver the composition names prints its up line before the first reading.
#
# The readings are the half nothing but the whole system produces: the app prints one only
# when a call to the sensor, which reads its device through the window the init granted, came
# back with a value other than the last.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

USAGE="usage: check_golden_system.sh <kickos-build> <kickos-source> <cmake> [<example-dir>]"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
EXAMPLE="${4:-$KICKOS_SRC/examples/composition}"
: "${GOLDEN_LINK_ONLY:=0}"
if [ "$GOLDEN_LINK_ONLY" -ne 1 ] && [ -z "${KOS_CAPTURE:-}" ]; then
  need_qemu_machine
fi

scratch_dir

if [ -n "${KOS_CAPTURE:-}" ]; then
  echo "== reading the golden system's capture $KOS_CAPTURE =="
  capture_out "$KOS_CAPTURE"
else
  echo "== building sensor_system of $EXAMPLE against the installed package =="
  package_image "$KICKOS_BUILD" "$CMAKE" "$EXAMPLE" sensor_system
  if [ "$GOLDEN_LINK_ONLY" -eq 1 ]; then
    echo "PASS: the golden system links against the package"
    exit 0
  fi

  # Satisfied once the console holds three distinct readings.
  three_readings() { # <log>
    [ "$(tr -d '\r' < "$1" | sed -n 's/^sensor: \([0-9][0-9]*\)$/\1/p' | sort -u | wc -l)" -ge 3 ]
  }
  KOS_POLL_UNTIL=three_readings
  echo "== running sensor_system =="
  poll_image "$IMAGE"
fi

# Over the whole capture, once the run has ended.
assert_no_panic "a panic in the golden system"
for _line in '^=== THREAD FAULT ===' '^sensor: no reading' '^sensor: restarting' '^sensor: died' \
             '^sensor: gone for good' '^sensor: measurement frozen' '^health:'; do
  if has "$_line"; then
    fail "the capture holds a '$_line' line"
  fi
done
READINGS="$(printf '%s\n' "$OUT" | sed -n 's/^sensor: \([0-9][0-9]*\)$/\1/p' | sort -u | wc -l)"
if [ -n "${KOS_CAPTURE:-}" ]; then
  if [ "$READINGS" -lt 3 ]; then
    fail "$READINGS distinct reading(s) in the capture, not three"
  fi
elif [ "$POLL_UNTIL_OK" -ne 1 ]; then
  if [ "$POLL_ALIVE" -eq 0 ]; then
    fail "the system ended after $READINGS distinct reading(s), before the third"
  fi
  fail "$READINGS distinct reading(s) in ${QEMU_TIMEOUT}s, not three"
fi
BOARD="$(sed -n 's/^KICKOS_BOARD:[A-Z]*=//p' "$KICKOS_BUILD/CMakeCache.txt")"
[ -n "$BOARD" ] || fail "$KICKOS_BUILD/CMakeCache.txt names no KICKOS_BOARD"
FIRST="$(printf '%s\n' "$OUT" | grep -n '^sensor: [0-9][0-9]*$' | head -n 1 | cut -d: -f1)"
require_drivers_up "$EXAMPLE/systems/$BOARD.yaml" "$FIRST"
echo "PASS: the golden system read the sensor $READINGS distinct times with no failure line"
