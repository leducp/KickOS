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

# --- the rule, handed to the scanners, so the self-test and the corpus cannot disagree ---
# Both patterns are BRE: `grep` below is invoked without -E.
PRAGMA_ERE='^[[:space:]]*#[[:space:]]*pragma[[:space:]][[:space:]]*once'
ENDIF_ERE='^#endif'
ADJACENT=1
FIRST_DIRECTIVE=1

pragma_hits() { # <file> <pragma-bre> -> one `<file>:<line>:<text>` record per hit
    grep -an "$2" "$1" | awk -v F="$1" '{ print F ":" $0 }'
}

# One awk pass, so the directive and the line after it cannot come from different places.
# ADJ and FIRST are 1 everywhere but in the self-test's mutation arms.
read_guard() { # <file> <adjacent 0|1> <first-directive 0|1> -> the guard macro, or nothing
    awk -v ADJ="$2" -v FIRST="$3" '
        /^[[:space:]]*#/ {
            if ($1 != "#ifndef" || NF != 2) {
                if (FIRST) { exit 0 }
                next
            }
            g = $2
            do {
                if ((getline) <= 0) { exit 0 }
            } while (!ADJ && NF == 0)
            if ($1 != "#define" || NF != 2 || $2 != g) { exit 0 }
            print g
            exit 0
        }' "$1"
}

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

# `ok`, `refused`, or `guard <found> <dictated>`. The content and the path arrive separately,
# so a planted control can be read against a path it does not live at.
header_verdict() { # <content file> <path> <adjacent 0|1> <first-directive 0|1> <endif-bre>
    _hv_guard="$(read_guard "$1" "$3" "$4")"
    if [ -z "$_hv_guard" ]; then
        printf 'refused\n'
        return
    fi
    _hv_last="$(grep -v '^[[:space:]]*$' "$1" | tail -n1)"
    if ! printf '%s\n' "$_hv_last" | grep -q "$5"; then
        printf 'refused\n'
        return
    fi
    _hv_want="$(expected_guard "$2")"
    if [ "$_hv_guard" != "$_hv_want" ]; then
        printf 'guard %s %s\n' "$_hv_guard" "$_hv_want"
        return
    fi
    printf 'ok\n'
}

# --- self-test: prove every clause of the rule, one control per clause ---------
# Each control is a MINIMAL PAIR against its opposite number, differing in one property only,
# and the counts below are exact and differ per clause.

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

# Leg 1. The three positives differ from the negative beneath them in one property each: the
# `#` being the line's first non-blank, a blank separating `pragma` from `once`, and the
# pragma's own name.
cat > "$TMP/pragma_pos.h" <<'EOF'
#pragma once
   #  pragma   once
#pragma once  /* and a trailing comment */
EOF
cat > "$TMP/pragma_neg.h" <<'EOF'
// #pragma once
 * #pragma once, named in a block comment
#pragma pack(push, 1)
#pragmaonce
#ifndef KICKOS_SYS_ABI_H
EOF

PPOS="$(pragma_hits "$TMP/pragma_pos.h" "$PRAGMA_ERE" | wc -l | tr -d ' ')"
[ "$PPOS" -eq 3 ] || fail "the pragma scan found $PPOS of 3 planted spellings; it would miss real ones"

# EACH negative on its own, so one that is silent for the WRONG reason is visible. A
# whole-file zero cannot tell "three clauses hold" from "one clause swallowed the file".
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$TMP/one.h"
    n="$(pragma_hits "$TMP/one.h" "$PRAGMA_ERE" | wc -l | tr -d ' ')"
    [ "$n" -eq 0 ] || fail "pragma negative control $i reports: $line"
done < "$TMP/pragma_neg.h"
[ "$i" -eq 5 ] || fail "$i pragma negative control(s) ran, expected 5"

# Leg 2. One control per clause of the parse, each read against a path whose dictated guard is
# known, so a verdict of `guard` names both spellings.
: > "$TMP/controls"
control() { # <name> <path the derivation reads> <expected verdict>; the body arrives on stdin
    cat > "$TMP/ctl_$1"
    printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$TMP/controls"
}
ABI=user/include/kickos/sys/abi.h
control p_first_directive "$ABI" refused <<'EOF'
#include <stdint.h>
#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H
#endif
EOF
control p_adjacent "$ABI" refused <<'EOF'
#ifndef KICKOS_SYS_ABI_H

#define KICKOS_SYS_ABI_H
#endif
EOF
control p_same_name "$ABI" refused <<'EOF'
#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_X
#endif
EOF
control p_two_fields "$ABI" refused <<'EOF'
#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H 1
#endif
EOF
control p_hash_space "$ABI" refused <<'EOF'
# ifndef KICKOS_SYS_ABI_H
# define KICKOS_SYS_ABI_H
#endif
EOF
control p_pragma_guard "$ABI" refused <<'EOF'
#pragma once
int kos_abi_version;
EOF
control p_endif_last "$ABI" refused <<'EOF'
#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H
#endif
int kos_after_the_guard;
EOF
control p_endif_indent "$ABI" refused <<'EOF'
#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H
  #endif
EOF
control p_derivation "$ABI" "guard KICKOS_ABI_H KICKOS_SYS_ABI_H" <<'EOF'
#ifndef KICKOS_ABI_H
#define KICKOS_ABI_H
#endif
EOF
control n_canonical "$ABI" ok <<'EOF'
#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H
#endif
EOF
control n_prologue "$ABI" ok <<'EOF'
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H
#endif
EOF
control n_guard_indent "$ABI" ok <<'EOF'
  #ifndef KICKOS_SYS_ABI_H
  #define KICKOS_SYS_ABI_H
