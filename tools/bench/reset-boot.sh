#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sourced by bench-capture.sh, never executed.

# A whole banner title, full or terse, and every row only a banner prints after it. CR tolerated.
RESET_BOOT_TITLE_RE='^(   KickOS [0-9][^ ]*  -  microkernel RTOS|K [0-9][^ ]*)'$'\r''?$'
_rb_stamp='[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} [+-][0-9]{4}'
_rb_app='[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}( [+-][0-9]{4})?'
_rb_commit='[A-Za-z0-9._/+-]+'
RESET_BOOT_ROW_RE='^('\
'   board   [a-z0-9-]+|b [a-z0-9-]+|'\
'   arch    [a-z][a-z0-9_]*|a [a-z][a-z0-9_]*|'\
'   cpu     [A-Za-z0-9_.+-]+|u [A-Za-z0-9_.+-]+|'\
'   mpu     (off|enforce)|m (off|enforce)|'\
'   sched   (tickless|periodic tick)|s (tickless|periodic tick)|'\
"   build   $_rb_stamp|t $_rb_stamp|"\
"   app     $_rb_app|p $_rb_app|"\
"   commit  $_rb_commit|c $_rb_commit|"\
'   heap    ([0-9]+ KiB available|none)|h [0-9]+|'\
'   kstack  [0-9]+ B x [0-9]+ = [0-9]+ B|k [0-9]+ [0-9]+ [0-9]+'\
')'$'\r''?$'
unset _rb_stamp _rb_app _rb_commit

# reset_boot_cut <log> <bytes>: cuts <log> to the boot that printed past its first <bytes>, from
# its banner title (and the rule above a full one). Returns 1, printing why, when no whole title
# follows <bytes>, or when a banner row comes ahead of the first one: a boot whose title arrived
# damaged, which a cut at a later title would hide.
reset_boot_cut() {
  local log=$1 bytes=$2 post first row
  post=$(mktemp) || return 1
  tail -c +"$((bytes + 1))" "$log" > "$post"
  first=$(grep -anE "$RESET_BOOT_TITLE_RE" "$post" | head -n1 | cut -d: -f1)
  row=$(grep -anE "$RESET_BOOT_ROW_RE" "$post" | head -n1 | cut -d: -f1)
  if [ -n "$row" ] && { [ -z "$first" ] || [ "$row" -lt "$first" ]; }; then
    rm -f "$post"
    echo "a boot after the reset in $log arrived without its banner title"
    return 1
  fi
  if [ -z "$first" ]; then
    rm -f "$post"
    echo "no whole banner title follows the reset in $log"
    return 1
  fi
  if [ "$first" -gt 1 ] && sed -n "$((first - 1))p" "$post" | grep -aqE '^  =+'$'\r''?$'; then
    first=$((first - 1))
  fi
  tail -n +"$first" "$post" > "$log"
  rm -f "$post"
}
