# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The DOC side of the syscall return-code contract, and the join: every -KOS_E* code a
# dispatch arm returns must be documented on that syscall's own entry in the
# `enum kos_syscall_nr` table.
#
#   awk -v ABI=<abi.h path> -v DISPATCH=<dispatch TU path> -v CODES="<taxonomy>" \
#       -f syscall_return_agree.awk <abi.h> <records>
#
# The FIRST input is the table header, read RAW: the documented codes ARE its comments, so
# the comment-stripped residue every other gate reads would leave this side empty. The
# SECOND is tests/static/syscall_return_harvest.awk's records.
#
# CODES is the taxonomy out of system/include/kickos/sys/errno.h, space-separated and
# WITHOUT the KOS_ prefix. It is the only thing that tells a code from an English word in a
# comment ("the group is EMPTY"), so an empty CODES is refused rather than trusted.
#
#   entry     a `KOS_SYS_<name> = <n>,` line inside the table, plus every `//` line under it
#             until the next entry. That text is the syscall's documented contract.
#
#   as        `-> as KOS_SYS_<other>, plus -KOS_ETIMEDOUT` INHERITS the other entry's codes,
#             which is how the three timed twins are written today. Resolved transitively, so
#             a chain reaches its root. NOAS=1 disables it, for the gate's own near-miss
#             control.
#
#   grouped   one arm under several `case` labels answers several syscalls from ONE body, and
#             nothing in the text says which label reaches which refusal. A code the shared
#             arm returns therefore has to be documented by ONE label of the group, not by
#             each: KOS_SYS_FRAME_MAP and KOS_SYS_FRAME_UNMAP share an arm.
#
#   findings  `find<TAB><text>` on stdout, one per line. Counts go out as
#             `stat<TAB><key><TAB><value>`, so a caller reads the corpus it got rather than
#             the corpus it asked for.
#
# Exits 2, and no finding count may then be read as clean, when CODES is empty, when the
# table holds no entry, when the records hold no arm, or when the table never closes.

function is_code(name) { return (name in TAX) }

# The KOS_E* tokens of a comment or a statement. A token whose match is followed by another
# identifier character is a DIFFERENT macro (KOS_EP_MSG_MAX, KOS_EXIT_FAULT) and is dropped
# whole: its tail would otherwise read as a code name nothing defines.
function scan_prefixed(text, out,   s, tok, nxt, n) {
    n = 0
    s = text
    while (match(s, /KOS_E[A-Z][A-Z0-9]*/) > 0) {
        tok = substr(s, RSTART + 4, RLENGTH - 4)
        nxt = substr(s, RSTART + RLENGTH, 1)
        s = substr(s, RSTART + RLENGTH)
        if (nxt ~ /[A-Za-z_0-9]/) { continue }
        out[tok] = 1
        n++
    }
    return n
}

BEGIN {
    TAB = "\t"
    if (CODES == "") {
        printf("syscall_return_agree.awk: -v CODES=<taxonomy> is required and must not be empty\n") > "/dev/stderr"
        exit 2
    }
    ntax = split(CODES, tl, " ")
    ncode = 0
    for (i = 1; i <= ntax; i++) {
        if (tl[i] != "") { TAX[tl[i]] = 1; ncode++ }
    }
    if (ncode == 0) {
        printf("syscall_return_agree.awk: CODES holds no name\n") > "/dev/stderr"
        exit 2
    }
    intable = 0
    closed = 0
    cur = ""
    nentry = 0
}

# --- the table, raw ------------------------------------------------------------
FILENAME == ABI {
    if (!intable) {
        if ($0 ~ /^[ \t]*enum[ \t]+kos_syscall_nr[ \t]*$/) { intable = 1 }
        next
    }
    if ($0 ~ /^[ \t]*\};[ \t]*$/) {
        intable = 0
        closed = 1
        next
    }
    if (match($0, /^[ \t]*KOS_SYS_[A-Za-z_0-9]*[ \t]*=[ \t]*[0-9]+/) > 0) {
        nm = $0
        sub(/^[ \t]*/, "", nm)
        sub(/[ \t]*=.*$/, "", nm)
        num = $0
        sub(/^[^=]*=[ \t]*/, "", num)
        sub(/[^0-9].*$/, "", num)
        cur = nm
        nentry++
        entries[nm] = 1
        entryline[nm] = FNR
        if (num in numowner) {
            printf("find%s%s:%d: %s and %s both take syscall number %s\n",
                   TAB, ABI, FNR, numowner[num], nm, num)
        }
        else { numowner[num] = nm }
        txt = ""
        p = index($0, "//")
        if (p > 0) {
            txt = substr($0, p + 2)
            sub(/^[ \t]*/, "", txt)
        }
        doctext[nm] = txt
        next
    }
    if (cur != "" && $0 ~ /^[ \t]*\/\//) {
        txt = $0
        sub(/^[ \t]*\/\/[ \t]*/, "", txt)
        doctext[cur] = doctext[cur] " " txt
    }
    next
}

