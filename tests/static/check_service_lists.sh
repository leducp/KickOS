#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the KICKOS_SERVICE_LIST axis: every kickos_services_* provider in the tree
# is named, in tests/static/service_lists.txt, against a configure preset that compiles it.
#
# The axis is invisible without this. A provider is created by one
# kickos_add_board_provider() call inside its chip's branch of system/CMakeLists.txt, so
# a board whose chip is never configured compiles none of them and nothing reports a skip:
# rxsci.cc reached a fleet build with no object at all, and every K-seam gate failed to
# LINK under `sim-telem` for two milestones, both because "not covered" and "covered and
# passing" look identical from a green run.
#
# Usage:
#   check_service_lists.sh <cmake> <src-dir>
#
# Source-tree gate: reads the tree through `git ls-files` plus the preset files, builds
# nothing and configures nothing, so it registers on every board like doc_names.
#
# WHAT IT PINS, in both directions:
#   - every provider the tree creates is declared, and an undeclared one is REFUSED.
#   - every declared provider still exists; a line naming one that does not is dead.
#   - every declared preset is a VISIBLE configure preset.
#   - the declared preset's board is the provider's OWN board, taken from the directory
#     its SOURCE lives in (system/init/<board>/). Without this leg the file is
#     unfalsifiable, since every provider could name `sim` and the gate would stay green.
#   - `default` requires the board's Kconfig (or the root Kconfig, for the universal one)
#     to spell `default "<provider>"`; `select` requires that no Kconfig does.
#
# THEREFORE NOT CAUGHT. Know these before trusting a green run:
#   - COMPILED IS NOT LINKED, AND LINKED IS NOT RUN. Every provider under a chip's branch
#     is an ordinary target in `all`, so building that board compiles it whichever list
#     the board defaults to. A `select` provider whose services have never been brought
#     up on any image is green here and always will be. Nothing in this file is evidence
#     that a provider was executed; that is the bench's half.
#   - It builds nothing, so a provider that no longer COMPILES, and a preset that no
#     longer CONFIGURES, are both invisible here. tools/sweep_host_gates.sh is the half
#     that configures, and the fleet build is the half that compiles.
#   - The `default` leg pins that a board's Kconfig NAMES the provider, not that a given
#     preset's POSTURE selects it. frdmk64f's and xmc4800-relax's defaults are conditional
#     on MEMORY_MODEL_MPU, so their `-flat` variants fall back to kickos_services_none and
#     this gate cannot see the difference.
#   - Board equality comes from the provider's own SOURCE directory. Two boards sharing a
#     chip would both compile a provider written under one of their directories, and only
#     the declared one is ever checked.
#   - ONE preset per provider. A provider compiled by five presets is asserted about one,
#     so this file is a floor on coverage and never a map of it.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

if [ "$#" -ne 2 ]; then
    fail "usage: check_service_lists.sh <cmake> <src-dir>"
fi
CMAKE="$1"
SRC="$2"

[ -x "$CMAKE" ] || fail "no cmake at $CMAKE"
[ -d "$SRC" ] || fail "no source directory at $SRC"
cd "$SRC" || fail "cannot enter $SRC"
[ -f CMakeLists.txt ] || fail "$SRC is not the repo root"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "$SRC is not the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

DECL="tests/static/service_lists.txt"
FLATTEN="tests/static/preset_boards.cmake"
[ -f "$DECL" ] || fail "no declaration file at $SRC/$DECL"
[ -f "$FLATTEN" ] || fail "no preset flattener at $SRC/$FLATTEN"

CALL=kickos_add_board_provider

scratch_dir

# --- the providers, from the calls that create them ---------------------------
git ls-files -- CMakeLists.txt '*/CMakeLists.txt' > "$TMP/lists" || fail "git ls-files failed"
require_nonempty "$TMP/lists" "git ls-files matched no CMakeLists.txt; every check below would pass vacuously"

: > "$TMP/cand"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    if grep -q "$CALL(services_" "$f"; then
        printf '%s\n' "$f" >> "$TMP/cand"
    fi
done < "$TMP/lists"
require_nonempty "$TMP/cand" "no tracked CMakeLists.txt calls $CALL(services_...); the scan is broken"

