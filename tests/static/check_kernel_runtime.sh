#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The kernel calls its own runtime. memcpy, memset, strlen and the formatter are called from
# BOTH halves of the image, and a global symbol has one value: kernel-side, and EL0's call is a
# high address it cannot reach; app-side, and the kernel calls text that carries
# privileged-execute-never once EL0 can reach it. So the kernel links its own copies under the
# private names kernel/include/kickos/kruntime.h declares, and no archive holding kernel text
# may name an ordinary one.
#
# Most kernel-side references have no call in the .cc at all: the COMPILER emits them from
# ordinary constructions such as `ThreadAttr attr;` and `*d = Domain{}`, and which sites emit
# one differs per arch. Those are rewritten after `ar` by kickos_privatise_runtime()
# (cmake/kickos.cmake, map in cmake/kernel_runtime.syms), so what this gate reads is the archive
# the linker will read, after the rewrite.
#
# A RED RUN MEANS THE REWRITE DID NOT REACH THIS ARCHIVE, and the fix is a
# kickos_privatise_runtime() call for the target that built it, or the missing name in
# cmake/kernel_runtime.syms. It is NOT a licence to de-type the construction that emitted
# the reference: a struct's default member initialisers are its contract, and no compiler
# flag suppresses the libcall.
#
# The two formatter names are refused too and are deliberately out of the rewrite map: no
# compiler emits them, so a kernel-side reference to one is an explicit call and belongs
# corrected in the source.
#
# SCOPE: the archives that hold kernel text (kernel, arch, chip), on the boards where a
# translating backend splits the image. kickos_lib holds the app's copies and is not scanned,
# and the system/ provider and driver archives are app-side, so both keep the ordinary names. A
# REGION backend serves both privilege levels from one text mapping, where kickos/kruntime.h
# aliases the app's names.
#
# The two bit-count helpers are here for the same reason. rv64imac names no bit-manipulation
# extension, so __builtin_clzll and __builtin_ctzll lower to libgcc calls; libgcc is app-side,
# and a kernel-side definition under the ORDINARY name is the one the whole link sees, so an
# app-side libgcc member needing it (soft-float calls __clzdi2) then makes a call the halves
# cannot carry. The kernel links its own under private names
# (cmake/kernel_runtime_rv64imac.syms).
#
# The list is closed on purpose, so a hit names one symbol: a compiler that starts emitting
# `__aeabi_memclr` or `memchr` needs the name added here and to the rewrite map.
#
# usage: check_kernel_runtime.sh <nm> <archive>...

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The awk below keys on nm's symbol-type letters.
export LC_ALL=C

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <nm> <archive>..." >&2
    exit 2
fi

NM="$1"; shift

command -v "$NM" >/dev/null 2>&1 || fail "nm not found: $NM"
[ "$#" -gt 0 ] || fail "no archives given (guard would pass vacuously)"

scratch_dir

# Every name lib/libc/string.cc and lib/libc/fmt.cc define, plus the two _chk wrappers and
# the two BSD spellings a libc may lower a call to. The kernel's own names are the same
# words with the k prefix, so a hit is always one substitution away from correct.
BANNED='memcpy memset memmove memcmp bcmp bzero strlen strnlen __memcpy_chk __memset_chk kvsnprintf ksnprintf __clzdi2 __ctzdi2'

# The RX ABI prefixes every C identifier with an underscore, so both spellings of each name
# are refused rather than the gate passing vacuously on that one board. Expanded HERE and not
# inside the scanner, so the self-test below can hand the scanner one spelling at a time.
banned_both() { # <space-separated names>
    _out="$1"
    for _b in $1; do
        _out="$_out _$_b"
    done
    printf '%s' "$_out"
}
BANNED_ALL="$(banned_both "$BANNED")"