# --- the harvested records -----------------------------------------------------
{
    n = split($0, r, TAB)
    if (r[1] == "arm") {
        armlabels[r[2]] = armlabels[r[2]] " " r[3]
        if (r[3] in labgid) {
            if (labgid[r[3]] != r[2]) {
                printf("find%s%s: %s is answered by two arms with different label runs (%s and %s)\n",
                       TAB, DISPATCH, r[3], labgid[r[3]], r[2])
            }
        }
        else { labgid[r[3]] = r[2]; nlabel++ }
        if (!(r[2] in armseen)) { armseen[r[2]] = 1; narm++ }
    }
    else if (r[1] == "armcode") {
        if (is_code(r[3])) { armcode[r[2], r[3]] = r[4] ":" r[5] }
    }
    else if (r[1] == "armcall") { calls[r[2]] = calls[r[2]] " " r[3] }
    else if (r[1] == "armadmit") { armadmit[r[2]] = 1 }
    else if (r[1] == "fn") {
        fndef[r[2], r[3]] = 1
        if (!((r[2], r[3]) in fnseen)) {
            fnseen[r[2], r[3]] = 1
            fncount[r[3]]++
            fntus[r[3]] = fntus[r[3]] " " r[2]
        }
    }
    else if (r[1] == "fncode") {
        if (is_code(r[4])) { fncode[r[2], r[3], r[4]] = r[2] ":" r[5] }
    }
    else if (r[1] == "fnadmit") { fnadmit[r[2], r[3]] = 1 }
}

