#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot one fault-class image under qemu-system-x86_64 with UEFI firmware and assert what the
# descriptor tables and the fault report produced on COM1.
#
#   tools/run-qemu-x86_64-x2.sh <class> <application.efi> [workdir]
#
# <class> is one of none ud pf pfw pfx gp sel de soft df, and it selects the assertions. `none` is the
# NEGATIVE CONTROL: it asserts the tables loaded and that no report was printed.
#
# The machine, the EFI system partition and the serial capture come from
# tools/run-qemu-x86_64-common.sh.
#
# Environment:
#   KICKOS_X2_TOKEN     the token the report must carry (default below). Held against the IMAGE,
#                       never scraped from the source.
#   KICKOS_X2_FIRMWARE  `pflash` (split OVMF_CODE_4M plus OVMF_VARS_4M, the default) or
#                       `bios` (the combined /usr/share/ovmf/OVMF.fd through -bios).
#   KICKOS_X2_MACHINE   qemu machine type, default q35.
#   KICKOS_X2_TIMEOUT   seconds, default 60.
#   KICKOS_X86_64_CPU   a -cpu model, default none. Shared by all five witnesses.
#
# POSIX sh (dash-clean).

set -u

KOS_TOOLS=$(cd "$(dirname "$0")" && pwd); . "$KOS_TOOLS/run-qemu-x86_64-common.sh"

KOS_STEP=X2
KOS_TOKEN="${KICKOS_X2_TOKEN:-KICKOS-X2 5be09c14 x86_64/q35 descriptors}"
KOS_FIRMWARE="${KICKOS_X2_FIRMWARE:-pflash}"
KOS_MACHINE="${KICKOS_X2_MACHINE:-q35}"
KOS_TIMEOUT="${KICKOS_X2_TIMEOUT:-60}"
# The image HALTS rather than exiting, so nothing ends the emulator on its own.
KOS_END=halt

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    fail "usage: run-qemu-x86_64-x2.sh <class> <application.efi> [workdir]"
fi
CLASS="$1"

case "$CLASS" in
    none|ud|pf|pfw|pfx|gp|sel|de|soft|df) ;;
    *) fail "unknown class '$CLASS' (none ud pf pfw pfx gp sel de soft df)" ;;
esac

KOS_WORK_LEAF="x2run/$CLASS"
kos_boot "$2" "${3:-}"

HEX='0x[0-9a-f]\{16\}'

refuse() { # <what> <bre>
    grep -q "$2" "$PLAIN" && fail "$1: a line matching /$2/ is in $PLAIN"
    return 0
}
# The value of <key>= on the FIRST line that carries <anchor>, as a 0x... hex word.
field() { # <anchor> <key>
    grep "$1" "$PLAIN" | head -1 | sed -n "s/.* $2=\($HEX\).*/\1/p"
}
same() { # <what> <a> <b>
    [ -n "$2" ] || fail "$1: the first value is empty"
    [ -n "$3" ] || fail "$1: the second value is empty"
    [ "$2" = "$3" ] || fail "$1: $2 is not $3"
}

# --- every class: the handover still happens, and the tables are this image's ------------
need "the entry never reached its descriptor load" "^  $TOK descriptors loaded\$"
need "no gdt readback line" "^  gdt=$HEX gdtr=$HEX limit=$HEX\$"
need "no idt readback line" "^  idt=$HEX idtr=$HEX limit=$HEX\$"
need "no tss readback line" "^  tss=$HEX tr=$HEX rsp0=$HEX ist1=$HEX\$"
need "no segment readback line" "^  cs=$HEX ss=$HEX\$"

same "the loaded gdt is not this image's table" "$(field '^  gdt=' gdt)" "$(field '^  gdt=' gdtr)"
same "the loaded idt is not this image's table" "$(field '^  idt=' idt)" "$(field '^  idt=' idtr)"
# 7 entries of 8 bytes, and 256 gates of 16.
same "the gdt limit is not 7 entries" "$(field '^  gdt=' limit)" "0x0000000000000037"
same "the idt limit is not 256 gates" "$(field '^  idt=' limit)" "0x0000000000000fff"
same "cs is not the kernel code selector" "$(field '^  cs=' cs)" "0x0000000000000008"
same "ss is not the kernel data selector" "$(field '^  cs=' ss)" "0x0000000000000010"
same "the task register does not name the tss descriptor" "$(field '^  tss=' tr)" \
     "0x0000000000000028"
[ "$(field '^  tss=' ist1)" != "0x0000000000000000" ] \
    || fail "the double-fault stack slot is zero, so a double fault would triple-fault"

if [ "$CLASS" = "none" ]; then
    need "the entry did not reach its halt" " landed, halting\$"
    refuse "a report was printed by an image that takes no fault" "=== X86_64 EXCEPTION"
    echo "PASS: class none, tables loaded and no report printed"
    echo "      serial: $PLAIN"
    exit 0
fi

# --- the fault classes -------------------------------------------------------------------
need "no banner" "^=== X86_64 EXCEPTION ===\$"
need "the report carries no token, so it may be firmware's" "^  token=$TOK\$"
need "the report did not reach its halt" "^  $TOK halting\$"
need "no probe expectation line" "^  $TOK probe=$CLASS expect_vector=[0-9][0-9]* expect_rip=$HEX\$"
need "no ring line" "^  ring=[0-9] if=[0-9]\$"
need "the report did not run at ring 0" "^  ring=0 if=0\$"

