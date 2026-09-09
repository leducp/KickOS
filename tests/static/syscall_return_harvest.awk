# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The CODE side of the syscall return-code contract, harvested from one translation unit:
# every -KOS_E* code a RETURN statement carries, per dispatch arm and per function
# definition, plus the names each arm calls.
#
#   awk -v F=<real path> [-v ARMS=1] [-v NOGUARD=1] -f syscall_return_harvest.awk <residue>
#
# Input is the residue of tests/lib/strip_comments.awk, line for line with the source. A
# caller handing the RAW file instead reads a comment that NAMES a code as a code the arm
# returns, and the check then rests on the prose it exists to check.
#
# Records, tab-separated, on stdout:
#   fn       <file> <name>                  a function definition lives here
#   fncode   <file> <name> <CODE> <line>    that definition RETURNS -KOS_<CODE>
#   fnadmit  <file> <name>                  its body reaches the task object-budget gate
#   arm      <gid> <label>                  a `case KOS_SYS_*:` label of arm <gid>
#   armcode  <gid> <CODE> <file> <line>
#   armcall  <gid> <name>
#   armadmit <gid>
#
# An arm's gid is its labels joined by `+`, so two arms carrying one label under opposite
# `#if` arms share a gid and their code sets union.
#
# ONLY A RETURN STATEMENT CARRIES A CODE HERE. A refusal handed to another thread by writing
# its wait_result is that thread's own syscall answering, not this one's, and a code
# forwarded through a variable (`return rc`) names nothing this scan can read.
#
# A member call (`pool.at()`, `p->free()`) is NOT recorded: it resolves against a type, and a
# bare name lookup would hand the arm whatever else in the tree carries that name.
#
# A refusal reached only from a NULL CALLER CONTEXT is not harvested: `if (c == nullptr)`
# alone, where `c` came from sched::current(). No dispatch arm reaches such a branch, the
# dispatch dereferencing sched::current() unguarded, so that code answers a kernel-internal
# caller and never a syscall. A condition with anything else in it (`c == nullptr or not
# c->privileged`) is a real gate and IS harvested. NOGUARD=1 disables the skip, for the
# gate's own near-miss control.
#
# Exits 2, and no record may then be read as an absence, when the braces do not balance at
# EOF, when a `case KOS_SYS_*:` run is followed by no block, or when two label runs sit at
# different nesting depths.

function opens_of(s,   t) { t = s; return gsub(/[{]/, "", t) }
function closes_of(s,   t) { t = s; return gsub(/[}]/, "", t) }

# Every KOS_E* token of a return statement, provided the statement negates one: `return
# static_cast<uint64_t>(-(err == 0 ? KOS_EBADF : err))` carries EBADF as surely as a bare
# `return -KOS_EPERM` does.
function emit_codes(stmt, kind, who, ln,   s, tok, neg) {
    neg = 0
    s = stmt
    while (match(s, /KOS_E[A-Z][A-Z0-9]*/) > 0) {
        if (index(substr(s, 1, RSTART - 1), "-") > 0) { neg = 1 }
        tok = substr(s, RSTART + 4, RLENGTH - 4)
        if (neg) {
            if (kind == "arm") {
                printf("armcode%s%s%s%s%s%s%s%d\n", TAB, who, TAB, tok, TAB, F, TAB, ln)
            }
            else {
                printf("fncode%s%s%s%s%s%s%s%d\n", TAB, F, TAB, who, TAB, tok, TAB, ln)
            }
        }
        s = substr(s, RSTART + RLENGTH)
    }
}

BEGIN {
    TAB = "\t"
    if (F == "") {
        printf("syscall_return_harvest.awk: -v F=<path> is required\n") > "/dev/stderr"
        exit 2
    }
    depth = 0
    infn = 0
    fname = ""
    fstart = 0
    headpend = 0
    headname = ""
    inarm = 0
    gid = ""
    nlab = 0
    labpend = 0
    armdepth = -1
    astart = 0
    skip = 0
    skippend = 0
    instmt = 0
    stmt = ""
    stmtline = 0
    bad = 0
}