# A call spans several lines, so the comment-stripped file is joined into one string and
# the calls are cut out of it in order. A line-shaped state machine gets two single-line
# calls in a row wrong, closing the first on the second's line and eating the second
# whole. The count check below caught that. Refuses (exit 2) rather than
# skipping a block it could not read: a dropped block is a provider that silently needs
# no declaration.
: > "$TMP/providers"
: > "$TMP/awkerr"
while IFS= read -r f; do
    d="$(dirname "$f")"
    if awk -v CALL="$CALL(" -v DIR="$d" '
        { line = $0; sub(/#.*/, "", line); all = all " " line }
        END {
            while ((i = index(all, CALL)) > 0) {
                all = substr(all, i + length(CALL))
                j = index(all, ")")
                if (j == 0) {
                    printf("%s: an unterminated %s...\n", FILENAME, CALL) > "/dev/stderr"
                    bad = 1
                    break
                }
                body = substr(all, 1, j - 1)
                all = substr(all, j + 1)
                n = split(body, t, /[ \t]+/)
                name = ""; src = ""
                for (k = 1; k <= n; k++) {
                    if (t[k] == "") { continue }
                    if (name == "") { name = t[k]; continue }
                    if (t[k] == "SOURCE" && k < n) { src = t[k + 1] }
                }
                if (name !~ /^services_/) { continue }
                if (src == "") {
                    printf("%s: %s%s ...) carries no SOURCE this parse can read\n", FILENAME, CALL, name) > "/dev/stderr"
                    bad = 1
                    continue
                }
                printf("kickos_%s\t%s/%s\n", name, DIR, src)
            }
            if (bad) { exit 2 }
        }
    ' "$f" >> "$TMP/providers" 2>> "$TMP/awkerr"; then
        :
    else
        rc=$?
        [ "$rc" -eq 2 ] || fail "awk exited $rc scanning $f"
    fi
done < "$TMP/cand"

if [ -s "$TMP/awkerr" ]; then
    cat "$TMP/awkerr" >&2
    fail "a $CALL(services_...) call could not be read; a provider it creates would need no declaration"
fi
require_nonempty "$TMP/providers" "not one provider came out of the scan"

# The second, independent fact: a plain per-line count of the OPENING calls. The parser
# above has to close a paren and find a SOURCE, so a block it silently dropped shows up
# here as a disagreement and nowhere else.
: > "$TMP/opened"
while IFS= read -r f; do
    grep -ho "$CALL(services_[A-Za-z0-9_]*" "$f" >> "$TMP/opened"
done < "$TMP/cand"
OPENED="$(wc -l < "$TMP/opened" | tr -d ' ')"
PARSED="$(wc -l < "$TMP/providers" | tr -d ' ')"
[ "$OPENED" = "$PARSED" ] || fail "$OPENED $CALL(services_...) call(s) opened but $PARSED parsed"

# --- the presets, as CMake itself reads them ----------------------------------
tool_out "$TMP/flatten.log" '' \
    "$CMAKE" "-DSRC=$SRC" "-DOUT=$TMP/presets" -P "$FLATTEN"
require_nonempty "$TMP/presets" "the preset flattener produced no table"

# --- every provider a Kconfig names as a board default ------------------------
git ls-files -- Kconfig '*/Kconfig' > "$TMP/kconfigs" || fail "git ls-files failed"
require_nonempty "$TMP/kconfigs" "git ls-files matched no Kconfig"
: > "$TMP/kdefaults"
while IFS= read -r kf; do
    sed -n 's/.*default[[:space:]]*"\(kickos_services_[A-Za-z0-9_]*\)".*/\1/p' "$kf" \
        | while IFS= read -r p; do printf '%s\t%s\n' "$p" "$kf"; done >> "$TMP/kdefaults"
done < "$TMP/kconfigs"
require_nonempty "$TMP/kdefaults" "no Kconfig names a kickos_services_* default; the default leg would be vacuous"

# --- the declarations ----------------------------------------------------------
sed -e 's/#.*//' "$DECL" | grep -v '^[[:space:]]*$' > "$TMP/decl.txt"
require_nonempty "$TMP/decl.txt" "$DECL declares nothing"

: > "$TMP/findings.txt"
: > "$TMP/declared.txt"
report() { echo "$1" >> "$TMP/findings.txt"; }

N_DEFAULT=0
N_SELECT=0
while read -r prov preset how extra; do
    [ -z "$extra" ] || fail "$DECL: trailing junk after '$prov $preset $how': $extra"
    case "$prov" in
        kickos_services_*) ;;
        *) fail "$DECL: '$prov' is not a service-list provider name" ;;
    esac
    case "$how" in
        default) N_DEFAULT=$((N_DEFAULT + 1)) ;;
        select) N_SELECT=$((N_SELECT + 1)) ;;
        *) fail "$DECL: '$how' is not a selection; use default or select" ;;
    esac
    echo "$prov" >> "$TMP/declared.txt"

    SOURCE="$(grep -m1 "^$prov$TAB" "$TMP/providers" | cut -f2)"
    if [ -z "$SOURCE" ]; then
        report "$DECL declares $prov, which no $CALL() in this tree creates"
        continue
    fi
    [ -f "$SOURCE" ] || fail "$prov names $SOURCE, which is not a file; the scan is broken"

    # system/init/<board>/..., the provider's own board. system/init/common/ is the
    # universal one and constrains no preset.
    case "$SOURCE" in
        */init/*) rest="${SOURCE#*/init/}" ;;
        init/*) rest="${SOURCE#init/}" ;;
        *) report "$prov: its SOURCE $SOURCE is not under an init/<board>/ directory, so no board can be read off it"
           continue ;;
    esac
    home="${rest%%/*}"
    if [ "$home" != common ] && [ ! -f "boards/$home/board.cmake" ]; then
        report "$prov: its SOURCE $SOURCE sits under init/$home/, which is not a board (no boards/$home/board.cmake)"
        continue
    fi

    BOARD="$(grep -m1 "^$preset$TAB" "$TMP/presets" | cut -f2)"
    if [ -z "$BOARD" ]; then
        report "$prov names preset '$preset', which is not a visible configure preset"
        continue
    fi
    if [ "$BOARD" = "@none" ]; then
        report "$prov names preset '$preset', which resolves no KICKOS_BOARD"
        continue
    fi
    if [ "$home" != common ] && [ "$BOARD" != "$home" ]; then
        report "$prov lives under init/$home/ but names preset '$preset', whose board is $BOARD; that configuration never reaches the provider"
    fi

    if [ "$how" = default ]; then
        if ! grep -qxF "$prov${TAB}Kconfig" "$TMP/kdefaults" \
           && ! grep -qxF "$prov${TAB}boards/$BOARD/Kconfig" "$TMP/kdefaults"; then
            report "$prov is declared default, but neither Kconfig nor boards/$BOARD/Kconfig spells default \"$prov\""
        fi
    else
        WHERE="$(grep "^$prov$TAB" "$TMP/kdefaults" | cut -f2 | tr '\n' ' ')"
        if [ -n "$WHERE" ]; then
            report "$prov is declared select, but it is a Kconfig default in: $WHERE"
        fi
    fi
done < "$TMP/decl.txt"

DUP="$(sort "$TMP/declared.txt" | uniq -d)"
[ -z "$DUP" ] || fail "$DECL declares a provider twice: $DUP"

# The other direction: a provider the tree creates and this file does not name.
while IFS= read -r line; do
    prov="${line%%$TAB*}"
    if ! grep -qxF "$prov" "$TMP/declared.txt"; then
        report "$prov exists in this tree and $DECL does not declare it; name a preset that compiles it, and say default or select"
    fi
done < "$TMP/providers"

N_PRESETS="$(wc -l < "$TMP/presets" | tr -d ' ')"
echo "== $PARSED provider(s) in the tree, $((N_DEFAULT + N_SELECT)) declared ($N_DEFAULT default, $N_SELECT select) across $N_PRESETS visible preset(s) =="

if [ -s "$TMP/findings.txt" ]; then
    cat "$TMP/findings.txt" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings.txt" | tr -d ' ') finding(s) against $DECL." >&2
    echo "      Declare the provider, fix the preset it names, or drop a line for a provider that is gone." >&2
    echo "      A provider nothing names is a provider nothing compiles." >&2
    exit 1
fi

echo "PASS: every service-list provider is declared against a preset of its own board"
