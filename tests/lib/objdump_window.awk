# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# LEVEL TWO of an `objdump -d` reader: a WINDOW inside the symbol body, delimited by an opening
# and a closing LANDMARK. Level one is tests/lib/objdump_scope.awk, whose `seen` lets a reader
# refuse a symbol the image does not carry. Never run alone: gate.sh's scoped_body() hands awk
# the scope, then this file, then the reader, in that order.
#
# WHY IT IS SHARED. When a landmark is renamed, inlined or moved behind a forwarder the window
# never opens, every positional count inside it reads zero, and that is the SAME record as a
# body that carries the landmarks and does none of what the rule requires. A reader without
# this file reports the property broken, or passes, on a body it could not see. The three
# refusals below are UNKNOWN and never a verdict, and they are emitted here so no reader has to
# remember them.
#
# A LANDMARK NOT GIVEN IS AN ANCHOR, A LANDMARK NOT FOUND IS A REFUSAL, and collapsing the two
# is the bug. An empty `win_open` anchors the window at the body's first instruction, which is
# how a rule of the form "before the first X" is spelled; an empty `win_close` runs it to the
# body's last, which is how "from X onwards" is spelled. Both reference readers need one of
# those, in opposite directions, so the shared form carries both as configuration and refuses
# only a landmark it was told to find and did not.
#
# WHAT THE CALLER SETS, through -v:
#   win_open   ERE for the opening landmark, or "" to anchor at the body's first instruction.
#   win_close  ERE for the closing landmark, or "" to run to the body's last.
# EREs, matched against `win_text` below. An ERE handed to awk through -v must bracket a
# literal dot as [.] and never \., which gawk reads as any character; a landmark naming a
# mangled symbol has to bracket the [.] and [$] a clone suffix can carry.
#
# WHAT THE READER INHERITS, on every line and at END:
#   win_n        this instruction's ordinal in the body, 1-based; at END the body's count.
#   win_text     the instruction text the landmarks were matched against: address, trailing
#                comment and repeated whitespace removed.
#   win_open_n   ordinal of the opening landmark, 0 while unfound or unconfigured.
#   win_close_n  ordinal of the FIRST closing landmark past the opening one, 0 likewise.
#   win_pre      1 while the opening landmark has not been passed, its own line included.
#   win_in       1 when this instruction lies STRICTLY between the two landmarks. A landmark's
#                own line is never inside the window it delimits.
#   win_post     1 from the closing landmark's own line on.
# The three are exclusive and one of them is always set, so a reader sites an instruction by
# asking which rather than by comparing ordinals against a landmark it may not have reached
# yet: win_open_n is still 0 on every line ahead of the opening landmark.
#
# WHAT IT PRINTS, one record, after which nothing else runs:
#   NOSYM               the listing carries no body for the symbol.
#   NOINSN              the body decodes to no instruction.
#   NOBRACKET <n>       both landmarks were given and the body carries neither.
#   NOOPEN <n>          the opening landmark was given and the body carries none.
#   NOCLOSE <n>         the closing landmark was given and none stands past the opening one,
#                       so the window would silently run to the end of the body and count
#                       material the rule does not reach.
{
    win_n++
    win_text = $0
    sub(/^[^:]*:[ \t]*/, "", win_text)
    sub(/[ \t]*\/\/.*$/, "", win_text)
    sub(/[ \t]*#.*$/, "", win_text)
    sub(/[ \t]+$/, "", win_text)
    gsub(/[ \t]+/, " ", win_text)

    win_hit_open = (win_open != "" && win_text ~ win_open)
    win_hit_close = (win_close != "" && win_text ~ win_close)
    if (win_hit_open) { win_saw_open++ }
    if (win_hit_close) { win_saw_close++ }

    win_pre = 0
    win_in = 0
    win_post = 0
    if (win_open != "" && win_open_n == 0)
    {
        if (win_hit_open) { win_open_n = win_n }
        win_pre = 1
    }
    else if (win_close_n == 0)
    {
        if (win_hit_close) { win_close_n = win_n; win_post = 1 } else { win_in = 1 }
    }
    else
    {
        win_post = 1
    }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (win_n == 0) { print "NOINSN"; exit }
    if (win_open != "" && win_saw_open == 0)
    {
        # NOBRACKET only where BOTH were given, so the refusal can name both.
        if (win_close != "" && win_saw_close == 0) { print "NOBRACKET " win_n; exit }
        print "NOOPEN " win_n; exit
    }
    # Not win_saw_close: a closing landmark standing only AHEAD of the opening one leaves the
    # window open to the end of the body, which is the refusal that reads as a pass.
    if (win_close != "" && win_close_n == 0) { print "NOCLOSE " win_n; exit }
}
