# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The scanner behind tests/static/check_extern_c_linkage.sh. Run per file:
#   awk -v FNAME=<path> -f extern_c_linkage.awk <path>
#
# Prints one `<file>:<line>` per anonymous namespace opened while an `extern "C"` block is
# on the brace stack. Exits 2 with a REFUSE line on stderr when the file cannot be counted,
# so an unreadable file is never reported as clean.
#
# Its OWN file rather than inline in the shell script: the program needs both quote
# characters, and escaping them through a single-quoted shell heredoc is how a scanner
# silently stops matching.
#
# Character-level, because a brace inside a comment or a string literal shifts the nesting
# and every verdict after it. Each open brace is tagged from the code text since the last
# `;`, `{` or `}`:
#   ends in `extern "C"` / `extern "C++"` -> a language-linkage block
#   ends in `namespace`                   -> an ANONYMOUS namespace (a named one has its
#                                            name in that text, an alias ends in `;`)

BEGIN { depth = 0; ext = 0; buf = ""; state = 0; refuse = "" }

# state: 0 code, 1 line comment, 2 block comment, 3 string, 4 char literal.
# A block comment and a string both survive across lines, so state is NOT reset per line.
{
    # A preprocessor directive contributes no braces. Safe only because END asserts the
    # file still balances; a macro body carrying an unmatched brace is refused, not guessed.
    # A directive cannot begin inside a comment or a string, and a line-continued one is
    # covered because its tail carries no brace either.
    if (state == 0 && $0 ~ /^[ \t]*#/) { next }

    n = length($0)
    for (i = 1; i <= n; i++) {
        c = substr($0, i, 1)

        if (state == 1) { continue }

        if (state == 2) {
            if (c == "*" && substr($0, i + 1, 1) == "/") { state = 0; i++ }
            continue
        }

        if (state == 3 || state == 4) {
            # A string's BODY is appended, not skipped: dropping it turns `extern "C"` into
            # `extern ""` and the tag regex below stops matching, which reads as clean.
            # A brace inside a string cannot be mistaken for a real one; it is consumed
            # here, in state 3, and never reaches the `{` branch.
            if (c == "\\") { buf = buf c substr($0, i + 1, 1); i++; continue }
            if (state == 3 && c == "\"") { buf = buf c; state = 0; continue }
            if (state == 4 && c == "'") { state = 0; continue }
            if (state == 3) { buf = buf c }
            continue
        }

        # --- state 0: code ---
        if (c == "/" && substr($0, i + 1, 1) == "/") { state = 1; continue }
        if (c == "/" && substr($0, i + 1, 1) == "*") { state = 2; i++; continue }

        if (c == "\"") {
            # A raw string literal makes the escape rules above wrong, so refuse the file.
            if (i > 1 && substr($0, i - 1, 1) == "R") {
                refuse = "raw string literal at line " FNR
                exit 2
            }
            buf = buf c          # the quotes stay in buf: `extern "C"` must still match
            state = 3
            continue
        }
        if (c == "'") { state = 4; continue }

        if (c == "{") {
            tag = "block"
            if (buf ~ /extern[ \t]*"C(\+\+)?"[ \t]*$/) {
                tag = "extern"
            } else if (buf ~ /(^|[^A-Za-z0-9_])namespace[ \t]*$/) {
                tag = "anon"
            }
            if (tag == "anon" && ext > 0) { print FNAME ":" FNR }
            depth++
            stack[depth] = tag
            if (tag == "extern") { ext++ }
            buf = ""
            continue
        }
        if (c == "}") {
            if (depth > 0) {
                if (stack[depth] == "extern") { ext-- }
                depth--
            } else {
                refuse = "unmatched closing brace at line " FNR
                exit 2
            }
            buf = ""
            continue
        }
        if (c == ";") { buf = ""; continue }

        buf = buf c
    }

    # End of line. A line comment ends here; a block comment and a string do not.
    if (state == 1) { state = 0 }
    buf = buf " "                # a newline is whitespace to the two tag regexes above
}

END {
    if (refuse != "") { print "REFUSE " FNAME ": " refuse > "/dev/stderr"; exit 2 }
    if (depth != 0) {
        print "REFUSE " FNAME ": braces do not balance (final depth " depth ")" > "/dev/stderr"
        exit 2
    }
}
