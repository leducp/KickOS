#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The include-guard rule, both halves: never `#pragma once`, and the guard macro DERIVES
# from the project prefix plus the file path.
#
# Six directories under arch/ share KICKOS_ARCH_CONTEXT_H for their own
# kickos/arch/context.h, one on the include path per build, so the derivation is
# include-relative rather than repo-path relative.
#
# Run from the repo root, no arguments: tests/static/check_include_guards.sh
#
# A guard is `#ifndef G` as the FIRST preprocessor directive, `#define G` on the very next
# line, and `#endif` last. A header this parse cannot read off is REFUSED by name rather than
# read clean. G derives from the path:
#
#   1. drop a trailing `.in`.
#   2. if the path holds an `include/` component, keep only what follows the LAST one. That
#      is the spelling a consumer writes in `#include <...>`.
#   3. a driver's private header, `system/driver/<chip>/<module>/<file>` with nothing between
#      the module directory and the file: the name is `kickos/driver/<chip>/<file>`. The
#      module directory drops out, the chip stays. A driver header nested DEEPER than that
#      falls to rule 4.
#   4. otherwise the header is private to its directory: keep the whole repo path and prefix
#      `kickos/`.
#   5. uppercase; `/`, `.` and `-` each become `_`.
#
# SCOPE. `#pragma once` is scanned over every tracked C/C++ source, the guard spelling over
# tracked headers. What the scan reads is the path text and the header's own first two
# directives. Outside it: whether a directory named `include` is on any include path, which
# CMake alone knows; whether two headers that can meet carry the same guard, which nothing
# here compares; which `#ifndef` the closing `#endif` belongs to, presence being all that is
# read; and whether anything includes the header at all.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

# --- leg 1: no `#pragma once`, over every tracked C/C++ file ------------------
git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.S' '*.inc' '*.h.in' \
    > "$TMP/sources" || fail "git ls-files failed"
require_nonempty "$TMP/sources" "git ls-files matched no C/C++ file; the pragma scan would pass vacuously"
SOURCES="$(wc -l < "$TMP/sources" | tr -d ' ')"

: > "$TMP/pragma"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    grep -an '^[[:space:]]*#[[:space:]]*pragma[[:space:]][[:space:]]*once' "$f" \
        | awk -v F="$f" '{ print F ":" $0 }' >> "$TMP/pragma"
done < "$TMP/sources"

# --- leg 2: the headers, their guards, and the spelling the path dictates -----
git ls-files -- '*.h' '*.hh' '*.hpp' '*.inc' '*.h.in' > "$TMP/headers" \
    || fail "git ls-files failed"
require_nonempty "$TMP/headers" "git ls-files matched no header; every check below would pass vacuously"
HEADERS="$(wc -l < "$TMP/headers" | tr -d ' ')"

