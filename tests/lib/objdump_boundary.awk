# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Finds the lowest branch target inside `sym`'s own range that the objdump -d listing has no
# decoded instruction for. On a variable-width ISA, one byte of padding shifts every decode
# behind it out of sync until the stream happens to realign; a branch target is an instruction
# boundary by construction, so a target address with no decode there means the sweep desynced
# before it. gate.sh's synced_body() restarts the sweep from that address.
#
# Set through -v:
#   sym        only targets whose operand names this body count; a call or branch out of it
#              lands in a range this listing does not carry and would report as missing forever.
#   branch_re  ERE over the mnemonic. Empty disables detection (a fixed-width ISA cannot lose
#              sync). Must exclude pc-relative loads such as arm64 adrp or rv64 auipc: they
#              print the same <sym+0x..> operand shape but point at data, and splicing on one
#              corrupts a listing that was correct.
#   floor      optional, consider only addresses above this one. synced_body() passes the last
#              splice point so the loop terminates.
#
# Prints the lowest qualifying address, or nothing. Nothing also means a healthy listing, or a
# body with no branches; the two are not distinguished.
function below(x, y)
{
    return (length(x) < length(y) || (length(x) == length(y) && x < y))
}
/^[ \t]*[0-9a-f]+:/ {
    a = $0
    sub(/:.*$/, "", a)
    sub(/^[ \t]+/, "", a)
    have[a] = 1

    t = $0
    sub(/^[^:]*:[ \t]*/, "", t)
    sub(/[ \t]*#.*$/, "", t)
    m = t
    sub(/[ \t].*$/, "", m)
    if (branch_re == "" || m !~ branch_re) { next }
    # The trailing group only: a two-group operand is a literal-pool load, whose second group
    # is the pooled VALUE and never an instruction boundary.
    if (!match(t, /[0-9a-f]+ <[^<>()]+[+]0x[0-9a-f]+>$/)) { next }
    g = substr(t, RSTART, RLENGTH)
    addr = g
    sub(/ <.*$/, "", addr)
    name = g
    sub(/^[^<]*</, "", name)
    sub(/[+]0x[0-9a-f]+>$/, "", name)
    if (name != sym) { next }
    want[addr] = 1
}
END {
    lo = ""
    for (k in want)
    {
        if (k in have) { continue }
        if (floor != "" && !below(floor, k)) { continue }
        if (lo == "" || below(k, lo)) { lo = k }
    }
    if (lo != "") { print lo }
}
