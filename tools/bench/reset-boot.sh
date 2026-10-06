#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sourced by bench-capture.sh, never executed.

# A whole banner title or commit line, full or terse, CR tolerated.
RESET_BOOT_START_RE='^(   KickOS [0-9][^ ]*  -  microkernel RTOS|K [0-9][^ ]*'
RESET_BOOT_START_RE+='|   commit  [A-Za-z0-9._/+-]+|c [A-Za-z0-9._/+-]+)'$'\r''?$'
RESET_BOOT_FILLER_RE='^(  =+)?'$'\r''?$'

# reset_boot_cut <log> <bytes>: cuts <log> to the boot that printed past its first <bytes>, from
# the first whole title or commit line there, or from <bytes> where anything but a banner rule or
# blank line comes ahead of that line. Returns 1, printing why, when no such line follows.
reset_boot_cut() {
  local log=$1 bytes=$2 post first lead=1
  post=$(mktemp) || return 1
  tail -c +"$((bytes + 1))" "$log" > "$post"
  first=$(grep -anE "$RESET_BOOT_START_RE" "$post" | head -n1 | cut -d: -f1)
  if [ -z "$first" ]; then
    rm -f "$post"
    echo "no whole banner title or commit line follows the reset in $log"
    return 1
  fi
  # The line <bytes> splits ends the earlier output, so it is no evidence of damage.
  if [ "$bytes" -gt 0 ] && [ -n "$(head -c "$bytes" "$log" | tail -c 1 | tr -d '\n')" ]; then
    lead=2
  fi
  if [ "$first" -gt "$lead" ] \
    && sed -n "${lead},$((first - 1))p" "$post" | grep -avqE "$RESET_BOOT_FILLER_RE"; then
    first=1
  elif [ "$first" -gt "$lead" ] && sed -n "$((first - 1))p" "$post" | grep -aqE '^  =+'$'\r''?$'
  then
    first=$((first - 1))
  fi
  tail -n +"$first" "$post" > "$log"
  rm -f "$post"
}
