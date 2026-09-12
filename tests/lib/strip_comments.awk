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
# Exits 2, and the residue must then not be read as clean, when
#   - a block comment is still open at EOF,
#   - a string or character literal is unclosed at the end of a joined line, or
#   - a raw string literal opens. Its delimiter carries its own quoting rules, and a raw
#     string is free to hold a lone quote, a comment opener and a comment closer as ordinary
#     content, so this scan cannot tell where one ends. It is refused rather than guessed at:
#     a wrong guess blanks the code after it and exits 0, which reads as a clean file.
function rawopen(line, i,    j, pre, c) {
    if (substr(line, i, 1) != "R" || substr(line, i + 1, 1) != "\"") { return 0 }
    # The R has to start a token, alone or behind an encoding prefix, or it is an identifier
    # character and the quote after it opens an ordinary literal.
    j = i - 1
    pre = ""
    while (j >= 1) {
        c = substr(line, j, 1)
        if (c !~ /[A-Za-z0-9_]/) { break }
        pre = c pre
        j--
    }
    if (pre == "" || pre == "L" || pre == "u" || pre == "U" || pre == "u8") { return 1 }
    return 0
}
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
        if (rawopen(line, i)) {
            printf("%s:%d: a raw string literal opens here and this scan cannot classify one\n",
                   FILENAME, first) > "/dev/stderr"
            bad = 1
            break
        }
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
