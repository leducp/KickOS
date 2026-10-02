#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The positive control for tools/check-x86_64-weak-undef.sh: purpose-built inputs that MUST be
# refused, and the admitted vtable word and clean inputs that must not be.
#
#   check_x86_64_weak_undef_selftest.sh <guard> <readelf> <cc> <ar>
#
# Every form a weak undefined reference takes that links silently at the default image base is
# planted on its own: a call, a PC-relative lea and an absolute data word, each naming a symbol
# other than __cxa_pure_virtual. The admission is planted beside them, a pure virtual vtable
# word, and the same symbol reached any other way, which the admission must not cover. The
# inputs are assembly, so each relocation is exactly the one named and no compiler choice
# stands between the control and what it plants.
#
# POSIX sh (dash-clean).

set -u
. "$(dirname "$0")/../lib/gate.sh"

GUARD="${1:?usage: check_x86_64_weak_undef_selftest.sh <guard> <readelf> <cc> <ar>}"
READELF="${2:?}"
CC="${3:?}"
AR="${4:?}"

[ -x "$GUARD" ] || fail "no executable guard at $GUARD"

scratch_dir

plant() { # <name> <assembly>
    printf '%s\n' "$2" > "$TMP/$1.s"
    "$CC" -c -o "$TMP/$1.o" "$TMP/$1.s" || fail "$CC could not assemble $TMP/$1.s"
}

plant call '.text
.weak kos_selftest_wcall
.globl kos_selftest_caller
kos_selftest_caller:
    call kos_selftest_wcall
    ret'
plant lea '.text
.weak kos_selftest_wlea
.globl kos_selftest_taker
kos_selftest_taker:
    leaq kos_selftest_wlea(%rip), %rax
    ret'
plant word '.section .data.kos_selftest,"aw"
.weak kos_selftest_wword
.globl kos_selftest_table
kos_selftest_table:
    .quad kos_selftest_wword'
plant vtable '.section .data.rel.ro._ZTV12kos_selftest,"aw"
.weak __cxa_pure_virtual
.globl _ZTV12kos_selftest
_ZTV12kos_selftest:
    .quad 0
    .quad 0
    .quad __cxa_pure_virtual'
plant purecall '.text
.weak __cxa_pure_virtual
.globl kos_selftest_purecall
kos_selftest_purecall:
    call __cxa_pure_virtual
    ret'
plant debugonly '.text
.globl kos_selftest_plain
kos_selftest_plain:
    ret
.section .debug_kos_selftest,""
.weak kos_selftest_wdebug
    .quad kos_selftest_wdebug'
plant clean '.text
.globl kos_selftest_clean
kos_selftest_clean:
    leaq kos_selftest_local(%rip), %rax
    ret
.data
kos_selftest_local:
    .quad 0'

# The control on the CONTROL: a plant that carries no relocation against its weak symbol would
# make its refusal arm pass for the wrong reason.
for n in call lea word vtable purecall; do
    LC_ALL=C "$READELF" -rW "$TMP/$n.o" | grep -q 'R_X86_64_' \
        || fail "$TMP/$n.o carries no relocation, so its arm would assert nothing"
done

"$AR" rcs "$TMP/mixed.a" "$TMP/clean.o" "$TMP/call.o" || fail "$AR could not write mixed.a"
"$AR" rcs "$TMP/clean.a" "$TMP/clean.o" "$TMP/vtable.o" || fail "$AR could not write clean.a"

refuses() { # <what> <input>...
    _what="$1"
    shift
    if "$GUARD" "$READELF" "$@" >"$TMP/out" 2>&1; then
        echo "FAIL: the guard ACCEPTED $_what" >&2
        sed -n '1,5p' "$TMP/out" >&2
        exit 1
    fi
    echo "  refused: $_what"
}

accepts() { # <what> <input>...
    _what="$1"
    shift
    if ! "$GUARD" "$READELF" "$@" >"$TMP/out" 2>&1; then
        echo "FAIL: the guard REFUSED $_what" >&2
        sed -n '1,5p' "$TMP/out" >&2
        exit 1
    fi
    echo "  accepted: $_what"
}

refuses "a call to an undefined weak function" "$TMP/call.o"
refuses "a PC-relative lea of an undefined weak symbol" "$TMP/lea.o"
refuses "an absolute data word naming an undefined weak symbol" "$TMP/word.o"
refuses "__cxa_pure_virtual reached by a call rather than a vtable word" "$TMP/purecall.o"
refuses "an ARCHIVE whose SECOND member calls an undefined weak function" "$TMP/mixed.a"
accepts "a pure virtual vtable word and a clean object" "$TMP/vtable.o" "$TMP/clean.o"
accepts "a weak reference in a section that is never loaded" "$TMP/debugonly.o"
accepts "an archive of a clean object and a vtable word" "$TMP/clean.a"

# The tool-alive control. An input readelf cannot read produces no relocation lines, which an
# absence-assertion reads as clean; the guard counts ELF headers to catch exactly that.
printf 'not an object at all\n' > "$TMP/notelf.bin"
refuses "a file readelf cannot read" "$TMP/notelf.bin"
"$AR" rcs "$TMP/empty.a" || fail "$AR could not write an empty archive"
refuses "an archive with no members" "$TMP/empty.a"

echo "PASS: the weak-undef guard refuses a weak call, lea and word, in an object or an archive member, admits only the pure virtual vtable word, and refuses a dead tool"
