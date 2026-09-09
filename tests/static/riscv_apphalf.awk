# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The three readings tests/static/check_riscv_kernel_apphalf.sh takes out of readelf's output.
# PHASE selects one, and every pattern a reading is made of comes from the caller, so the
# self-test and the real scan cannot drift apart.
#
#   PHASE=bounds  -v LO=<symbol> -v HI=<symbol> -v WIDTH=<n>
#       reads `readelf -sW` and prints ONE record: `OK <lo> <hi> <n>` with both bound values
#       and the symbol count, `BOUNDS` when either bound symbol is absent, or `WIDTH <value>`
#       when the value column is not WIDTH characters wide. A nonzero exit from this program
#       therefore means the TOOL died and never that the input was refused.
#
#   PHASE=names   -v LO=<lo value> -v HI=<hi value> -v BIND=<ere> -v CLOSED=<0|1>
#       reads the same output and prints the NAME of every symbol whose binding BIND matches
#       and whose value falls in the window. CLOSED=1 includes HI itself.
#
#   PHASE=rel     -v AR=<archive> -v SECT=<ere> -v TYPE=<ere> -v SKIP=<ere> -v DATA=<ere>
#       reads `readelf -rW` over an archive and prints one TAB-separated record per
#       relocation sitting in a section SECT matches:
#           <kind>\t<symbol>\t<type>\t<section>\t<where>
#       kind is `data` when DATA matches the type and `insn` otherwise, so a type nobody has
#       seen yet counts as an instruction and over-refuses.
#
# VALUES ARE COMPARED AS THE FIXED-WIDTH LOWERCASE HEX readelf PRINTS, never converted, so
# WIDTH is what makes the comparison meaningful: at one width the string order and the numeric
# order agree, and at a different width the same comparison is silent nonsense.
#
# An ERE reaching here through -v must not spell a literal dot as a backslash escape: awk
# processes escape sequences in a -v assignment, and `\.` arrives as a plain `.` that matches
# any character. Bracket it as `[.]` instead.

PHASE == "bounds" && NF >= 8 && $1 ~ /^[0-9]+:$/ {
    if (length($2) != WIDTH) {
        width_bad = $2
        exit 0
    }
    if ($8 == LO) { lo = $2 }
    if ($8 == HI) { hi = $2 }
    n++
}

PHASE == "names" && NF >= 8 && $1 ~ /^[0-9]+:$/ && $5 ~ BIND {
    inwin = 0
    if ($2 >= LO && $2 < HI) { inwin = 1 }
    if (CLOSED == 1 && $2 == HI) { inwin = 1 }
    if (inwin == 1) { print $8 }
}

PHASE == "rel" && /^File:/ {
    member = $2
    next
}
PHASE == "rel" && /^Relocation section/ {
    sec = $3
    gsub(/'/, "", sec)
    intext = (sec ~ SECT)
    next
}
PHASE == "rel" && intext && NF >= 5 && $3 ~ TYPE {
    # A relaxation annotation names no symbol, so its own name column holds the addend sign.
    if ($3 ~ SKIP) { next }
    where = member
    if (where == "") { where = AR }
    kind = "insn"
    if ($3 ~ DATA) { kind = "data" }
    printf "%s\t%s\t%s\t%s\t%s\n", kind, $5, $3, sec, where
}

END {
    if (PHASE != "bounds") { exit 0 }
    if (width_bad != "") {
        print "WIDTH " width_bad
        exit 0
    }
    if (lo == "" || hi == "") {
        print "BOUNDS"
        exit 0
    }
    print "OK " lo " " hi " " n + 0
}
