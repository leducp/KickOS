#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The positive control for the two x86_64 relocation guards, tools/check-x86_64-no-got.sh and
# tools/check-x86_64-weak-undef.sh: purpose-built inputs each MUST refuse, and the clean ones
# and the admitted vtable word it must not.
#
#   check_x86_64_weak_undef_selftest.sh <no-got-guard> <weak-undef-guard> <readelf> <cc> <ar>
#                                       <cflags...>
#
# Both are absence-assertions, so every way of breaking one is silent. For the no-got guard: a
# grep on the full spelling `R_X86_64_REX_GOTPCRELX` matches nothing because readelf TRUNCATES
# the type column to `R_X86_64_REX_GOTP`; a French binutils prints `Fichier:` where the
# member-name parse reads `File:`; and an archive whose members readelf could not open at all
# comes back as clean. Its plants are C under the image's own flags, since the GOT form is the
# compiler's choice; it is fed an ARCHIVE as well as an object, an object-only invocation
# seeing no GOT reference that lives in a member.
#
# For the weak-undef guard, every form a weak undefined reference takes that links silently at
# the default image base is planted on its own: a call, a PC-relative lea and an absolute data
# word, each naming a symbol other than __cxa_pure_virtual. The admission is planted beside
# them, a pure virtual vtable word, and the same symbol reached any other way, which the
# admission must not cover. Its plants are assembly, so each relocation is exactly the one
# named and no compiler choice stands between the control and what it plants.
#
# POSIX sh (dash-clean).

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_x86_64_weak_undef_selftest.sh <no-got-guard> <weak-undef-guard> <readelf> <cc> <ar> <cflags...>"
NO_GOT="${1:?$_usage}"
WEAK_UNDEF="${2:?$_usage}"
READELF="${3:?$_usage}"
CC="${4:?$_usage}"
AR="${5:?$_usage}"
shift 5

[ -x "$NO_GOT" ] || fail "no executable guard at $NO_GOT"
[ -x "$WEAK_UNDEF" ] || fail "no executable guard at $WEAK_UNDEF"

scratch_dir

# --- the no-got plants --------------------------------------------------------
# A WEAK undefined function whose address is taken. That is the shape the toolchain cannot
# fix with visibility: a PC-relative form cannot encode "absolute zero", so gcc reaches it
# GOT-indirect whatever -fvisibility says.
cat > "$TMP/dirty.c" <<'EOF'
extern void kos_selftest_absent(void) __attribute__((weak, visibility("hidden")));
void* kos_selftest_take(void) { return (void*)kos_selftest_absent; }
EOF
cat > "$TMP/cclean.c" <<'EOF'
static int kos_selftest_state;
int kos_selftest_read(void) { return kos_selftest_state; }
EOF
# A locally defined thread-local, which names no GOT: an R_X86_64_TPOFF32, which q35 refuses.
cat > "$TMP/tls.c" <<'EOF'
static __thread int kos_selftest_tls;
int kos_selftest_tls_read(void) { return kos_selftest_tls; }
EOF
for n in dirty cclean tls; do
    "$CC" "$@" -c -o "$TMP/$n.o" "$TMP/$n.c" || fail "$CC could not compile $TMP/$n.c"
done

# --- the weak-undef plants ----------------------------------------------------
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

"$AR" rcs "$TMP/dirty.a" "$TMP/dirty.o" || fail "$AR could not write dirty.a"
"$AR" rcs "$TMP/gmixed.a" "$TMP/cclean.o" "$TMP/dirty.o" || fail "$AR could not write gmixed.a"
"$AR" rcs "$TMP/cclean.a" "$TMP/cclean.o" || fail "$AR could not write cclean.a"
"$AR" rcs "$TMP/wmixed.a" "$TMP/clean.o" "$TMP/call.o" || fail "$AR could not write wmixed.a"
"$AR" rcs "$TMP/clean.a" "$TMP/clean.o" "$TMP/vtable.o" || fail "$AR could not write clean.a"
"$AR" rcs "$TMP/empty.a" || fail "$AR could not write an empty archive"
printf 'not an object at all\n' > "$TMP/notelf.bin"

