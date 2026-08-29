# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Reports every ` -- ` that check_dash_punct.sh reads as punctuation, one finding per line:
#
#   awk -v DASH=<ere> -v SEP=<ere> -v RUN=<ere> -v TICK=<ere> -v HASH=<ere> \
#       [-v HEREDOC=1] -f dash_punct.awk <file>
#
# Every ERE comes from the caller so the self-test and the corpus scan cannot drift apart.
# A finding is printed as `<file>:<line>:<the original line>`.
#
# HEREDOC=1 blanks every heredoc BODY, terminator line included, and is set for shell only.
# A gate's self-test corpus lives in one and is data: it has to be free to plant the very
# spellings the gate refuses. `<<<` is a here-STRING, not a heredoc, and does not arm one.
#
# Exits 2, and the output must then not be read as clean, when a heredoc body is still open
# at EOF: everything after it was skipped, so the rest of the file is UNKNOWN.

# The matched text, replaced by as many spaces, so a column survives and no two blanked
# runs can abut into a new ` -- `.
function erase(s, re,    out, k, pad) {
    out = s
    while (match(out, re) > 0) {
        # A zero-length match would rebuild the same string forever.
        if (RLENGTH <= 0) { break }
        pad = ""
        for (k = RLENGTH; k > 0; k--) { pad = pad " " }
        out = substr(out, 1, RSTART - 1) pad substr(out, RSTART + RLENGTH)
    }
    return out
}

BEGIN { inhere = 0; hereterm = ""; hereline = 0 }
{
    if (inhere) {
        t = $0
        sub(/^[ \t]+/, "", t)
        if (t == hereterm) { inhere = 0 }
        next
    }

    work = erase($0, TICK)

    # SEP reads shell, and a comment is not shell: prose spells a command position inside one
    # and forges an option terminator there. Erased ahead of the first comment opener only.
    cut = length(work) + 1
    if (match($0, HASH) > 0) { cut = RSTART }
    work = erase(substr(work, 1, cut - 1), SEP) substr(work, cut)

    # A section banner opens with a run of three or more dashes; the run that CLOSES it is
    # often only two. Dropped at end of line only, so prose earlier on a banner line still
    # reports.
    if (work ~ RUN) { sub(/[ \t]--[ \t]*$/, "", work) }

    if (work ~ DASH) { printf("%s:%d:%s\n", FILENAME, FNR, $0) }

    # `<<<` is a here-STRING and arms nothing. The word must also be FOLLOWED by something
    # that can end a redirection: `printf 'cat <<EOF\\n...'` names the operator inside a
    # literal, and arming on it swallows the rest of the file.
    if (HEREDOC && match($0, /(^|[^<])<<-?[ \t]*('[A-Za-z_][A-Za-z0-9_]*'|"[A-Za-z_][A-Za-z0-9_]*"|[A-Za-z_][A-Za-z0-9_]*)([ \t;|&)<>]|$)/) > 0) {
        t = substr($0, RSTART, RLENGTH)
        sub(/^.*<<-?[ \t]*/, "", t)
        gsub(/['"]/, "", t)
        sub(/[^A-Za-z0-9_].*$/, "", t)
        if (t != "") { inhere = 1; hereterm = t; hereline = FNR }
    }
}
END {
    if (inhere) {
        printf("%s:%d: a heredoc body opens here and its terminator %s never appears, so every line below it went unscanned\n",
               FILENAME, hereline, hereterm) > "/dev/stderr"
        exit 2
    }
}
