# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Emits the body of one function definition, as "<line>:<text>" records keyed on the line
# numbers of the ORIGINAL file, so a caller's finding cites a real line.
#
#   awk -v FN=<name> -f tests/static/fn_body.awk <file>
#
# Input is expected to be the residue of tests/lib/strip_comments.awk, which is line-for-line
# with the source: a caller that hands the raw file instead gets comment text in the records
# and every claim below can then be satisfied by prose.
#
# The body starts at the first `{` at or after the line whose text holds `<FN>(` outside a
# call position (column 1 or preceded only by a return type), and ends where the brace depth
# returns to zero. Preprocessor lines are emitted unchanged: the callers key on `#if` arms.
#
# Exits 2, and no record may then be read as an absence, when the name is not found or its
# braces never balance.

BEGIN {
    if (FN == "") {
        printf("fn_body.awk: -v FN=<name> is required\n") > "/dev/stderr"
        exit 2
    }
    started = 0
    depth = 0
    found = 0
}
{
    line = $0
    if (!started) {
        # A DEFINITION and not a call or a declaration. The column-0 test is what makes it
        # sound: a multiline CALL has no trailing `;` on its FIRST line either (sched.cc
        # spreads arch_ctx_redirect over three), so "no semicolon" alone accepts calls.
        if (index(line, FN "(") > 0 \
            && line ~ ("^[A-Za-z_][A-Za-z_0-9 *&:<>,]*" FN "[[:space:]]*\\(") \
            && line !~ /;[[:space:]]*$/) {
            found = 1
        }
        if (!found) {
            next
        }
    }
    n = length(line)
    for (i = 1; i <= n; i++) {
        c = substr(line, i, 1)
        if (c == "{") {
            depth++
            started = 1
        }
        else if (c == "}") {
            depth--
        }
    }
    if (started) {
        printf("%d:%s\n", FNR, line)
        if (depth <= 0) {
            exit 0
        }
    }
}
END {
    if (!found) {
        printf("fn_body.awk: %s: no definition of %s\n", FILENAME, FN) > "/dev/stderr"
        exit 2
    }
    if (depth > 0) {
        printf("fn_body.awk: %s: %s never closes its braces\n", FILENAME, FN) > "/dev/stderr"
        exit 2
    }
}
