# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The subscript scanner behind tests/static/check_kernel_core_index.sh. Input must be
# ALREADY-STRIPPED, one file at a time; F carries the real path:
#   awk -v F=<real-path> -f kernel_core_index.awk <stripped-file>
#
# Records go to STDOUT, one per line, for the caller to APPEND to its own files:
#   FINDING <file>:<line>: <text>
#   REFUSED <file>:<line>: <prose>
#   SEEN    <count>
# awk's `>` truncates on its first write in a process, so a per-file redirection here would
# leave only the last file's records; the caller appends instead.
#
# Two passes over the whole file, because a binding can sit after the subscript that uses it.
{
    L[NR] = $0
}
END {
    # Pass 1: the names THIS FILE binds to the machine's core identity, an intervening cast
    # allowed. Never corpus-wide: `cpu` and `me` are ordinary local names elsewhere.
    for (i = 1; i <= NR; i++)
    {
        s = L[i]
        while (match(s, /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*[^;]*arch_cpu_id[ \t]*\(/))
        {
            t = substr(s, RSTART, RLENGTH)
            s = substr(s, RSTART + RLENGTH)
            sub(/[ \t]*=.*$/, "", t)
            CPU[t] = 1
        }
    }

    seen = 0
    for (i = 1; i <= NR; i++)
    {
        s = L[i]
        closed = 0
        while (match(s, /(^|[^A-Za-z0-9_])(current|idle|boot)[ \t]*\[[^][]*\]/))
        {
            m = substr(s, RSTART, RLENGTH)
            s = substr(s, RSTART + RLENGTH)
            closed++
            seen++
            idx = m
            sub(/^[^[]*\[/, "", idx)
            sub(/\]$/, "", idx)
            bad = 0
            if (idx ~ /(^|[^A-Za-z0-9_])arch_cpu_id[ \t]*\(/)
            {
                bad = 1
            }
            n = split(idx, tok, /[^A-Za-z0-9_]+/)
            for (k = 1; k <= n; k++)
            {
                if (tok[k] != "" && (tok[k] in CPU))
                {
                    bad = 1
                }
            }
            if (bad)
            {
                printf("FINDING %s:%d: %s\n", F, i, L[i])
            }
        }
        # An opening the closed shape above could not bound holds a nested subscript. Refused,
        # never skipped: unclassified is not clean. Counted by the SAME match loop, so the two
        # tallies cannot differ over where one match ends and the next may begin.
        o = L[i]
        opens = 0
        while (match(o, /(^|[^A-Za-z0-9_])(current|idle|boot)[ \t]*\[/))
        {
            o = substr(o, RSTART + RLENGTH)
            opens++
        }
        if (opens > closed)
        {
            printf("REFUSED %s:%d: a current/idle/boot subscript this scan cannot bound: %s\n",
                   F, i, L[i])
        }
    }
    printf("SEEN %d\n", seen)
}
