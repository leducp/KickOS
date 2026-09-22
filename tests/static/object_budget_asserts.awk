# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The reader behind check_object_budget_asserts.sh. Reads ONE comment-stripped C++ header
# and prints two kinds of record:
#
#   CEIL   <kind> <budget-macro>    a charged arm of task_object_ceiling
#   ASSERT <pool> <budget> <op> <pool2>   a static_assert of the pool-above-budget shape
#   CEILSEEN <0|1>                  whether the task_object_ceiling header line was matched
#   PARSE  <seen> <parsed>          static_assert openers seen, and those of the shape above
#
# THE LAST TWO SAY WHY A COUNT IS ZERO. No CEIL record means either a switch charging nothing
# or a function header this reader did not recognise, and no ASSERT for a budget means either
# a deleted assert or one spelled past the parse above. Both pairs read alike in the records
# and only one of each is a claim about the tree, so the two counts are printed rather than
# left to be inferred.
#
# THE KIND SET IS DERIVED AND NEVER LISTED. task_object_ceiling's own switch is what says
# which pools are charged, so a charged kind added there without an assert beside it is a
# missing record here rather than a name nobody thought to add to a list.
#
# The `default:` arm answers 0 and names no budget macro, so it emits nothing; clearing the
# pending kind on it is belt and braces for a reader that met a `return` it did not expect.

BEGIN { inceil = 0; started = 0; depth = 0; pending = ""; all = "" }

{ all = all " " $0 }

/constexpr[[:space:]]+int[[:space:]]+task_object_ceiling[[:space:]]*\(/ {
    inceil = 1
    ceilseen = 1
    started = 0
    depth = 0
    pending = ""
}

inceil {
    t = $0
    opens = gsub(/\{/, "&", t)
    t = $0
    closes = gsub(/\}/, "&", t)
    if (match($0, /case[[:space:]]+CapType::CAP_[A-Z_0-9]+[[:space:]]*:/)) {
        s = substr($0, RSTART, RLENGTH)
        sub(/.*CapType::/, "", s)
        sub(/[[:space:]]*:$/, "", s)
        pending = s
    }
    if (match($0, /default[[:space:]]*:/)) {
        pending = ""
    }
    if (pending != "" && match($0, /return[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*;/)) {
        m = substr($0, RSTART, RLENGTH)
        sub(/return[[:space:]]+/, "", m)
        sub(/[[:space:]]*;$/, "", m)
        print "CEIL " pending " " m
        pending = ""
    }
    started = started + opens
    depth = depth + opens - closes
    if (started > 0 && depth <= 0) {
        inceil = 0
    }
}

END {
    print "CEILSEEN " (ceilseen + 0)
    gsub(/[[:space:]]+/, " ", all)
    n = split(all, parts, /static_assert[[:space:]]*\(/)
    for (i = 2; i <= n; i++) {
        s = parts[i]
        sub(/^ +/, "", s)
        if (!match(s, /^[A-Za-z_][A-Za-z0-9_]*/)) {
            continue
        }
        pool1 = substr(s, 1, RLENGTH)
        s = substr(s, RLENGTH + 1)
        if (!match(s, /^ *== *0 +or +/)) {
            continue
        }
        s = substr(s, RLENGTH + 1)
        if (!match(s, /^[A-Za-z_][A-Za-z0-9_]*/)) {
            continue
        }
        bud = substr(s, 1, RLENGTH)
        s = substr(s, RLENGTH + 1)
        if (!match(s, /^ *<=? */)) {
            continue
        }
        op = substr(s, RSTART, RLENGTH)
        gsub(/ /, "", op)
        s = substr(s, RLENGTH + 1)
        if (!match(s, /^[A-Za-z_][A-Za-z0-9_]*/)) {
            continue
        }
        pool2 = substr(s, 1, RLENGTH)
        parsed++
        print "ASSERT " pool1 " " bud " " op " " pool2
    }
    printf "PARSE %d %d\n", n - 1, parsed + 0
}
