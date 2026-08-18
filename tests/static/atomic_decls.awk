# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The declaration harvester behind tests/static/check_atomic_rmw.sh. Prints one TAB-separated
# record per atomic object a file declares. Input must be ALREADY-STRIPPED, one file at a
# time; F carries the real path:
#   awk -v F=<real-path> -f atomic_decls.awk <stripped-file>
#
#   <file> <TAB> <line> <TAB> <name> <TAB> <member|param>
#
# Over-harvesting is the dangerous direction: a name here that no atomic bears makes the
# caller fire on ordinary code.
#
# Only `member` records are safe to match corpus-wide; a `param` name is scoped to one
# function and covers its own file only.
BEGIN {
    # NO trailing `[ \t]*` on any alternative: the match is leftmost-LONGEST, so the type
    # token would eat the space and the identifier-run guard below would reject the
    # declaration it protects.
    AT = "(std::atomic(_flag)?(<[^<>]*>)?|_Atomic[ \t]+[A-Za-z_][A-Za-z0-9_]*|atomic_(flag|bool|char|short|int|long|llong|u?int(_least|_fast)?(8|16|32|64)_t|size_t|uintptr_t|ptrdiff_t))"
    KEYWORD = "^(const|volatile|restrict|static|extern|inline|constinit|constexpr|mutable|register|thread_local|struct|class|union|enum|signed|unsigned|_Atomic|operator|return|sizeof|alignas|alignof)$"
}
# A #define whose replacement text names an atomic type would donate whatever follows it.
/^[ \t]*#/ { next }
# A type alias names a type, not an object.
/(^|[^A-Za-z0-9_])(typedef|using)([^A-Za-z0-9_]|$)/ { next }
{
    line = $0
    while (match(line, AT)) {
        after = substr(line, RSTART + RLENGTH, 1)
        rest = substr(line, RSTART + RLENGTH)
        line = rest
        # A type token running on into identifier characters was only a prefix of a name.
        if (after ~ /^[A-Za-z0-9_]$/) { continue }
        sub(/^[ \t*&]+/, "", rest)
        while (sub(/^(const|volatile|restrict|__restrict)[ \t*&]+/, "", rest)) { }
        if (!match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) { continue }
        nm = substr(rest, RSTART, RLENGTH)
        end = substr(rest, RLENGTH + 1, 1)
        if (nm ~ KEYWORD) { continue }
        # `(` is excluded on purpose: a function returning an atomic would donate its name.
        if (end == "" || end ~ /^[ \t;={[]$/) {
            printf("%s\t%d\t%s\tmember\n", F, FNR, nm)
        }
        else if (end ~ /^[,)]$/) {
            printf("%s\t%d\t%s\tparam\n", F, FNR, nm)
        }
    }
}