{
    line = $0
    op = opens_of(line)
    cl = closes_of(line)

    # A variable that holds sched::current() in THIS body. Read before any skip, so the
    # assignment is seen even when the guard below it is the very next line.
    if (match(line, /[A-Za-z_][A-Za-z_0-9]*[ \t]*=[ \t]*sched::current\(\)/) > 0) {
        cv = substr(line, RSTART, RLENGTH)
        sub(/[ \t]*=.*$/, "", cv)
        ctx[cv] = 1
    }

    if (skip > 0) {
        skip = skip + op - cl
        depth = depth + op - cl
        if (skip <= 0) { skip = 0 }
        next
    }
    if (skippend) {
        skippend = 0
        if (op > 0) {
            skip = op - cl
            depth = depth + op - cl
            if (skip <= 0) { skip = 0 }
            next
        }
    }
    if (!NOGUARD && match(line, /^[ \t]*if[ \t]*\([A-Za-z_][A-Za-z_0-9]*[ \t]*==[ \t]*nullptr\)[ \t]*$/) > 0) {
        gv = line
        sub(/^[ \t]*if[ \t]*\(/, "", gv)
        sub(/[ \t]*==.*$/, "", gv)
        if (gv in ctx) {
            skippend = 1
            depth = depth + op - cl
            next
        }
    }

    if (infn || inarm) {
        if (instmt) {
            stmt = stmt " " line
        }
        else if (match(line, /(^|[^A-Za-z_0-9])return([^A-Za-z_0-9]|$)/) > 0) {
            stmt = line
            stmtline = FNR
            instmt = 1
        }
        if (instmt && index(stmt, ";") > 0) {
            if (inarm) { emit_codes(stmt, "arm", gid, stmtline) }
            if (infn) { emit_codes(stmt, "fn", fname, stmtline) }
            instmt = 0
            stmt = ""
        }
        if (index(line, "task_object_admit(") > 0) {
            if (inarm) { printf("armadmit%s%s\n", TAB, gid) }
            if (infn) { printf("fnadmit%s%s%s%s\n", TAB, F, TAB, fname) }
        }
    }

    # The arm's callees, from its WHOLE body: a mint stores its status in a local and
    # answers it two statements later, so a return-only scan would miss the creator.
    if (inarm) {
        s = line
        while (match(s, /[A-Za-z_][A-Za-z_0-9]*[ \t]*\(/) > 0) {
            nm = substr(s, RSTART, RLENGTH)
            sub(/[ \t]*\($/, "", nm)
            bef = substr(s, RSTART - 1, 1)
            s = substr(s, RSTART + RLENGTH)
            # A MEMBER call resolves against a type and not against a bare name, so
            # `pool.at()` must not pick up whatever else in the tree is called `at`.
            if (bef == "." || bef == ">") { continue }
            printf("armcall%s%s%s%s\n", TAB, gid, TAB, nm)
        }
    }

    if (ARMS && match(line, /^[ \t]*case[ \t]+KOS_SYS_[A-Za-z_0-9]*:[ \t]*$/) > 0) {
        nm = line
        sub(/^[ \t]*case[ \t]+/, "", nm)
        sub(/:[ \t]*$/, "", nm)
        if (!labpend) {
            if (armdepth < 0) { armdepth = depth }
            else if (depth != armdepth) {
                printf("%s:%d: a case label sits at brace depth %d, not %d: the arms of one switch are what this reads\n",
                       F, FNR, depth, armdepth) > "/dev/stderr"
                bad = 1
            }
            nlab = 0
            gid = ""
        }
        labpend = 1
        nlab++
        lab[nlab] = nm
        if (nlab == 1) { gid = nm }
        else { gid = gid "+" nm }
        next
    }
    if (labpend) {
        if (op > 0) {
            labpend = 0
            inarm = 1
            astart = depth
            for (i = 1; i <= nlab; i++) {
                printf("arm%s%s%s%s\n", TAB, gid, TAB, lab[i])
            }
        }
        else if (line ~ /[^ \t]/) {
            printf("%s:%d: a case label run is followed by no block, so its arm reads as empty\n",
                   F, FNR) > "/dev/stderr"
            bad = 1
            labpend = 0
        }
    }

    probe = line
    sub(/^[ \t]*(\[\[[^]]*\]\][ \t]*)*/, "", probe)
    sub(/^__attribute__\(\([^)]*\)\)[ \t]*/, "", probe)
    if (!infn && !inarm && !headpend && line !~ /^[ \t]*#/ \
        && match(probe, /[A-Za-z_][A-Za-z_0-9]*[ \t]*\(/) > 0) {
        nm = substr(probe, RSTART, RLENGTH)
        sub(/[ \t]*\($/, "", nm)
        pre = substr(probe, 1, RSTART - 1)
        # A DEFINITION and not a call or a declaration: nothing but a return type in front,
        # no trailing `;`, and a body brace opening on this line or on the parameter list's
        # last one. The keyword filter is load-bearing, `if (x)` over a `{` having a
        # definition's exact shape.
        if (nm != "if" && nm != "for" && nm != "while" && nm != "switch" && nm != "return" \
            && nm != "catch" && nm != "sizeof" && nm != "delete" \
            && pre ~ /^([A-Za-z_][A-Za-z_0-9:<>*&, \t]*[^A-Za-z_0-9])?$/ \
            && line !~ /;[ \t]*$/) {
            headpend = 1
            headname = nm
            headage = 0
        }
    }
    if (headpend) {
        if (op > 0) {
            headpend = 0
            infn = 1
            fname = headname
            fstart = depth
            split("", ctx)
            printf("fn%s%s%s%s\n", TAB, F, TAB, fname)
        }
        else {
            headage++
            if (index(line, ";") > 0 || headage > 6) { headpend = 0 }
        }
    }

    depth = depth + op - cl
    if (inarm && depth <= astart) {
        inarm = 0
        gid = ""
        instmt = 0
        stmt = ""
    }
    if (infn && depth <= fstart) {
        infn = 0
        fname = ""
        instmt = 0
        stmt = ""
    }
}

END {
    if (bad) { exit 2 }
    if (depth != 0) {
        printf("%s: the braces do not balance at EOF (depth %d), so the bodies below the break went unread\n",
               F, depth) > "/dev/stderr"
        exit 2
    }
    if (skip > 0 || skippend || labpend || headpend || instmt) {
        printf("%s: the scan ended mid-construct, so the tail of the file is unread\n", F) > "/dev/stderr"
        exit 2
    }
    if (ARMS && armdepth < 0) {
        printf("%s: no `case KOS_SYS_*:` label was found, so every arm reads as empty\n", F) > "/dev/stderr"
        exit 2
    }
}
