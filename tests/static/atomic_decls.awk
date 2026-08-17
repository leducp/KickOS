# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The declaration harvester behind tests/static/check_atomic_rmw.sh. Prints one TAB-separated
# record per atomic object a file declares. Run on ALREADY-STRIPPED input, one file at a
# time, with the real path passed in because the input is a scratch copy:
#   awk -v F=<real-path> -f atomic_decls.awk <stripped-file>
#
#   <file> <TAB> <line> <TAB> <name> <TAB> <member|param>
#
# The operator half of the RMW rule (`++`, `+=`, `|=`) carries no atomic spelling at the use
# site, so the only way a text scan can tell `count++` on an atomic from `count++` on a
# uint32_t is to learn which identifiers were DECLARED atomic. This file is that half.
#
# Over-harvesting is the dangerous direction: a name learned here that no atomic bears makes
# the caller fire on ordinary code. Hence the type token must end on a non-identifier
# character (so `std::atomic_thread_fence` does not donate `_thread_fence`), cv qualifiers
# are consumed before the name is read, and KEYWORD below refuses what is left.
#
# The last field is why the caller can scan a struct member's name across the whole corpus
# without drowning: a declarator ending in `)` or `,` is a PARAMETER, whose scope is one
# function, and a parameter called `c` or `mode` must never teach a corpus-wide pattern. Only
# `member` records are safe to match cross-file; `param` records still cover their own file.
BEGIN {
    # NO trailing `[ \t]*` on any alternative. The match is leftmost-LONGEST, so a greedy
    # trailing space would be eaten by the type token and the identifier-run guard below would
    # then see the NAME character and reject the declaration it was meant to protect.
    AT = "(std::atomic(_flag)?(<[^<>]*>)?|_Atomic[ \t]+[A-Za-z_][A-Za-z0-9_]*|KOS_ATOMIC_U32|atomic_(flag|bool|char|short|int|long|llong|u?int(_least|_fast)?(8|16|32|64)_t|size_t|uintptr_t|ptrdiff_t))"
    KEYWORD = "^(const|volatile|restrict|static|extern|inline|constinit|constexpr|mutable|register|thread_local|struct|class|union|enum|signed|unsigned|_Atomic|operator|return|sizeof|alignas|alignof)$"
}
# A preprocessor line states the SPELLING of the type, not an object: byte_ring.h's
# `#define KOS_ATOMIC_U32 std::atomic<uint32_t>` would otherwise donate `_Atomic` as a name.
/^[ \t]*#/ { next }
# A type alias names a TYPE. Harvesting it teaches a name no object bears.
/(^|[^A-Za-z0-9_])(typedef|using)([^A-Za-z0-9_]|$)/ { next }
{
    line = $0
    while (match(line, AT)) {
        after = substr(line, RSTART + RLENGTH, 1)
        rest = substr(line, RSTART + RLENGTH)
        line = rest
        # A type token that runs straight on into more identifier characters was only a
        # PREFIX of a longer name, not this declaration's type.
        if (after ~ /^[A-Za-z0-9_]$/) { continue }
        sub(/^[ \t*&]+/, "", rest)
        while (sub(/^(const|volatile|restrict|__restrict)[ \t*&]+/, "", rest)) { }
        if (!match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) { continue }
        nm = substr(rest, RSTART, RLENGTH)
        end = substr(rest, RLENGTH + 1, 1)
        if (nm ~ KEYWORD) { continue }
        # A declarator ends on one of these. Anything else (`::`, `(`, an operator) means what
        # was read is not the name of an object declared here. `(` is excluded on purpose: a
        # function RETURNING an atomic would otherwise donate its own name.
        if (end == "" || end ~ /^[ \t;={[]$/) {
            printf("%s\t%d\t%s\tmember\n", F, FNR, nm)
        }
        else if (end ~ /^[,)]$/) {
            printf("%s\t%d\t%s\tparam\n", F, FNR, nm)
        }
    }
}