# nm -A prefixes each line with `<path>:<member>:`, and an UNDEFINED symbol has no address,
# so the record is exactly three fields: prefix, `U` (or `w` for an undefined weak, which
# binds the same way), name. A defined symbol carries its address glued to that prefix, so
# its type letter never lands in $2 as U.
scan_refs() { # <symfile> <banned names> <archive label>
    awk -v ban="$2" -v arch="$3" '
    BEGIN {
        n = split(ban, b, " ")
        for (i = 1; i <= n; i++) { bad[b[i]] = 1 }
    }
    NF == 3 && ($2 == "U" || $2 == "w") {
        total++
        if (!($3 in bad)) { next }
        loc = $1
        sub(/:$/, "", loc)
        k = split(loc, p, ":")
        printf "HIT %s(%s) %s\n", arch, p[k], $3
    }
    END { printf "REFS %d\n", total + 0 }' "$1"
}

# --- self-test: prove every clause of the rule, one control per clause ---------
# Each control below is a MINIMAL PAIR: the positive and the negative differ in one property
# only, and every expected count is exact. One banned name per reason it is on the list, in
# the two record shapes that carry an undefined reference.
cat > "$TMP/ctl.pos.sym" <<'EOF'
libkickos_kernel.a:task.cc.obj:                 U memcpy
libkickos_kernel.a:sched.cc.obj:                 w memset
libkickos_kernel.a:rx.cc.obj:                 U _strlen
libkickos_kernel.a:print.cc.obj:                 U kvsnprintf
libkickos_kernel.a:guard.cc.obj:                 U __memcpy_chk
libkickos_kernel.a:bits.cc.obj:                 U __clzdi2
EOF

# Every line a reference the gate must NOT report, each differing from a positive in one
# property: the type letter, the record shape, or the name.
cat > "$TMP/ctl.neg.sym" <<'EOF'
libkickos_kernel.a:kstring.cc.obj:0000000000000000 T memcpy
libkickos_kernel.a:kstring.cc.obj:0000000000000010 W memset
libkickos_kernel.a:kstring.cc.obj:0000000000000020 t memcpy
libkickos_kernel.a:tls.cc.obj:                 U kmemcpy
libkickos_kernel.a:tls.cc.obj:                 U memchr
libkickos_kernel.a:tls.cc.obj:                 U memcpyx
libkickos_kernel.a:tls.cc.obj:                 U arch_idle_wait
EOF

ctl_hits() { # <symfile> [banned]
    _ban="$BANNED_ALL"
    if [ "$#" -ge 2 ]; then _ban="$2"; fi
    scan_refs "$1" "$_ban" ctl.a | grep -c '^HIT ' || :
}
ctl_refs() { # <symfile>
    scan_refs "$1" "$BANNED_ALL" ctl.a | awk '/^REFS /{ print $2 }'
}

POS="$(ctl_hits "$TMP/ctl.pos.sym")"
[ "$POS" -eq 6 ] || fail "the scanner found $POS of 6 planted app-runtime references; it would miss a real one"

if [ "$(ctl_hits "$TMP/ctl.neg.sym")" -ne 0 ]; then
    scan_refs "$TMP/ctl.neg.sym" "$BANNED_ALL" ctl.a | sed 's/^/      /' >&2
    fail "the scanner reported a DEFINED runtime name, a local, a private k-name or a name off
      the list; the gate would cry wolf and be switched off"
fi

# EACH negative on its own, so a control that is silent for the WRONG reason is visible. A
# whole-file zero cannot tell "every clause works" from "one clause swallowed the file".
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$TMP/ctl.one.sym"
    n="$(ctl_hits "$TMP/ctl.one.sym")"
    [ "$n" -eq 0 ] || fail "negative control $i reports: $line"
done < "$TMP/ctl.neg.sym"
[ "$i" -eq 7 ] || fail "$i negative control(s) ran, expected 7"

# The reference TALLY, which is the gate's vacuity floor. Only the four undefined records of
# the negative file count, so a scanner that stopped reading records would show a smaller
# number here while every absence-assertion above still passed.
[ "$(ctl_refs "$TMP/ctl.pos.sym")" -eq 6 ] \
    || fail "the scanner tallied $(ctl_refs "$TMP/ctl.pos.sym") of 6 undefined reference(s) in the positive controls"
