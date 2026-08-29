#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The positive control for tools/check-x86_64-no-got.sh: purpose-built inputs that MUST be
# refused, and clean ones that must not be.
#
#   check_x86_64_no_got_selftest.sh <guard> <readelf> <cc> <ar> <cflags...>
#
# The guard is an absence-assertion, so every way of breaking it is silent:
# a grep on the full spelling `R_X86_64_REX_GOTPCRELX` matches nothing because readelf
# TRUNCATES the type column to `R_X86_64_REX_GOTP`; a French binutils prints `Fichier:` where
# the member-name parse reads `File:`; and an archive whose members readelf could not open at
# all comes back as clean. Each of the four arms below is one of those.
#
# The guard is fed an ARCHIVE as well as an object: an object-only invocation cannot see a GOT
# reference that lives in an archive member.
#
# POSIX sh (dash-clean).

set -u
. "$(dirname "$0")/../lib/gate.sh"

GUARD="${1:?usage: check_x86_64_no_got_selftest.sh <guard> <readelf> <cc> <ar> <cflags...>}"
READELF="${2:?}"
CC="${3:?}"
AR="${4:?}"
shift 4

[ -x "$GUARD" ] || fail "no executable guard at $GUARD"

scratch_dir

# A WEAK undefined function whose address is taken. That is the shape the toolchain cannot
# fix with visibility: a PC-relative form cannot encode "absolute zero", so gcc reaches it
# GOT-indirect whatever -fvisibility says.
cat > "$TMP/dirty.c" <<'EOF'
extern void kos_selftest_absent(void) __attribute__((weak, visibility("hidden")));
void* kos_selftest_take(void) { return (void*)kos_selftest_absent; }
EOF
cat > "$TMP/clean.c" <<'EOF'
static int kos_selftest_state;
int kos_selftest_read(void) { return kos_selftest_state; }
EOF

for n in dirty clean; do
    "$CC" "$@" -c -o "$TMP/$n.o" "$TMP/$n.c" || fail "$CC could not compile $TMP/$n.c"
done
"$AR" rcs "$TMP/dirty.a" "$TMP/dirty.o" || fail "$AR could not write $TMP/dirty.a"
"$AR" rcs "$TMP/mixed.a" "$TMP/clean.o" "$TMP/dirty.o" || fail "$AR could not write mixed.a"
"$AR" rcs "$TMP/clean.a" "$TMP/clean.o" || fail "$AR could not write $TMP/clean.a"

# The control on the CONTROL: an input built to carry the relocation and not carrying one
# would make every refusal arm below pass for the wrong reason.
if ! LC_ALL=C "$READELF" -r "$TMP/dirty.o" | grep -q 'GOT'; then
    fail "$TMP/dirty.o carries no GOT relocation, so the refusal arms would assert nothing"
fi
if LC_ALL=C "$READELF" -r "$TMP/clean.o" | grep -q 'GOT'; then
    fail "$TMP/clean.o carries a GOT relocation, so the acceptance arms would assert nothing"
fi

refuses() { # <what> <input>...
    if "$GUARD" "$READELF" "$2" "$3" >"$TMP/out" 2>&1; then
        echo "FAIL: the guard ACCEPTED $1" >&2
        sed -n '1,5p' "$TMP/out" >&2
        exit 1
    fi
    echo "  refused: $1"
}

accepts() { # <what> <input>...
    if ! "$GUARD" "$READELF" "$2" >"$TMP/out" 2>&1; then
        echo "FAIL: the guard REFUSED $1" >&2
        sed -n '1,5p' "$TMP/out" >&2
        exit 1
    fi
    echo "  accepted: $1"
}

refuses "a bare object carrying a GOT relocation" "$TMP/dirty.o" "$TMP/clean.o"
refuses "an ARCHIVE whose only member carries one" "$TMP/dirty.a" "$TMP/clean.o"
refuses "an ARCHIVE whose SECOND member carries one" "$TMP/mixed.a" "$TMP/clean.o"
accepts "a clean object and a clean archive" "$TMP/clean.o"
accepts "a clean archive" "$TMP/clean.a"

# The tool-alive control. An input readelf cannot read produces no relocation lines, which an
# absence-assertion reads as clean; the guard counts ELF headers to catch exactly that.
printf 'not an object at all\n' > "$TMP/notelf.bin"
if "$GUARD" "$READELF" "$TMP/notelf.bin" >"$TMP/out" 2>&1; then
    echo "FAIL: the guard ACCEPTED a file readelf cannot read, so a dead tool reads as clean" >&2
    sed -n '1,5p' "$TMP/out" >&2
    exit 1
fi
echo "  refused: a file readelf cannot read"

# An EMPTY archive is the same trap one level down: it has members to report on and none.
"$AR" rcs "$TMP/empty.a" || fail "$AR could not write an empty archive"
if "$GUARD" "$READELF" "$TMP/empty.a" >"$TMP/out" 2>&1; then
    echo "FAIL: the guard ACCEPTED an archive with no members" >&2
    sed -n '1,5p' "$TMP/out" >&2
    exit 1
fi
echo "  refused: an archive with no members"

echo "PASS: the no-got guard refuses a GOT relocation in an object, in either member of an archive, and a dead tool"
