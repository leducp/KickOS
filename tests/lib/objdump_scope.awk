# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The symbol-body scope of an `objdump -d` listing, as the rules a per-arch reader is appended
# to. Never run alone: awk takes this file, tests/lib/objdump_window.awk and the reader, in
# that order, with sym= set to the symbol wanted (gate.sh's scoped_body()).
#
# RULE ORDER IS THE MECHANISM AND THE THREE -f ARGUMENTS ARE NOT INTERCHANGEABLE: awk runs
# rules in the order it read them, so a reader placed first sees every other body's
# instructions.
#
# What a reader inherits here: `seen`, false when the listing carries no header for sym, which
# is how a renamed or inlined body is told from a clean one; and $0, only ever an instruction
# line of sym's own body. The window file inherited next names a region INSIDE that body and
# refuses a landmark the body does not carry.
/^[0-9a-f]+ <.*>:$/ {
    name = $2
    gsub(/[<>:]/, "", name)
    inbody = (name == sym)
    if (inbody) { seen = 1 }
    next
}
!inbody { next }
$0 !~ /^[ \t]*[0-9a-f]+:/ { next }
