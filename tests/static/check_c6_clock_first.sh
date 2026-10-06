#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An ESP32-C6 constructor may read SystemCoreClock, so the image holds the CPU clock there before
# Reset_Handler runs the constructors. Read out of the linked image. On the HP core (hp), which
# reads its clock off the PCR registers, the call to mtime_rate_init in Reset_Handler's body stands
# ahead of the first instruction naming an init_array bound. On the LP core (lp), whose clock is
# a link-time constant, SystemCoreClock is a nonzero word of the data the image loads.
# Structural because nothing here can run a C6 image. It runs no image, so it carries the host
# label.
#
# usage: check_c6_clock_first.sh <elf> <objdump> <hp|lp>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_c6_clock_first.sh <elf> <objdump> <hp|lp>"
elf="${1:?$_usage}"
objdump="${2:?$_usage}"
core="${3:?$_usage}"
case "$core" in
    hp | lp) ;;
    *) fail "$_usage" ;;
esac

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

# "<section> <address>" of SystemCoreClock in an `objdump -t` listing, NOSYM for none.
clock_datum() { # <listing>
    awk '$NF == "SystemCoreClock" && $(NF - 2) ~ /^\./ { print $(NF - 2), $1; found = 1; exit }
         END { if (!found) { print "NOSYM" } }' "$1"
}

# The first data word of an `objdump -s` dump, empty for a dump holding none.
first_word() { # <dump>
    awk '/^ [0-9a-f]+ [0-9a-f]+/ { print $2; exit }' "$1"
}

cat > "$TMP/ctl_sym_data" <<'EOF'
40858a70 l     O .data	00000004 g_other
40858a74 g     O .data	00000004 SystemCoreClock
EOF
cat > "$TMP/ctl_sym_bss" <<'EOF'
4085d000 g     O .sbss	00000004 SystemCoreClock
EOF
cat > "$TMP/ctl_sym_none" <<'EOF'
40858a70 l     O .data	00000004 g_other
EOF
cat > "$TMP/ctl_dump_set" <<'EOF'

Contents of section .data:
 40858a74 002d3101                             .-1.
EOF
cat > "$TMP/ctl_dump_zero" <<'EOF'

Contents of section .data:
 40858a74 00000000                             ....
EOF

expect_out() { # <label> <answer> <wanted> <what a wrong answer means>
    if [ "$2" != "$3" ]; then
        fail "the reader answered [$2] rather than [$3] for the planted $1, so $4"
    fi
}
expect_out "symbol in .data" "$(clock_datum "$TMP/ctl_sym_data")" ".data 40858a74" \
    "it cannot find the clock word this gate reads"
expect_out "symbol in .sbss" "$(clock_datum "$TMP/ctl_sym_bss")" ".sbss 4085d000" \
    "a clock word the image does not load would go unreported"
expect_out "table with no clock" "$(clock_datum "$TMP/ctl_sym_none")" "NOSYM" \
    "a renamed clock word would read as a found one"
expect_out "set word" "$(first_word "$TMP/ctl_dump_set")" "002d3101" \
    "it cannot read the clock's link-time value"
expect_out "zero word" "$(first_word "$TMP/ctl_dump_zero")" "00000000" \
    "a clock left at zero would go unreported"

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

if [ "$core" = lp ]; then
    tool_out "$TMP/sym" "^SYMBOL TABLE:" "$objdump" -t "$elf"
    datum="$(clock_datum "$TMP/sym")"
    [ "$datum" != "NOSYM" ] || fail "no SystemCoreClock in $elf"
    section="${datum%% *}"
    addr="${datum##* }"
    case "$section" in
        .data | .sdata) ;;
        *) fail "SystemCoreClock in $elf sits in $section, which the image does not load, so a
  constructor reads the LP core's clock at 0" ;;
    esac
    tool_out "$TMP/dump" "^Contents of section " "$objdump" -s -j "$section" \
        --start-address="0x$addr" --stop-address="$(printf '0x%x' "$((0x$addr + 4))")" "$elf"
    word="$(first_word "$TMP/dump")"
    printf '%s\n' "$word" | grep -q '[1-9a-f]' || fail "SystemCoreClock in $elf loads as
  [$word], so a constructor reads the LP core's clock at 0"
    echo "PASS: $elf loads the LP core's clock as a link-time constant ($word in $section)"
    exit 0
fi

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
