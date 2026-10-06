#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The ESP32-C6 reads its CPU clock into SystemCoreClock before Reset_Handler runs the
# constructors, which may read it. Read out of the linked image: in Reset_Handler's body, the
# call to mtime_rate_init stands ahead of the first instruction naming an init_array bound.
# Structural because nothing here can run a C6 image. It runs no image, so it carries the host
# label.
#
# usage: check_c6_clock_first.sh <elf> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_c6_clock_first.sh <elf> <objdump>"
elf="${1:?$_usage}"
objdump="${2:?$_usage}"

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# "<clock> <ctors>": the ordinal in Reset_Handler of the call that reads the clock and of the
# first instruction naming an init_array bound, 0 for one the body lacks. NOSYM for no body.
order() { # <listing>
    awk '
        /^[0-9a-f]+ <.*>:$/ { inbody = ($2 == "<Reset_Handler>:"); if (inbody) { seen = 1 }; next }
        !inbody || $0 !~ /^[ \t]*[0-9a-f]+:/ { next }
        {
            n++
            if (clock == 0 && $0 ~ /[ \t]jal[ \t][^<]*<[^>]*mtime_rate_init[^>]*>/) { clock = n }
            if (ctors == 0 && $0 ~ /<__init_array_(start|end)>/) { ctors = n }
        }
        END {
            if (!seen) { print "NOSYM"; exit }
            print (clock + 0) " " (ctors + 0)
        }' "$1"
}

cat > "$TMP/ctl_ok" <<'EOF'
4080a93c <Reset_Handler>:
4080a93c:	addi	sp,sp,-16
4080a940:	jal	4080a800 <_ZL15mtime_rate_initv>
4080a944:	auipc	s0,0x11
4080a948:	addi	s0,s0,-194 # 4081b8f0 <__init_array_end>
4080a94c:	jalr	a5
EOF
cat > "$TMP/ctl_late" <<'EOF'
4080a93c <Reset_Handler>:
4080a93c:	addi	sp,sp,-16
4080a944:	auipc	s0,0x11
4080a948:	addi	s0,s0,-194 # 4081b8f0 <__init_array_start>
4080a94c:	jalr	a5
4080a950:	jal	4080a800 <_ZL15mtime_rate_initv>
EOF
cat > "$TMP/ctl_none" <<'EOF'
4080a93c <Reset_Handler>:
4080a93c:	addi	sp,sp,-16
4080a948:	addi	s0,s0,-194 # 4081b8f0 <__init_array_end>
4080a94c:	jal	4080a7b8 <arch_init>
EOF

expect() { # <label> <listing> <wanted> <what a wrong answer means>
    _got="$(order "$2")"
    if [ "$_got" != "$3" ]; then
        fail "the reader answered [$_got] rather than [$3] for the planted $1, so $4"
    fi
}
expect "in-order body" "$TMP/ctl_ok" "2 4" "it cannot recognise the order this gate requires"
expect "late clock read" "$TMP/ctl_late" "5 3" \
    "a clock read moved behind the constructors would go unreported"
expect "body with no clock read" "$TMP/ctl_none" "0 2" \
    "a deleted or inlined clock read would read as a clean body"

tool_out "$TMP/dis" "^[0-9a-f]+ <Reset_Handler>:\$" "$objdump" -d --no-show-raw-insn "$elf"
got="$(order "$TMP/dis")"
[ "$got" != "NOSYM" ] || fail "no Reset_Handler body in $elf"
clock="${got%% *}"
ctors="${got##* }"
require_number "$clock" "the clock read's ordinal"
require_number "$ctors" "the constructor loop's ordinal"
[ "$clock" -ne 0 ] || fail "Reset_Handler in $elf makes no call to mtime_rate_init: it was
  renamed, inlined or moved, so the clock may be read after the constructors"
[ "$ctors" -ne 0 ] || fail "Reset_Handler in $elf names no init_array bound, so this gate has
  no constructor loop to order the clock read against"
[ "$clock" -lt "$ctors" ] || fail "Reset_Handler in $elf reads the CPU clock (instruction
  $clock) after it reaches the constructors (instruction $ctors), so a constructor sees
  SystemCoreClock at 0"
echo "PASS: $elf reads the CPU clock (instruction $clock) ahead of the constructors ($ctors)"