#endif
EOF
control n_endif_comment "$ABI" ok <<'EOF'
#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H
#endif // KICKOS_SYS_ABI_H
EOF
control n_endif_blanks "$ABI" ok <<'EOF'
#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H
#endif

EOF
control n_template kernel/include/kickos/config/cap_width.h.in ok <<'EOF'
#ifndef KICKOS_CONFIG_CAP_WIDTH_H
#define KICKOS_CONFIG_CAP_WIDTH_H
#endif
EOF

NREF=0
NGUARD=0
NOK=0
i=0
while IFS="$TAB" read -r name path want; do
    i=$((i + 1))
    got="$(header_verdict "$TMP/ctl_$name" "$path" "$ADJACENT" "$FIRST_DIRECTIVE" "$ENDIF_ERE")"
    [ "$got" = "$want" ] || fail "control $name: the guard scan says '$got', expected '$want'"
    case "$got" in
        refused) NREF=$((NREF + 1)) ;;
        ok)      NOK=$((NOK + 1)) ;;
        *)       NGUARD=$((NGUARD + 1)) ;;
    esac
done < "$TMP/controls"
[ "$i" -eq 15 ] || fail "$i guard control(s) ran, expected 15"
[ "$NREF" -eq 8 ] || fail "the guard scan refused $NREF of 8 unreadable controls; a header it cannot parse would read clean"
[ "$NGUARD" -eq 1 ] || fail "the guard scan reported $NGUARD of 1 planted misnamed guard"
[ "$NOK" -eq 6 ] || fail "the guard scan read $NOK of 6 conforming controls clean; the gate would cry wolf"

# Relax one clause and the count over the control corpus must move to an EXACT number: that
# is what proves the control was a near miss and not slack. Every relaxed spelling below is
# self-test only; the corpus scan never sees one.
NEVER='KICKOS_THIS_PATTERN_MATCHES_NOTHING'
PRAGMA_ERE_NOBLANK='^[[:space:]]*#[[:space:]]*pragma[[:space:]]*once'
PRAGMA_ERE_FLOATING='#[[:space:]]*pragma[[:space:]][[:space:]]*once'
ENDIF_ERE_ANY='^'

pragma_mutation() { # <clause> <file> <pragma-bre> <expected hits>
    _pm="$(pragma_hits "$2" "$3" | wc -l | tr -d ' ')"
    [ "$_pm" -eq "$4" ] || fail "with the $1 clause relaxed the pragma scan found $_pm hit(s) in $2, expected $4;
      the controls for it are not near misses and prove nothing"
}
pragma_mutation whole-pattern   "$TMP/pragma_pos.h" "$NEVER"                0
pragma_mutation mandatory-blank "$TMP/pragma_neg.h" "$PRAGMA_ERE_NOBLANK"   1
pragma_mutation leading-hash    "$TMP/pragma_neg.h" "$PRAGMA_ERE_FLOATING"  2

guard_mutation() { # <clause> <adjacent> <first-directive> <endif-bre> <expected refusals>
    _gm=0
    while IFS="$TAB" read -r name path want; do
        case "$(header_verdict "$TMP/ctl_$name" "$path" "$2" "$3" "$4")" in
            refused) _gm=$((_gm + 1)) ;;
        esac
    done < "$TMP/controls"
    [ "$_gm" -eq "$5" ] || fail "with the $1 clause relaxed the guard scan refused $_gm control(s), expected $5;
      the controls for it are not near misses and prove nothing"
}
guard_mutation adjacency       0 1 "$ENDIF_ERE"     7
guard_mutation first-directive 1 0 "$ENDIF_ERE"     7
guard_mutation endif-last      1 1 "$ENDIF_ERE_ANY" 6

# --- leg 1: no `#pragma once`, over every tracked C/C++ file ------------------
git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.S' '*.inc' '*.h.in' \
    > "$TMP/sources" || fail "git ls-files failed"
require_nonempty "$TMP/sources" "git ls-files matched no C/C++ file; the pragma scan would pass vacuously"
SOURCES="$(wc -l < "$TMP/sources" | tr -d ' ')"

: > "$TMP/pragma"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    pragma_hits "$f" "$PRAGMA_ERE" >> "$TMP/pragma"
done < "$TMP/sources"

# --- leg 2: the headers, their guards, and the spelling the path dictates -----
git ls-files -- '*.h' '*.hh' '*.hpp' '*.inc' '*.h.in' > "$TMP/headers" \
    || fail "git ls-files failed"
require_nonempty "$TMP/headers" "git ls-files matched no header; every check below would pass vacuously"
HEADERS="$(wc -l < "$TMP/headers" | tr -d ' ')"

: > "$TMP/findings"
: > "$TMP/refused"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    V="$(header_verdict "$f" "$f" "$ADJACENT" "$FIRST_DIRECTIVE" "$ENDIF_ERE")"
    case "$V" in
        ok) ;;
        refused) printf '%s\n' "$f" >> "$TMP/refused" ;;
        *)
            _pair="${V#guard }"
            printf '%s: guard is %s, the path dictates %s\n' \
                "$f" "${_pair%% *}" "${_pair#* }" >> "$TMP/findings" ;;
    esac
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
