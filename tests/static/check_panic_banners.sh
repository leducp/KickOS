#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every fault banner a reporter can put on the wire is matched by tests/lib/panic.ere.
# DERIVED from the emit sites, never from a second list: the ERE is what makes
# assert_no_panic and the sim FAIL_REGULAR_EXPRESSION see a fault at all, so a banner it
# misses turns a board that died into a board that passed.
#
# Run from the repo root, no arguments: tests/static/check_panic_banners.sh
#
# THE MATCH: a single-line string literal that BOTH opens with `\n=== ` and carries a
# closing ` ===` after that. Prose in this tree names a banner without either half, so the
# shape separates an emit site from a comment ABOUT one with no by-name exemption list to
# maintain. A comment that spells a WHOLE banner including its `\n` escape does report, and
# the fix is to stop spelling an escape sequence in prose.
#
# THE NAME: when the banner name is itself the conversion, `\n=== %s ===`, the reporter
# picks it at runtime and the wire text is one line per label. The labels come from the
# argument NAME on the emit line and from every `<name> = "..."` assignment in the same
# file, so all three armv7m labels are checked. Every other conversion is substituted with
# a placeholder, since the ERE keys on the fixed prefix.
#
# SCOPE. Read are the banner literals as bytes, one source line at a time, over tracked
# *.c, *.cc, *.h and *.S under arch/, kernel/, include/, lib/ and system/, any of which
# could grow a reporter. Outside it: a banner assembled at runtime from pieces or spelled
# across two source lines, since nothing here parses C; a label held anywhere but a
# `<name> = "..."` in the reporter's own file, such as a table, a function return or
# another TU; and the reverse direction, an alternative in panic.ere matching no banner at
# all. The three non-banner alternatives ("KERNEL PANIC:", "MPU FAULT: thread",
# "ISOLATION FAULT:") have no `=== ` shape and are outside this gate entirely. Whether a
# banner SHOULD be a panic is the exclusion below, and that is a ruling.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

# THE ONE EXCLUSION, and it is a ruling: the thread-fault report is what fault ISOLATION
# prints when a thread died and the system did not. Four gates (check_rootfault.sh,
# check_mpu_fault.sh, check_faultsurvive.sh, check_qemu_ringppb.sh) assert that line is
# PRESENT while asserting no panic occurred, so putting it in the ERE would make every one
# of them contradict itself. On a board with no privilege ring the same violation panics
# instead, and the banner it prints there is one of the reporter banners below.
EXCLUDED='=== THREAD FAULT ==='

# Files that MUST each yield a banner, one per fault reporter in the tree, so a shape that
# stops matching cannot read as "no reporter emits a banner any more".
REPORTERS='arch/arm/armv6m/arch_armv6m.cc
arch/arm/armv7m/arch_armv7m.cc
arch/riscv/rv32imac/arch_rv32imac.cc
arch/rx/rxv3/arch_rxv3.cc
arch/xtensa/lx6/arch_xtensa.cc
arch/sim/sim.cc'

# The armv7m reporter picks between HARD, MPU and BUS. Fewer resolved labels means the
# label scan went vacuous, which would otherwise read as a clean tree.
MIN_LABELS=3

scratch_dir

git ls-files -- 'arch/*' 'kernel/*' 'include/*' 'lib/*' 'system/*' \
    | grep -E '\.(c|cc|h|S)$' > "$TMP/corpus" 2>/dev/null
require_nonempty "$TMP/corpus" "the corpus is empty: git ls-files matched no source file"

