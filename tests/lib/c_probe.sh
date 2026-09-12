# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The standalone C11 compile two header gates share. SOURCED, never executed, and only after
# gate.sh, whose fail() it uses:
#   . "$(dirname "$0")/../lib/gate.sh"
#   KOS_C_FORM=quoted
#   KOS_C_CC="$CC"
#   . "$(dirname "$0")/../lib/c_probe.sh"
#   compile_as_c "$subject" "$err" -I"$root" -DX="a b"
#   kos_c_prove_missing_include "$TMP" -I"$root"
#
# It carries the COMPILE and nothing else. Which headers to compile, and what a refusal
# means for the tree, are the gates' own, and they assert different things over different
# corpora: check_c_headers.sh walks the source tree with the board's cross compiler,
# check_public_headers.sh walks an installed package with the consumer's.
#
# KOS_C_FORM is how the include is spelled, and it decides what the argument to each function
# below MEANS. `quoted` takes a PATH, resolved against the working directory, which is the
# repo root for a source-tree walk. `angled` takes an INCLUDE SPELLING, resolved against the
# -I roots, which is what an installed package advertises. Neither form is a substitute for
# the other: a quoted include that resolves beside its includer is not reachable by spelling,
# and a package's advertised spelling has no path in the consumer's tree.
#
# The -I roots and any -D the subject is compiled with are TRAILING ARGUMENTS, one shell word
# each, never a string the callee re-splits. A root or a define holding a space arrives whole,
# and one holding a glob character keeps its value instead of taking the name of whatever
# file happens to sit in the working directory, which no verdict below would have reported.
#
# THE TU COMES FROM STDIN in both forms, so nothing resolves against a scratch directory.
#
# -fsyntax-only proves a C TU parses and type-checks against the header. Whether the consumer
# then finds a symbol is a link property and is outside this file.

KOS_C_STD=c11
KOS_C_FLAGS="-std=$KOS_C_STD -ffreestanding -fsyntax-only"
# A SECOND pass over the same subject. The main pass is not pedantic, so a construct that is
# a GNU extension rather than ISO C11 passes it exactly as it passes a consumer build with
# extensions on; this pass is what fails it before a consumer compiling strictly conforming
# C11 does.
KOS_C_PEDFLAGS="$KOS_C_FLAGS -pedantic-errors"

kos_c_include() { # <subject> -> the one #include line the TU is
    case "${KOS_C_FORM:-}" in
        quoted) printf '#include "%s"\n' "$1" ;;
        angled) printf '#include <%s>\n' "$1" ;;
        *) fail "KOS_C_FORM is [${KOS_C_FORM:-}], which is neither quoted nor angled, so
      no TU can be written and every verdict below would be the probe's own" ;;
    esac
}

# The two wordings a compiler uses when an #include did not resolve: gcc says `No such file
# or directory`, clang says `file not found`. gate.sh's LC_ALL=C is what keeps either in
# English. A wording this does not know turns a header the compiler never judged into a claim
# that the header is bad C, and the fix a gate then prints is to edit a header that is fine.
kos_c_missing_include() { # <stderr file>; 0 when it names an #include that did not resolve
    grep -qE 'No such file or directory|file not found' "$1"
}

# 0 valid C11, 1 a language error, 2 an #include was not found. The three are not one
# verdict: a header the compiler could not judge is UNKNOWN, not invalid.
compile_as_c() { # <subject> <stderr file> <compiler argument>...
    _cc_subject="$1"
    _cc_err="$2"
    shift 2
    # shellcheck disable=SC2086
    if kos_c_include "$_cc_subject" | "$KOS_C_CC" $KOS_C_FLAGS "$@" -x c - 2>"$_cc_err"; then
        return 0
    fi
    if kos_c_missing_include "$_cc_err"; then
        return 2
    fi
    return 1
}

# The classification above, proven BOTH ways beside a gate's other instrument proofs. The two
# recorded diagnostics are read on every run whatever compiler the gate was handed, so neither
# wording can rot unnoticed; the compile that follows is the real one and pins the wording of
# the compiler actually in hand.
kos_c_prove_missing_include() { # <scratch dir> <compiler argument>...
    _cc_d="$1"
    shift
    printf 'fatal error: kickos/absent.h: No such file or directory\n' >"$_cc_d/miss_gcc.err"
    printf "fatal error: 'kickos/absent.h' file not found\n" >"$_cc_d/miss_clang.err"
    printf "error: unknown type name 'namespace'\n" >"$_cc_d/miss_lang.err"
    kos_c_missing_include "$_cc_d/miss_gcc.err" \
        || fail "gcc's wording for an unresolved #include is not recognised, so a header whose
      include root is missing would be reported as invalid C instead of as UNKNOWN"
    kos_c_missing_include "$_cc_d/miss_clang.err" \
        || fail "clang's wording for an unresolved #include is not recognised, so a header
      whose include root is missing would be reported as invalid C instead of as UNKNOWN"
    if kos_c_missing_include "$_cc_d/miss_lang.err"; then
        fail "a plain language error reads as an unresolved #include, so a header that really
      is not C would be excused as UNKNOWN and never reported"
    fi
    compile_as_c kos_probe_absent.h "$_cc_d/miss.err" "$@"
    _cc_rc=$?
    [ "$_cc_rc" -eq 2 ] || fail "an #include that cannot resolve was classified $_cc_rc and not
      2, so this compiler words a missing include in a way neither spelling above knows"
}

# The trailing typedef keeps the TU non-empty: -pedantic-errors forbids an empty translation
# unit, which is what a macros-only header would otherwise produce.
compile_pedantic_c() { # <subject> <stderr file> <compiler argument>...
    _cc_subject="$1"
    _cc_err="$2"
    shift 2
    # shellcheck disable=SC2086
    { kos_c_include "$_cc_subject"; printf 'typedef int kos_gate_pedantic_tu;\n'; } \
        | "$KOS_C_CC" $KOS_C_PEDFLAGS "$@" -x c - 2>"$_cc_err"
}