exp_vec="$(grep "probe=$CLASS" "$PLAIN" | head -1 | sed -n 's/.* expect_vector=\([0-9]*\).*/\1/p')"
exp_rip="$(grep "probe=$CLASS" "$PLAIN" | head -1 | sed -n "s/.* expect_rip=\($HEX\).*/\1/p")"
got_vec="$(grep '^  vector=' "$PLAIN" | head -1 | sed -n 's/^  vector=\([0-9]*\) .*/\1/p')"
got_rip="$(field '^  RIP=' RIP)"

same "the reported vector is not the one the probe armed" "$got_vec" "$exp_vec"

case "$CLASS" in
    ud)
        need "wrong decode for vector 6" "^  vector=6 (invalid opcode)\$"
        need "an error code was reported where the vector pushes none" \
             "^  error=0x0000000000000000\$"
        same "the reported instruction pointer is not the faulting instruction" \
             "$got_rip" "$exp_rip"
        need "the report did not run on the interrupted stack" "^  stack=interrupted\$"
        refuse "a page-fault decode on a vector that pushes no address" "^  pf:"
        refuse "a selector decode on a vector that pushes no selector" "^  sel:"
        ;;
    pf)
        need "wrong decode for vector 14" "^  vector=14 (page fault)\$"
        need "no page-fault decode line" \
             "^  pf: present=0 write=0 user=0 rsvd=0 ifetch=0 pkey=0 shadow=0\$"
        same "the reported instruction pointer is not the faulting instruction" \
             "$got_rip" "$exp_rip"
        exp_cr2="$(grep 'expect_cr2=' "$PLAIN" | head -1 | sed -n "s/.*expect_cr2=\($HEX\).*/\1/p")"
        same "the faulting address is not the one the probe touched" \
             "$(field '^  CR2=' CR2)" "$exp_cr2"
        refuse "a selector decode on a page fault" "^  sel:"
        ;;
    pfw)
        need "wrong decode for vector 14" "^  vector=14 (page fault)\$"
        need "no page-fault decode line, or the write bit is not where it belongs" \
             "^  pf: present=0 write=1 user=0 rsvd=0 ifetch=0 pkey=0 shadow=0\$"
        same "the reported instruction pointer is not the faulting instruction" \
             "$got_rip" "$exp_rip"
        exp_cr2="$(grep 'expect_cr2=' "$PLAIN" | head -1 | sed -n "s/.*expect_cr2=\($HEX\).*/\1/p")"
        same "the faulting address is not the one the probe touched" \
             "$(field '^  CR2=' CR2)" "$exp_cr2"
        ;;
    pfx)
        need "wrong decode for vector 14" "^  vector=14 (page fault)\$"
        need "no page-fault decode line, or the fetch bit is not where it belongs" \
             "^  pf: present=0 write=0 user=0 rsvd=0 ifetch=1 pkey=0 shadow=0\$"
        exp_cr2="$(grep 'expect_cr2=' "$PLAIN" | head -1 | sed -n "s/.*expect_cr2=\($HEX\).*/\1/p")"
        same "the faulting address is not the one control was transferred to" \
             "$(field '^  CR2=' CR2)" "$exp_cr2"
        # A fetch fault reports the target as BOTH the address and the instruction pointer,
        # so the two independent reads of the frame have to agree.
        same "the instruction pointer is not the address that could not be fetched" \
             "$got_rip" "$exp_cr2"
        ;;
    gp)
        need "wrong decode for vector 13" "^  vector=13 (general protection)\$"
        need "no selector decode line" "^  sel: ext=0 tbl=gdt index=0\$"
        need "a selector was reported where the fault names none" \
             "^  error=0x0000000000000000\$"
        same "the reported instruction pointer is not the faulting instruction" \
             "$got_rip" "$exp_rip"
        refuse "a page-fault decode on a general-protection fault" "^  pf:"
        ;;
    sel)
        need "wrong decode for vector 13" "^  vector=13 (general protection)\$"
        # Index 8 in the table this image built, which holds 7 entries.
        need "no selector decode line, or the index is not where it belongs" \
             "^  sel: ext=0 tbl=gdt index=8\$"
        exp_err="$(grep 'expect_error=' "$PLAIN" | head -1 | sed -n "s/.*expect_error=\($HEX\).*/\1/p")"
        same "the error code is not the selector the probe loaded" \
             "$(field '^  error=' error)" "$exp_err"
        same "the reported instruction pointer is not the faulting instruction" \
             "$got_rip" "$exp_rip"
        ;;
    de)
        need "wrong decode for vector 0" "^  vector=0 (divide error)\$"
        same "the reported instruction pointer is not the faulting instruction" \
             "$got_rip" "$exp_rip"
        ;;
    soft)
        need "wrong decode for a vector above the defined ones" \
             "^  vector=64 (unexpected interrupt vector)\$"
        same "the reported instruction pointer is not the instruction after the int" \
             "$got_rip" "$exp_rip"
        ;;
    df)
        need "wrong decode for vector 8" "^  vector=8 (double fault)\$"
        need "the double-fault report did not run on the interrupt-stack-table stack" \
             "^  stack=ist1\$"
        need "a double fault reported a non-zero error code" "^  error=0x0000000000000000\$"
        # An interrupt-stack-table delivery loads a NULL stack selector (Intel SDM Vol 3),
        # so the report's own readback says the stack was switched and not merely reused.
        rep_ss="$(grep '^  cs=' "$PLAIN" | tail -1 | sed -n "s/.* ss=\($HEX\).*/\1/p")"
        same "the stack selector was not nulled, so no stack switch happened" \
             "$rep_ss" "0x0000000000000000"
        ;;
esac

echo "PASS: class $CLASS, vector $got_vec reported and decoded through this image's tables"
echo "      serial: $PLAIN"