# Emits one tab-separated record per resolved banner:
#   <file> <line> <slot> <banner text>
# slot is `fixed` for a literal name and `label` for one substituted into a `%s` name slot.
: > "$TMP/banners"
while IFS= read -r f; do
    [ -f "$f" ] || continue
    awk -v FNAME="$f" '
        # Placeholder for every conversion that is not the name slot. The ERE keys on the
        # fixed prefix, so the value only has to be free of regex-significant bytes.
        function subst_convs(s,   out, i, c) {
            out = ""
            i = 1
            while (i <= length(s)) {
                c = substr(s, i, 1)
                if (c == "%" && i < length(s)) {
                    # %% is a literal percent; anything else is one conversion character
                    # after optional flag/width bytes, and this tree uses none.
                    if (substr(s, i + 1, 1) == "%") {
                        out = out "%"
                    } else {
                        out = out "X"
                    }
                    i = i + 2
                    continue
                }
                out = out c
                i = i + 1
            }
            return out
        }
        # The wire text of a banner literal: from the `===` up to the first `\n` escape
        # after it, so a multi-line format contributes only its banner line.
        function banner_line(body,   rest, p) {
            rest = substr(body, 3)          # drop the leading \n escape
            p = index(rest, "\\n")
            if (p > 0) {
                rest = substr(rest, 1, p - 1)
            }
            return rest
        }
        { line[NR] = $0 }
        END {
            for (n = 1; n <= NR; n++) {
                s = line[n]
                pos = 1
                while (1) {
                    # index() over the literal 7 bytes, so no regex escaping is in play.
                    p = index(substr(s, pos), "\"\\n=== ")
                    if (p == 0) { break }
                    start = pos + p - 1                  # the opening quote
                    rest = substr(s, start + 1)
                    q = index(rest, "\"")
                    if (q == 0) { pos = start + 1; continue }
                    body = substr(rest, 1, q - 1)
                    pos = start + q + 1
                    if (index(substr(body, 6), " ===") == 0) { continue }
                    text = banner_line(body)
                    if (substr(text, 1, 7) == "=== %s ") {
                        # The name IS the conversion, so it comes from the argument name
                        # on this line and from every assignment to that name in this file.
                        tail = substr(s, start + q + 1)
                        sub(/^[ \t]*,[ \t]*/, "", tail)
                        if (match(tail, /^[A-Za-z_][A-Za-z_0-9]*/) == 0) { continue }
                        nm = substr(tail, 1, RLENGTH)
                        for (m = 1; m <= NR; m++) {
                            t = line[m]
                            while (match(t, nm "[ \t]*=[ \t]*\"[^\"]*\"")) {
                                a = substr(t, RSTART, RLENGTH)
                                t = substr(t, RSTART + RLENGTH)
                                if (match(a, /"[^"]*"/) == 0) { continue }
                                v = substr(a, RSTART + 1, RLENGTH - 2)
                                if (v == "") { continue }
                                out = text
                                sub(/%s/, v, out)
                                printf "%s\t%d\tlabel\t%s\n", FNAME, n, subst_convs(out)
                            }
                        }
                        continue
                    }
                    printf "%s\t%d\tfixed\t%s\n", FNAME, n, subst_convs(text)
                }
            }
        }
    ' "$f" >> "$TMP/banners"
done < "$TMP/corpus"

require_nonempty "$TMP/banners" \
    "no banner was extracted from any file: the literal shape no longer matches an emit site"

# --- positive controls -------------------------------------------------------
rc=0
printf '%s\n' "$REPORTERS" | while IFS= read -r r; do
    [ -n "$r" ] || continue
    if ! cut -f1 "$TMP/banners" | grep -qxF "$r"; then
        echo "FAIL: $r yielded no banner: it is a fault reporter, so either it stopped" >&2
        echo "      emitting one or this gate stopped seeing it" >&2
        echo x >> "$TMP/rc"
    fi
done
labels=$(awk -F"$TAB" '$3 == "label"' "$TMP/banners" | wc -l)
if [ "$labels" -lt "$MIN_LABELS" ]; then
    echo "FAIL: the name-slot leg resolved $labels label(s), expected at least $MIN_LABELS." >&2
    echo "      A '=== %s ===' banner whose labels are not found is checked against nothing," >&2
    echo "      which is exactly how '=== BUS FAULT' went unmatched." >&2
    echo x >> "$TMP/rc"
fi

# --- every banner must be matched -------------------------------------------
checked=0
while IFS="$TAB" read -r f n slot text; do
    checked=$((checked + 1))
    case "$text" in
        *"$EXCLUDED"*)
            # Named above, with the ruling. Asserted NOT matched: in the ERE it would break
            # the four gates that read it as a survivable outcome.
            if printf '%s\n' "$text" | grep -qE "$KOS_PANIC_RE"; then
                echo "FAIL: $f:$n banner '$text' IS matched by tests/lib/panic.ere, but it is" >&2
                echo "      the fault-isolation report: the thread died and the system did" >&2
                echo "      not. Every assert_no_panic on a thread-kill board would now fail." >&2
                echo x >> "$TMP/rc"
            fi
            continue
            ;;
        *) ;;
    esac
    if ! printf '%s\n' "$text" | grep -qE "$KOS_PANIC_RE"; then
        echo "FAIL: $f:$n banner '$text' ($slot) is NOT matched by tests/lib/panic.ere." >&2
        echo "      A gate asserting no panic passes on a board that printed it, and a gate" >&2
        echo "      asserting a panic cannot key on it. Add it to the ERE." >&2
        echo x >> "$TMP/rc"
    fi
done < "$TMP/banners"

if [ -s "$TMP/rc" ]; then
    rc=1
fi
if [ "$rc" -ne 0 ]; then
    fail "$(wc -l < "$TMP/rc") banner finding(s) above"
fi

echo "PASS: $checked banner(s) from $(cut -f1 "$TMP/banners" | sort -u | wc -l) file(s)," \
     "$labels resolved from a name slot, all accounted for by tests/lib/panic.ere"
exit 0