# The control on the CONTROL: a plant that carries no relocation of the kind it plants would
# make its refusal arm pass for the wrong reason.
carries() { # <ere> <object> <what the arm then asserts>
    LC_ALL=C "$READELF" -rW "$2" | grep -qE "$1" || fail "$2 carries no /$1/ relocation, so $3"
}
carries 'GOT' "$TMP/dirty.o" "the GOT refusal arms would assert nothing"
carries 'TPOFF' "$TMP/tls.o" "the TLS refusal arm would assert nothing"
for n in call lea word vtable purecall; do
    carries 'R_X86_64_' "$TMP/$n.o" "its arm would assert nothing"
done
if LC_ALL=C "$READELF" -rW "$TMP/cclean.o" | grep -q 'GOT'; then
    fail "$TMP/cclean.o carries a GOT relocation, so the acceptance arms would assert nothing"
fi

refuses() { # <guard> <what> <input>...
    _guard="$1"
    _what="$2"
    shift 2
    if "$_guard" "$READELF" "$@" >"$TMP/out" 2>&1; then
        echo "FAIL: $(basename "$_guard") ACCEPTED $_what" >&2
        sed -n '1,5p' "$TMP/out" >&2
        exit 1
    fi
    echo "  $(basename "$_guard") refused: $_what"
}

accepts() { # <guard> <what> <input>...
    _guard="$1"
    _what="$2"
    shift 2
    if ! "$_guard" "$READELF" "$@" >"$TMP/out" 2>&1; then
        echo "FAIL: $(basename "$_guard") REFUSED $_what" >&2
        sed -n '1,5p' "$TMP/out" >&2
        exit 1
    fi
    echo "  $(basename "$_guard") accepted: $_what"
}

refuses "$NO_GOT" "a bare object carrying a GOT relocation" "$TMP/dirty.o" "$TMP/cclean.o"
refuses "$NO_GOT" "an ARCHIVE whose only member carries one" "$TMP/dirty.a" "$TMP/cclean.o"
refuses "$NO_GOT" "an ARCHIVE whose SECOND member carries one" "$TMP/gmixed.a" "$TMP/cclean.o"
refuses "$NO_GOT" "a local-exec TLS relocation, which names no GOT" "$TMP/tls.o" "$TMP/cclean.o"
accepts "$NO_GOT" "a clean object" "$TMP/cclean.o"
accepts "$NO_GOT" "a clean archive" "$TMP/cclean.a"

refuses "$WEAK_UNDEF" "a call to an undefined weak function" "$TMP/call.o"
refuses "$WEAK_UNDEF" "a PC-relative lea of an undefined weak symbol" "$TMP/lea.o"
refuses "$WEAK_UNDEF" "an absolute data word naming an undefined weak symbol" "$TMP/word.o"
refuses "$WEAK_UNDEF" "__cxa_pure_virtual reached by a call rather than a vtable word" \
    "$TMP/purecall.o"
refuses "$WEAK_UNDEF" "an ARCHIVE whose SECOND member calls an undefined weak function" \
    "$TMP/wmixed.a"
accepts "$WEAK_UNDEF" "a pure virtual vtable word and a clean object" "$TMP/vtable.o" "$TMP/clean.o"
accepts "$WEAK_UNDEF" "a weak reference in a section that is never loaded" "$TMP/debugonly.o"
accepts "$WEAK_UNDEF" "an archive of a clean object and a vtable word" "$TMP/clean.a"

# The tool-alive controls. An input readelf cannot read produces no relocation lines, which an
# absence-assertion reads as clean, and an EMPTY archive is the same trap one level down; each
# guard counts ELF headers to catch exactly that.
for g in "$NO_GOT" "$WEAK_UNDEF"; do
    refuses "$g" "a file readelf cannot read" "$TMP/notelf.bin"
    refuses "$g" "an archive with no members" "$TMP/empty.a"
done

echo "PASS: the no-got guard refuses a GOT relocation in an object or either member of an
  archive and a TLS relocation; the weak-undef guard refuses a weak call, lea and word, in an
  object or an archive member, and admits only the pure virtual vtable word; both refuse a
  dead tool"