[ "$(ctl_refs "$TMP/ctl.neg.sym")" -eq 4 ] \
    || fail "the scanner tallied $(ctl_refs "$TMP/ctl.neg.sym") of 4 undefined reference(s) in the negative controls"
# A file of DEFINED symbols only must tally zero, which is what the vacuity refusal below
# keys on: without it, wrong nm output and a clean archive read the same.
printf '%s\n' 'libkickos_kernel.a:kstring.cc.obj:0000000000000000 T kmemcpy' > "$TMP/ctl.none.sym"
[ "$(ctl_refs "$TMP/ctl.none.sym")" -eq 0 ] \
    || fail "a corpus with no undefined symbol tallied a reference, so the vacuity refusal can never fire"

# The member name comes off the LAST colon-separated field of the nm prefix, so a build path
# holding a colon must not shift it.
printf '%s\n' 'weird:path/libkickos_kernel.a:task.cc.obj:                 U memcpy' > "$TMP/ctl.colon.sym"
scan_refs "$TMP/ctl.colon.sym" "$BANNED_ALL" ctl.a | grep -qxF 'HIT ctl.a(task.cc.obj) memcpy' \
    || fail "the member name was not read off the last field of the nm prefix: $(scan_refs "$TMP/ctl.colon.sym" "$BANNED_ALL" ctl.a | grep '^HIT ')"

# Change the list the scanner is given and the count over the control corpus must MOVE by an
# EXACT amount: that is what proves each control was a near miss rather than slack.
mutate() { # <what> <symfile> <banned> <expect>
    _got="$(ctl_hits "$2" "$3")"
    [ "$_got" -eq "$4" ] || fail "with the $1 the scanner reported $_got hit(s), expected $4;
      the controls for it are not near misses and prove nothing"
}
# Without the RX spellings only the underscored control goes quiet, which is what proves that
# arm is load-bearing and the gate is not vacuous on that board.
mutate "RX spellings dropped" "$TMP/ctl.pos.sym" "$BANNED" 5
# The header says a compiler that starts emitting memchr needs the name added here; with it
# added, the control that proves the list is CLOSED must newly report.
mutate "list widened by memchr" "$TMP/ctl.neg.sym" "$(banned_both "$BANNED memchr")" 1
# An empty list reports nothing while the tally above still counts every record, which is
# what separates "no banned reference" from "the corpus went unread".
mutate "list emptied" "$TMP/ctl.pos.sym" 'KICKOS_NO_SUCH_SYMBOL' 0

# --- the archives -------------------------------------------------------------
: > "$TMP/hits"
for A in "$@"; do
    [ -f "$A" ] || fail "archive not found: $A"
    # Positive control: every KickOS archive carries code, so a run that saw no text symbol
    # saw nothing and the absence-assertion below would be vacuous.
    tool_out "$TMP/sym" '[[:space:]][TtWw][[:space:]]' "$NM" -A "$A"
    scan_refs "$TMP/sym" "$BANNED_ALL" "$(basename "$A")" >> "$TMP/hits"
done

refs="$(awk '/^REFS /{ t += $2 } END { print t + 0 }' "$TMP/hits")"
[ "$refs" -gt 0 ] || fail "no undefined symbol in any of the $# archive(s) given: wrong nm, \
wrong files, or a link model this gate cannot read (guard would pass vacuously)"

if grep -q '^HIT ' "$TMP/hits"; then
    echo "FAIL: archive(s) holding kernel text name the APP's runtime" >&2
    echo "      Kernel text runs from the privileged half and may not call app text," >&2
    echo "      which carries privileged-execute-never. Either the target that built this" >&2
    echo "      archive has no kickos_privatise_runtime() call (cmake/kickos.cmake), or the" >&2
    echo "      name below is missing from cmake/kernel_runtime.syms, or it is an explicit" >&2
    echo "      call in the source that should read the kickos/kruntime.h name instead." >&2
    awk '/^HIT /{ printf "        %s names %s\n", $2, $3 }' "$TMP/hits" >&2
    exit 1
fi

echo "PASS: $# archive(s) holding kernel text, $refs undefined reference(s), none to the app runtime"
