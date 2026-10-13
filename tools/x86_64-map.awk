# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The x86_64 image's link map, from the pre-link's map and the PE link's
# (tools/x86_64-link.sh): the pre-link's head, which records each archive member it extracted
# and every section it collected, then the PE layout with the pre-linked object's LOAD line
# replaced by the inputs it holds and each of its sections replaced by the input sections the
# pre-link put there, at their final addresses.
#
#   awk -v pre=<pre-linked object> -f x86_64-map.awk <pre-link map> <PE map>
#
# Portable awk: no strtonum, and the 64-bit addresses are carried as doubles, exact below 2^53.

function h2n(s,    i, d, v) {
    sub(/^0[xX]/, "", s)
    v = 0
    for (i = 1; i <= length(s); i++) {
        d = index("0123456789abcdef", tolower(substr(s, i, 1))) - 1
        if (d < 0) {
            return -1
        }
        v = v * 16 + d
    }
    return v
}

function n2h(v,    s, d) {
    s = ""
    while (length(s) < 16) {
        d = v % 16
        s = substr("0123456789abcdef", d + 1, 1) s
        v = (v - d) / 16
    }
    return "0x" s
}

# The line with its first address moved from the pre-linked section's base to `at`.
function moved(line, sec, at,    i, n, f, old) {
    n = split(line, f, " ")
    for (i = 1; i <= n; i++) {
        if (f[i] ~ /^0x[0-9a-fA-F]+$/) {
            old = f[i]
            sub(old, n2h(at + h2n(old) - base[sec]), line)
            return line
        }
    }
    return line
}

function emit(sec, at,    i) {
    if (!(sec in count)) {
        printf "x86_64-map: %s places a section the pre-link map does not hold: %s\n", \
            FILENAME, sec > "/dev/stderr"
        failed = 1
        return
    }
    for (i = 1; i <= count[sec]; i++) {
        print moved(rec[sec, i], sec, at)
    }
}

# --- the pre-link's map ----------------------------------------------------------------------
FNR == NR {
    if ($0 == "Linker script and memory map") {
        inbody = 1
        next
    }
    if (!inbody) {
        print
        next
    }
    if ($0 ~ /^LOAD /) {
        loads = loads $0 "\n"
        next
    }
    if ($0 ~ /^\.[^ ]/) {
        sec = $1
        count[sec] = 0
        if (NF >= 3 && $2 ~ /^0x/) {
            base[sec] = h2n($2)
        } else {
            want_base = 1
        }
        next
    }
    if (want_base && $0 ~ /^ +0x/) {
        base[sec] = h2n($1)
        want_base = 0
        next
    }
    if (sec == "") {
        next
    }
    # Input sections, their continuation and symbol lines, and fill; not the script's own.
    if ($0 ~ /^ \.[^ ]/ || $0 ~ /^ COMMON/ || $0 ~ /^ \*fill\*/ || $0 ~ /^ +0x[0-9a-fA-F]+ /) {
        if ($0 ~ /^ +0x/ && $0 ~ / = |ASSERT|PROVIDE|ALIGN|LONG|SORT/) {
            next
        }
        rec[sec, ++count[sec]] = $0
    }
    next
}

# --- the PE link's map -----------------------------------------------------------------------
$0 == "Linker script and memory map" {
    body = 1
    print
    next
}
!body {
    next
}
$0 == "LOAD " pre {
    printf "%s", loads
    next
}
held != "" {
    if ($NF == pre && $1 ~ /^0x/) {
        emit(held_sec, h2n($1))
        held = ""
        skipping = 1
        next
    }
    print held
    held = ""
}
$0 ~ /^ \.[^ ]+$/ {
    held = $0
    held_sec = $1
    next
}
$NF == pre && $0 ~ /^ \.[^ ]/ && $2 ~ /^0x/ {
    emit($1, h2n($2))
    skipping = 1
    next
}
skipping && NF == 2 && $1 ~ /^0x/ && $2 !~ /^0x/ {
    next
}
{
    skipping = 0
    print
}
END {
    if (held != "") {
        print held
    }
    if (failed) {
        exit 1
    }
}
