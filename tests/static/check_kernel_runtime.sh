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

: > "$TMP/hits"
for A in "$@"; do
    [ -f "$A" ] || fail "archive not found: $A"
    # Positive control: every KickOS archive carries code, so a run that saw no text symbol
    # saw nothing and the absence-assertion below would be vacuous.
    tool_out "$TMP/sym" '[[:space:]][TtWw][[:space:]]' "$NM" -A "$A"
    base="$(basename "$A")"
    # nm -A prefixes each line with `<path>:<member>:`, and an UNDEFINED symbol has no
    # address, so the record is exactly three fields: prefix, `U` (or `w` for an undefined
    # weak, which binds the same way), name. A defined symbol carries its address glued to
    # that prefix, so its type letter never lands in $2 as U.
    awk -v ban="$BANNED" -v arch="$base" '
    BEGIN {
        n = split(ban, b, " ")
        # The RX ABI prefixes every C identifier with an underscore, so both spellings of
        # each name are refused rather than the gate passing vacuously on that one board.
        for (i = 1; i <= n; i++) { bad[b[i]] = 1; bad["_" b[i]] = 1 }
    }
    NF == 3 && ($2 == "U" || $2 == "w") {
        total++
        if (!($3 in bad)) { next }
        loc = $1
        sub(/:$/, "", loc)
        k = split(loc, p, ":")
        printf "HIT %s(%s) %s\n", arch, p[k], $3
    }
    END { printf "REFS %d\n", total + 0 }' "$TMP/sym" >> "$TMP/hits"
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
