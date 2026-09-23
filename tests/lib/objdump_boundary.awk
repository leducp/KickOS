# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# THE INSTRUCTION BOUNDARY A BODY LISTING NAMES AND DOES NOT CARRY. An `objdump -d` sweep
# decodes forward from wherever it last stopped, so on a variable-width ISA one byte of padding
# between two instructions shifts every decode behind it until the stream happens to realign.
# What comes out is not a refusal and not a decode failure: it is a run of instructions that
# were never in the image, printed in the shape of ones that were. A positional reader counting
# a call across that run reports a body that does not make it.
#
# A BRANCH TARGET IS AN INSTRUCTION BOUNDARY BY CONSTRUCTION. So a target this body names in
# its own range, at an address the listing carries no decoded instruction for, is proof the
# decode went wrong somewhere ahead of it, and a second sweep STARTED at that address is right
# from there. This file names the lowest such address; gate.sh's synced_body() splices.
#
# WHAT THE CALLER SETS, through -v:
#   sym         the body's symbol. A target is considered only where the operand names THIS
#               body, because a call or a branch out of it lands in a range this listing does
#               not carry and would be reported missing forever.
#   branch_re   ERE over the MNEMONIC, and the caller owns it because only the caller knows
#               which of this arch's mnemonics carry a target. A pc-relative LOAD prints its
#               operand in exactly the same shape and points at DATA, so a detector taking
#               every <sym+0x..> demands a resync on every arm64 adrp and every rv64 auipc
#               pair, and splicing on one of those corrupts a listing that was correct.
#               Empty disables the detector, which is how a fixed-width ISA spells "a sweep
#               here cannot lose sync".
#   floor       optional: consider only addresses ABOVE this one. synced_body() passes the
#               address it spliced at last, which is what makes the loop terminate.
#
# WHAT IT PRINTS: the lowest qualifying address, or nothing at all when the listing carries
# every boundary it names. Nothing is the answer for a healthy listing and for a body with no
# branches, and the two are not distinguished here.
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