expected_guard() { # <tracked path> -> the macro the rule dictates
    _p="${1%.in}"
    case "$_p" in
        # An include root anywhere on the path wins, keeping a driver's PUBLIC header on the
        # consumer spelling rather than on rule 3.
        */include/*) _rel="${_p##*/include/}" ;;
        include/*)   _rel="${_p#include/}" ;;
        system/driver/*/*/*)
            _t="${_p#system/driver/}"
            _chip="${_t%%/*}"
            _file="${_t#*/}"
            _file="${_file#*/}"
            case "$_file" in
                */*) _rel="kickos/$_p" ;;
                *)   _rel="kickos/driver/$_chip/$_file" ;;
            esac ;;
        *)           _rel="kickos/$_p" ;;
    esac
    # Explicit ranges under LC_ALL=C, or a Turkish-locale box produces a dotless I.
    printf '%s' "$_rel" | LC_ALL=C tr 'abcdefghijklmnopqrstuvwxyz/.-' 'ABCDEFGHIJKLMNOPQRSTUVWXYZ___'
}

# A derivation returning the empty string reports every header in the tree, one returning its
# argument unchanged reports none, so it is proven on known paths first.
[ "$(expected_guard user/include/kickos/sys/abi.h)" = "KICKOS_SYS_ABI_H" ] \
    || fail "the guard derivation is broken on an include-rooted header"
[ "$(expected_guard arch/arm/common/mpu.h)" = "KICKOS_ARCH_ARM_COMMON_MPU_H" ] \
    || fail "the guard derivation is broken on a private header"
[ "$(expected_guard kernel/include/kickos/config/cap_width.h.in)" = "KICKOS_CONFIG_CAP_WIDTH_H" ] \
    || fail "the guard derivation is broken on a .in template"
[ "$(expected_guard system/driver/rp2xxx/rpusb/rp_usb_regs.h)" = "KICKOS_DRIVER_RP2XXX_RP_USB_REGS_H" ] \
    || fail "the guard derivation is broken on a driver's private header"
# Both sides of rule 3's boundary. The nested path is synthetic and is the only input here
# that reaches rule 3's fallthrough to rule 4.
[ "$(expected_guard system/driver/xmc4800/xmcuartirq/include/kickos/driver/xmcuartirq.h)" = "KICKOS_DRIVER_XMCUARTIRQ_H" ] \
    || fail "the guard derivation is broken on a driver's public header"
[ "$(expected_guard system/driver/rx72m/rxsci/regs/sci.h)" = "KICKOS_SYSTEM_DRIVER_RX72M_RXSCI_REGS_SCI_H" ] \
    || fail "the guard derivation applies rule 3 below the flat driver shape"

: > "$TMP/findings"
: > "$TMP/refused"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"

    # One awk pass, so the directive and the line after it cannot come from different places.
    GUARD="$(awk '
        /^[[:space:]]*#/ {
            if ($1 != "#ifndef" || NF != 2) { exit 0 }
            g = $2
            if ((getline) <= 0) { exit 0 }
            if ($1 != "#define" || NF != 2 || $2 != g) { exit 0 }
            print g
            exit 0
        }' "$f")"
    if [ -z "$GUARD" ]; then
        printf '%s\n' "$f" >> "$TMP/refused"
        continue
    fi
    LAST="$(grep -v '^[[:space:]]*$' "$f" | tail -n1)"
    case "$LAST" in
        '#endif'*) ;;
        *) printf '%s\n' "$f" >> "$TMP/refused"; continue ;;
    esac

    WANT="$(expected_guard "$f")"
    if [ "$GUARD" != "$WANT" ]; then
        printf '%s: guard is %s, the path dictates %s\n' "$f" "$GUARD" "$WANT" >> "$TMP/findings"
    fi
done < "$TMP/headers"

echo "== checked $HEADERS tracked header(s) for a path-derived guard, $SOURCES tracked C/C++ file(s) for #pragma once =="

RC=0

# A header spelling `#pragma once` has no guard to read either, so it lands in BOTH lists.
# The pragma leg reports first: the rule it broke reads better than a parse failure.
if [ -s "$TMP/pragma" ]; then
    echo "FAIL: #pragma once is not in the standard and its identity test is the file the" >&2
    echo "      implementation resolved, which differs across the five toolchains this tree" >&2
    echo "      builds with. Write a traditional guard derived from the path:" >&2
    sed 's/^/      /' "$TMP/pragma" >&2
    RC=1
fi

if [ -s "$TMP/refused" ]; then
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/refused" | tr -d ' ') header(s) carry no guard this scan can read, so their" >&2
    echo '      verdict is UNKNOWN, not clean. A guard is #ifndef G as the first preprocessor' >&2
    echo '      directive, #define G on the next line, and #endif last:' >&2
    sed 's/^/      /' "$TMP/refused" >&2
    RC=1
fi

if [ -s "$TMP/findings" ]; then
    echo "" >&2
    cat "$TMP/findings" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') guard(s) do not follow the path." >&2
    echo "      The derivation is stated at the top of this script. Rename the guard: the" >&2
    echo "      rule is the project's, and relaxing this gate does not change it." >&2
    RC=1
fi

[ "$RC" -eq 0 ] || exit 1

echo "PASS: no #pragma once, and every header guard derives from its path"
