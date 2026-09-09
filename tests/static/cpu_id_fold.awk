# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The definition scanner behind tests/static/check_cpu_id_fold.sh. Reads one file and sorts
# every site that names arch_cpu_id into a definition, a refusal, or neither:
#
#   awk -v F=<real-path> -v GUARD=<ere> -v EXPRKWS=<word list> -f cpu_id_fold.awk \
#       FINDINGS=<file> REFUSED=<file> <file>
#
# GUARD and EXPRKWS come from the caller so the self-test and the corpus scan cannot drift
# apart. Both output files are APPENDED to, one awk process per corpus file: `>` opens for
# writing on first use in a process, so the second file scanned would truncate the first
# file's findings away.
#
# A DECLARATION (`uint32_t arch_cpu_id(void);`) and a CALL are both allowed, so a finding
# keys on the definition SHAPE. A line whose statement ends in `;` is one of those two. What
# is left is read on both sides of the seam's own parenthesised list:
#
#   after      a definition's list is followed by nothing but trailing qualifiers and the
#              body's opening brace, on the line or on the next non-blank one (this tree is
#              Allman, so the next line is the usual case). A call sits inside an expression,
#              so the rest of that expression follows its list instead: `] == nullptr)`,
#              `!= 0u)`, a bare `)`.
#
#   before     a definition names a RETURN TYPE first, so the text from the last `{`, `}` or
#              `;` on the line is identifier tokens, `*` and the `"C"` of `extern "C"`, and
#              its final identifier is not one EXPRKWS lists. This reading only ever ADDS a
#              refusal: a return type in front of a list the rest of an expression follows is
#              a shape the scan cannot name. It is not required for a finding, because an
#              attribute or a macro in front of a real definition would then read as an
#              expression and escape.
#
# A parameter list this scan cannot bound, and a complete declarator no brace follows, are
# both REFUSED: unclassified is not clean.

BEGIN {
    n = split(EXPRKWS, w, " ")
    for (i = 1; i <= n; i++) { EXPR[w[i]] = 1 }
}
# Strip a whole-line comment and any trailing // comment before judging, so the seam header
# prose (which names arch_cpu_id repeatedly) cannot register.
{ line = $0; sub(/\/\/.*$/, "", line) }
# A definition inside `#if KICKOS_NUM_CORES > 1` is not compiled at one core, so it cannot
# break the fold and is not a finding here. This scan has no preprocessor, so it tracks that
# ONE guard by nesting depth: the multi-core arm opens at the depth its `#if` reached, and
# closes at the `#else` or `#endif` that returns to that depth. A guard spelled any other way
# (`>= 2`) is not recognised and its body is scanned, so the canonical spelling is the one
# arch/include/kickos/arch/arch.h uses.
line ~ /^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef)/ {
    depth++
    if (multi == 0 && line ~ GUARD) { multi = depth }
    next
}
line ~ /^[[:space:]]*#[[:space:]]*(else|elif)/ {
    if (multi == depth) { multi = 0 }
    next
}
line ~ /^[[:space:]]*#[[:space:]]*endif/ {
    if (multi == depth) { multi = 0 }
    depth--
    next
}
multi > 0 { next }
line ~ /^[[:space:]]*[*]/ { next }
line !~ /arch_cpu_id[[:space:]]*\(/ { next }
# The macro itself, which leg 1 owns.
line ~ /^[[:space:]]*#[[:space:]]*define/ { next }
{
    code = line
    sub(/[[:space:]]+$/, "", code)

    # The text after the seam name AND its own parenthesised list. A list this scan cannot
    # bound (a parenthesis nested inside it) leaves the verdict UNKNOWN, unless the statement
    # ends anyway.
    suf = code
    if (sub(/^.*arch_cpu_id[[:space:]]*\([^()]*\)/, "", suf) == 0) {
        if (code ~ /;[[:space:]]*$/) { next }
        print F ":" FNR ": " $0 >> REFUSED
        next
    }
    sub(/^([[:space:]]*(const|volatile|noexcept|override|final))*[[:space:]]*/, "", suf)

    # The body opens right after the list. Judged BEFORE the statement terminator, because a
    # stray `;` after the closing brace still leaves a definition.
    if (suf ~ /^\{/) {
        print F ":" FNR ": " $0 >> FINDINGS
        next
    }
    # No body here, so a terminated statement is a declaration or a call.
    if (code ~ /;[[:space:]]*$/) { next }

    # The text before the seam name, from the last scope or statement boundary on the line, so
    # an `extern "C" {` or a closing brace ahead of it does not read as part of the declarator.
    pre = code
    sub(/arch_cpu_id[[:space:]]*\(.*$/, "", pre)
    sub(/^.*[{};]/, "", pre)
    sub(/^[[:space:]]+/, "", pre)
    sub(/[[:space:]]+$/, "", pre)
    kw = pre
    sub(/[^A-Za-z0-9_]+$/, "", kw)
    sub(/^.*[^A-Za-z0-9_]/, "", kw)
    type_pre = (pre ~ /^[A-Za-z0-9_"*[:space:]]+$/ && pre ~ /[A-Za-z_]/ && !(kw in EXPR))

    if (suf != "") {
        # The rest of an enclosing expression follows the list, so the seam is a CALL. A
        # return type in front of one is neither an expression nor a shape this scan can name.
        if (pre != "" && type_pre) { print F ":" FNR ": " $0 >> REFUSED }
        next
    }
    # Nothing follows the list. Allman: the brace is on the next non-blank line, and FNR walks
    # with getline, so the declarator line is kept.
    ln = FNR
    nxt = ""
    while ((getline nxt) > 0) { if (nxt ~ /[^[:space:]]/) { break } }
    if (nxt ~ /^[[:space:]]*\{/) {
        print F ":" ln ": " $0 >> FINDINGS
        next
    }
    # A complete declarator that no brace follows: this scan cannot classify it.
    print F ":" ln ": " $0 >> REFUSED
}