END {
    if (!closed) {
        printf("syscall_return_agree.awk: %s: enum kos_syscall_nr never closes; the table is unread\n",
               ABI) > "/dev/stderr"
        exit 2
    }
    if (nentry == 0) {
        printf("syscall_return_agree.awk: %s: the table holds no KOS_SYS_* entry\n", ABI) > "/dev/stderr"
        exit 2
    }
    if (narm == 0) {
        printf("syscall_return_agree.awk: the records hold no dispatch arm\n") > "/dev/stderr"
        exit 2
    }

    # Documented codes, then the `as <other>` closure. Four passes carry any chain this
    # table can hold; a longer one leaves the tail unresolved and reports as a missing code,
    # which is the safe direction.
    for (nm in entries) {
        scan_prefixed(doctext[nm], seen)
        for (tok in seen) {
            if (is_code(tok)) { doc[nm, tok] = 1 }
            else {
                printf("find%s%s:%d: %s documents -KOS_%s, which system/include/kickos/sys/errno.h does not define\n",
                       TAB, ABI, entryline[nm], nm, tok)
            }
        }
        split("", seen)
        # A bare enumeration, `(EPERM/EINVAL/ENOMEM)`, is how half the table is written.
        t = doctext[nm]
        while (match(t, /[A-Z][A-Z0-9]+/) > 0) {
            tok = substr(t, RSTART, RLENGTH)
            nxt = substr(t, RSTART + RLENGTH, 1)
            prv = substr(t, RSTART - 1, 1)
            t = substr(t, RSTART + RLENGTH)
            if (nxt ~ /[A-Za-z_0-9]/ || prv ~ /[A-Za-z_0-9]/) { continue }
            if (is_code(tok)) { doc[nm, tok] = 1 }
        }
        if (!NOAS) {
            t = doctext[nm]
            while (match(t, /as[ \t]+KOS_SYS_[A-Za-z_0-9]*/) > 0) {
                other = substr(t, RSTART, RLENGTH)
                sub(/^as[ \t]+/, "", other)
                t = substr(t, RSTART + RLENGTH)
                inherits[nm] = inherits[nm] " " other
            }
        }
    }
    for (pass = 1; pass <= 4; pass++) {
        for (nm in entries) {
            k = split(inherits[nm], parents, " ")
            for (i = 1; i <= k; i++) {
                for (tok in TAX) {
                    if ((parents[i], tok) in doc) { doc[nm, tok] = 1 }
                }
            }
        }
    }

    # --- totality: the table and the dispatch name the same syscalls ------------
    for (nm in entries) {
        if (!(nm in labgid)) {
            printf("find%s%s:%d: %s is in the table and no dispatch arm answers it\n",
                   TAB, ABI, entryline[nm], nm)
        }
    }
    for (nm in labgid) {
        if (!(nm in entries)) {
            printf("find%s%s: %s is a dispatch arm and the table holds no entry for it\n",
                   TAB, DISPATCH, nm)
        }
    }

    # --- the codes each arm reaches --------------------------------------------
    nresolved = 0
    nseam = 0
    nextern = 0
    for (gid in armseen) {
        for (tok in TAX) {
            if ((gid, tok) in armcode) { can[gid, tok] = armcode[gid, tok] }
        }
        k = split(calls[gid], cs, " ")
        for (i = 1; i <= k; i++) {
            nmc = cs[i]
            if (nmc in done) { continue }
            done[nmc] = 1
            if ((DISPATCH, nmc) in fndef) { resolved[nmc] = " " DISPATCH }
            else if (fncount[nmc] >= 1) {
                resolved[nmc] = fntus[nmc]
                if (fncount[nmc] > 1) { seam[nmc] = 1 }
            }
            else { extern[nmc] = 1; continue }
        }
        split("", done)
        for (i = 1; i <= k; i++) {
            nmc = cs[i]
            if (!(nmc in resolved)) { continue }
            m = split(resolved[nmc], tus, " ")
            for (j = 1; j <= m; j++) {
                tu = tus[j]
                for (tok in TAX) {
                    if ((tu, nmc, tok) in fncode) { can[gid, tok] = fncode[tu, nmc, tok] }
                }
                if ((tu, nmc) in fnadmit) { armadmit[gid] = 1 }
            }
        }
    }
    for (nmc in resolved) { nresolved++ }
    for (nmc in seam) { nseam++ }
    for (nmc in extern) { nextern++ }

    # --- clause: a code the arm returns is documented on one of its labels ------
    for (gid in armseen) {
        k = split(armlabels[gid], labs, " ")
        where = DISPATCH
        if (labs[1] in entries) { where = ABI ":" entryline[labs[1]] }
        for (tok in TAX) {
            if (!((gid, tok) in can)) { continue }
            held = 0
            for (i = 1; i <= k; i++) {
                if ((labs[i], tok) in doc) { held = 1 }
            }
            if (held) { continue }
            printf("find%s%s: %s returns -KOS_%s at %s and this entry does not document it\n",
                   TAB, where, gid, tok, can[gid, tok])
        }
    }

    # --- clause: the three exhaustion answers are not collapsed ----------------
    # A syscall that asks the TASK's object budget has all three refusals: -KOS_ENOMEM for
    # the pool, -KOS_EMFILE for the caller's capability table, -KOS_EOVERFLOW for the budget
    # itself. Whoever documents one and not the others has collapsed them, and the fixes are
    # opposite (more RAM against a wider declared table). The set is the budget gate's own
    # call sites, never a list kept here.
    for (gid in armseen) {
        if (!(gid in armadmit)) { continue }
        k = split(armlabels[gid], labs, " ")
        where = DISPATCH
        if (labs[1] in entries) { where = ABI ":" entryline[labs[1]] }
        n3 = split("ENOMEM EMFILE EOVERFLOW", three, " ")
        for (j = 1; j <= n3; j++) {
            held = 0
            for (i = 1; i <= k; i++) {
                if ((labs[i], three[j]) in doc) { held = 1 }
            }
            if (held) { continue }
            printf("find%s%s: %s charges the task object budget and does not document -KOS_%s; the three exhaustion answers must not be collapsed\n",
                   TAB, where, gid, three[j])
        }
    }

    printf("stat%sentries%s%d\n", TAB, TAB, nentry)
    printf("stat%sarms%s%d\n", TAB, TAB, narm)
    printf("stat%slabels%s%d\n", TAB, TAB, nlabel)
    printf("stat%staxonomy%s%d\n", TAB, TAB, ncode)
    printf("stat%scallees_resolved%s%d\n", TAB, TAB, nresolved)
    printf("stat%scallees_multi_unit%s%d\n", TAB, TAB, nseam)
    printf("stat%scallees_external%s%d\n", TAB, TAB, nextern)
}
