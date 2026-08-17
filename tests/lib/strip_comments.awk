# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Emits each input line with comments, string literals and character literals blanked out,
# one output line per input line, so a caller's finding cites the real line.
#
#   awk -f tests/lib/strip_comments.awk <file>
#
# A line ending in a backslash is JOINED to the next before anything is scanned, the way the
# compiler splices it, so a continued literal or macro is one logical line here too. The
# residue of a joined line prints on the FIRST of the lines it came from and each further
# line prints empty, which is what keeps the line count equal to the input's.
#
# Exits 2, and the residue must then not be read as clean, when either
#   - a block comment is still open at EOF, or
#   - a string or character literal is unclosed at the end of a joined line.
function scan(line, first, blanks,    out, i, n, c, d, q, e, closed, kind) {
    out = ""
    i = 1
    n = length(line)
    while (i <= n) {
        c = substr(line, i, 1)
        d = substr(line, i, 2)
        if (inblk) {
            if (d == "*/") { inblk = 0; i += 2; continue }
            i++
            continue
        }
        if (d == "/*") { inblk = 1; blkline = first; i += 2; continue }
        if (d == "//") { break }
        if (c == "\"" || c == "'") {
            q = c
            i++
            closed = 0
            while (i <= n) {
                e = substr(line, i, 1)
                if (e == "\\") { i += 2; continue }
                if (e == q) { i++; closed = 1; break }
                i++
            }
            if (!closed) {
                kind = "character"
                if (q == "\"") { kind = "string" }
                printf("%s:%d: a %s literal opens here and is not closed on this line\n",
                       FILENAME, first, kind) > "/dev/stderr"
                bad = 1
                break
            }
            continue
        }
        out = out c
        i++
    }
    print out
    while (blanks > 0) { print ""; blanks-- }
}
BEGIN { inblk = 0; blkline = 0; pending = 0; held = 0 }
{
    if (pending) {
        buf = buf $0
        held++
    } else {
        buf = $0
        first = FNR
        held = 0
    }
    if (buf ~ /\\$/) {
        buf = substr(buf, 1, length(buf) - 1)
        pending = 1
        next
    }
    pending = 0
    scan(buf, first, held)
}
END {
    # A backslash on the LAST line splices nothing, so what was held still has to be scanned.
    if (pending) { scan(buf, first, held) }
    if (inblk) {
        printf("%s:%d: a block comment opens here and is never closed\n", FILENAME, blkline) > "/dev/stderr"
        bad = 1
    }
    if (bad) { exit 2 }
}
