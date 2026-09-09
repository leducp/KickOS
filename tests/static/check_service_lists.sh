#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the KICKOS_SERVICE_LIST axis: every kickos_services_* provider in the tree
# is named, in tests/static/service_lists.txt, against a configure preset that compiles it.
#
# The axis is invisible without this. A provider is created by one
# kickos_add_board_provider() call inside its chip's branch of system/CMakeLists.txt, so a
# board whose chip is never configured compiles none of them and nothing reports a skip,
# leaving "not covered" and "covered and passing" identical from a green run.
#
# Source-tree gate: reads the tree through `git ls-files` plus the preset files, builds
# nothing and configures nothing, so it registers on every board.
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
# SCOPE, and it is a floor on coverage rather than a map of it:
#   - COMPILED IS NOT LINKED, AND LINKED IS NOT RUN. Every provider under a chip's branch
#     is an ordinary target in `all`, so building that board compiles it whichever list
#     the board defaults to. A `select` provider whose services have never been brought
#     up on any image is green here and always will be. Evidence that a provider was
#     executed is the bench's half: tools/sweep_service_lists.sh forces each declared row
#     through its own -DKICKOS_SERVICE_LIST and confirms it links.
#   - It builds nothing: tools/sweep_host_gates.sh is the half that configures a preset,
#     and the fleet build is the half that compiles a provider.
#   - The `default` leg pins that a board's Kconfig NAMES the provider, not that a given
#     preset's POSTURE selects it. frdmk64f's and xmc4800-relax's defaults are conditional
#     on MEMORY_MODEL_MPU, so their `-flat` variants fall back to kickos_services_none and
#     read the same here.
#   - Board equality comes from the provider's own SOURCE directory. Two boards sharing a
#     chip both compile a provider written under one of their directories, and the declared
#     one is what gets checked.
#   - ONE preset per provider. A provider compiled by five presets is asserted about one.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

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
# The one init directory that is not a board, and the Kconfig the universal provider's
# default may sit in beside the board's own.
COMMON_DIR=common
ROOT_KCONFIG=Kconfig

scratch_dir

# --- the scanners, each driven by both the self-test and the corpus -----------
# A call spans several lines, so the comment-stripped file is joined into one string and
# the calls are cut out of it in order. A line-shaped state machine gets two single-line
# calls in a row wrong, closing the first on the second's line and eating the second whole.
# Refuses (exit 2) rather than skipping a block it could not read: a dropped block is a
# provider that silently needs no declaration.
parse_providers() { # <file> <dir>; records on stdout, refusals on stderr, exit 2 on one
    awk -v CALL="$CALL(" -v DIR="$2" '
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
    ' "$1"
}

# The second, independent fact: a plain per-line count of the OPENING calls. The parser
# has to close a paren and find a SOURCE, so a block it silently dropped shows up here as
# a disagreement and nowhere else.
#
# BOTH SIDES STRIP COMMENTS, and with the same expression. What the cross-check asserts is
# that the parser dropped no block, and two sides reading different corpora cannot assert
# that: a call written inside a comment creates no provider, so counting it as opened puts
# the disagreement on prose instead.
opened_calls() { # <file>; one line per opening call
    if sed -e 's/#.*//' "$1" > "$TMP/oc_nc"; then
        _oc_rc=0
    else
        _oc_rc=$?
    fi
    [ "$_oc_rc" -eq 0 ] || fail "sed exited $_oc_rc stripping the comments out of $1"
    # rc 1 is a file that opens no call; past 1 is grep dying, and a short count here reads
    # downstream as a parser that dropped a block.
    grep -ho "$CALL(services_[A-Za-z0-9_]*" "$TMP/oc_nc"
    _oc_rc=$?
    [ "$_oc_rc" -le 1 ] || fail "grep exited $_oc_rc counting the $CALL( calls opened in $1"
}

# Every provider a Kconfig names as a default, with the file that names it. The trailing
# `.*` is what keeps a conditional `default "x" if SYM` in, and the leading one is greedy,
# so of two defaults written on ONE line only the last is read.
#
# Comments go first, with the same expression the provider parse uses: a `default` written
# inside a comment is not a default, and reading one as real lets a provider whose Kconfig
# never names it pass the default leg.
kconfig_defaults() { # <file>; <provider> TAB <file> per default
    if sed -e 's/#.*//' "$1" > "$TMP/kd_nc"; then
        _kd_rc=0
    else
        _kd_rc=$?
    fi
    [ "$_kd_rc" -eq 0 ] || fail "sed exited $_kd_rc stripping the comments out of $1"
    if sed -n 's/.*default[[:space:]]*"\(kickos_services_[A-Za-z0-9_]*\)".*/\1/p' \
        "$TMP/kd_nc" > "$TMP/kd_hit"; then
        _kd_rc=0
    else
        _kd_rc=$?
    fi
    [ "$_kd_rc" -eq 0 ] || fail "sed exited $_kd_rc reading the service-list defaults out of $1"
    while IFS= read -r p; do
        printf '%s\t%s\n' "$p" "$1"
    done < "$TMP/kd_hit"
}

decl_rows() { # <file>; the declaration lines, comments and blank lines dropped
    sed -e 's/#.*//' "$1" | grep -v '^[[:space:]]*$'
}

report() { printf '%s\n' "$1" >> "$_VF"; }

# The declaration legs. Every table it reads is an argument, so the self-test can put a
# planted tree in front of it instead of the one this checkout happens to have.
verdict() { # <rows> <providers> <presets> <kdefaults> <findings> <declared>; prints "<ndefault> <nselect>"
    _rows="$1"; _prov="$2"; _pres="$3"; _kdef="$4"; _VF="$5"; _VD="$6"
    : > "$_VF"
    : > "$_VD"
    _nd=0
    _ns=0
    while read -r prov preset how extra; do
        [ -z "$extra" ] || fail "$DECL: trailing junk after '$prov $preset $how': $extra"
        case "$prov" in
            kickos_services_*) ;;
            *) fail "$DECL: '$prov' is not a service-list provider name" ;;
        esac
        case "$how" in
            default) _nd=$((_nd + 1)) ;;
            select) _ns=$((_ns + 1)) ;;
            *) fail "$DECL: '$how' is not a selection; use default or select" ;;
        esac
        echo "$prov" >> "$_VD"

        SOURCE="$(grep -m1 "^$prov$TAB" "$_prov" | cut -f2)"
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
        if [ "$home" != "$COMMON_DIR" ] && [ ! -f "boards/$home/board.cmake" ]; then
            report "$prov: its SOURCE $SOURCE sits under init/$home/, which is not a board (no boards/$home/board.cmake)"
            continue
        fi

        BOARD="$(grep -m1 "^$preset$TAB" "$_pres" | cut -f2)"
        if [ -z "$BOARD" ]; then
            report "$prov names preset '$preset', which is not a visible configure preset"
            continue
        fi
        if [ "$BOARD" = "@none" ]; then
            report "$prov names preset '$preset', which resolves no KICKOS_BOARD"
            continue
        fi
        if [ "$home" != "$COMMON_DIR" ] && [ "$BOARD" != "$home" ]; then
            report "$prov lives under init/$home/ but names preset '$preset', whose board is $BOARD; that configuration never reaches the provider"
        fi

        if [ "$how" = default ]; then
            if ! grep -qxF "$prov$TAB$ROOT_KCONFIG" "$_kdef" \
               && ! grep -qxF "$prov${TAB}boards/$BOARD/Kconfig" "$_kdef"; then
                report "$prov is declared default, but neither $ROOT_KCONFIG nor boards/$BOARD/Kconfig spells default \"$prov\""
            fi
        else
            WHERE="$(grep "^$prov$TAB" "$_kdef" | cut -f2 | tr '\n' ' ')"
            if [ -n "$WHERE" ]; then
                report "$prov is declared select, but it is a Kconfig default in: $WHERE"
            fi
        fi
    done < "$_rows"

    # A dead sort or uniq leaves DUP empty, which is the SAME answer as "no duplicate", so
    # each is read on its own and neither goes through a pipe.
    if sort "$_VD" > "$_VD.sorted"; then
        _v_rc=0
    else
        _v_rc=$?
    fi
    [ "$_v_rc" -eq 0 ] || fail "sort exited $_v_rc over the declared providers of $DECL; a
      provider declared twice would read as none"
    if uniq -d "$_VD.sorted" > "$_VD.dup"; then
        _v_rc=0
    else
        _v_rc=$?
    fi
    [ "$_v_rc" -eq 0 ] || fail "uniq exited $_v_rc over the declared providers of $DECL; a
      provider declared twice would read as none"
    if [ -s "$_VD.dup" ]; then
        fail "$DECL declares a provider twice: $(tr '\n' ' ' < "$_VD.dup")"
    fi

    # The other direction: a provider the tree creates and this file does not name.
    while IFS= read -r line; do
        prov="${line%%$TAB*}"
        if ! grep -qxF "$prov" "$_VD"; then
            report "$prov exists in this tree and $DECL does not declare it; name a preset that compiles it, and say default or select"
        fi
    done < "$_prov"

    printf '%s %s\n' "$_nd" "$_ns"
}

# --- self-test: prove every clause of the rule, one control per clause ---------
# A control that some unrelated clause would also catch proves nothing about the clause it
# claims to test, so each below is a MINIMAL PAIR and every expected count is exact.
cat > "$TMP/t_pos.cmake" <<'EOF'
kickos_add_board_provider(services_alpha SOURCE init/alpha/a.cc)
kickos_add_board_provider(services_bravo SOURCE init/bravo/b.cc)
kickos_add_board_provider(services_charlie
    SOURCE init/charlie/c.cc
    DEPENDS kickos_chip_x)
EOF
POS="$(parse_providers "$TMP/t_pos.cmake" system 2> "$TMP/t_err" | wc -l | tr -d ' ')"
[ "$POS" -eq 3 ] || fail "the parser read $POS of 3 planted calls; it would miss a real provider"
[ ! -s "$TMP/t_err" ] || fail "the parser refused a file of legal calls; every such file would read UNKNOWN"
# The RECORD and not just the count: a parse that loses the SOURCE reports every provider
# as one no call creates.
parse_providers "$TMP/t_pos.cmake" system \
    | grep -qxF "kickos_services_charlie${TAB}system/init/charlie/c.cc" \
    || fail "the parser did not join a call spanning three lines onto its own SOURCE"
# Two single-line calls in a row, which is the shape a line-shaped parse reads as one.
head -n 2 "$TMP/t_pos.cmake" > "$TMP/t_pair.cmake"
POS="$(parse_providers "$TMP/t_pair.cmake" system | wc -l | tr -d ' ')"
[ "$POS" -eq 2 ] || fail "the parser read $POS of 2 adjacent single-line calls, so it closes the first on the second's line"

# EACH negative on its own, so a control silent for the WRONG reason is visible. A
# whole-file zero cannot tell "four clauses work" from "one clause swallowed the file".
cat > "$TMP/t_neg" <<'EOF'
# kickos_add_board_provider(services_zulu SOURCE init/zulu/z.cc)
kickos_add_board_provider(other_thing SOURCE init/x/x.cc)
kickos_add_board_provider_helper(services_alpha SOURCE init/a/a.cc)
set(services_alpha 1)
EOF
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$TMP/t_one.cmake"
    n="$(parse_providers "$TMP/t_one.cmake" system 2> "$TMP/t_err" | wc -l | tr -d ' ')"
    [ "$n" -eq 0 ] || fail "negative control $i parses as a provider: $line"
    [ ! -s "$TMP/t_err" ] || fail "negative control $i is refused rather than ignored: $line"
done < "$TMP/t_neg"
[ "$i" -eq 4 ] || fail "$i negative control(s) ran, expected 4"

# A call the parse cannot read must REFUSE with 2, which is the only code the corpus loop
# tolerates, and must emit no record beside the refusal.
cat > "$TMP/t_refuse" <<'EOF'
kickos_add_board_provider(services_nosrc)|carries no SOURCE
kickos_add_board_provider(services_keyonly SOURCE)|carries no SOURCE
kickos_add_board_provider(services_open SOURCE init/o/o.cc|an unterminated
EOF
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "${line%%|*}" > "$TMP/t_one.cmake"
    parse_providers "$TMP/t_one.cmake" system > "$TMP/t_out" 2> "$TMP/t_err"
    rc=$?
    [ "$rc" -eq 2 ] || fail "refusal control $i exited $rc, not 2; the corpus loop takes a hard fail on any other code"
    [ ! -s "$TMP/t_out" ] || fail "refusal control $i emitted a record beside its refusal"
    grep -qF "${line#*|}" "$TMP/t_err" \
        || fail "refusal control $i refused for the wrong reason: $(cat "$TMP/t_err")"
done < "$TMP/t_refuse"
[ "$i" -eq 3 ] || fail "$i refusal control(s) ran, expected 3"

# The opening count, against the same corpus and against the one line the two disagree on.
POS="$(opened_calls "$TMP/t_pos.cmake" | wc -l | tr -d ' ')"
[ "$POS" -eq 3 ] || fail "the opening count saw $POS of 3 planted calls; a parser that drops a block would agree with it"
# The cross-check's two sides, on a call written inside a comment: BOTH must read zero, or
# the disagreement they report is about which corpus each read and not about a dropped block.
printf '# %s\n' "$CALL(services_zulu SOURCE init/zulu/z.cc)" > "$TMP/t_one.cmake"
POS="$(opened_calls "$TMP/t_one.cmake" | wc -l | tr -d ' ')"
[ "$POS" -eq 0 ] || fail "the opening count reads a call written inside a comment, so it and
      the parser read two different corpora and their disagreement means nothing"
POS="$(parse_providers "$TMP/t_one.cmake" system | wc -l | tr -d ' ')"
[ "$POS" -eq 0 ] || fail "the parser read a call written inside a comment"
# Its minimal pair: a real call with a comment after it must still be counted by both, or
# stripping comments has cost the cross-check the calls it exists to compare.
printf '%s # the alpha board\n' "$CALL(services_alpha SOURCE init/alpha/a.cc)" > "$TMP/t_one.cmake"
POS="$(opened_calls "$TMP/t_one.cmake" | wc -l | tr -d ' ')"
[ "$POS" -eq 1 ] || fail "the opening count lost a real call to a trailing comment"
POS="$(parse_providers "$TMP/t_one.cmake" system | wc -l | tr -d ' ')"
[ "$POS" -eq 1 ] || fail "the parser lost a real call to a trailing comment"

# The Kconfig default scan.
cat > "$TMP/t_kc" <<'EOF'
config KICKOS_SERVICE_LIST
    string
    default "kickos_services_alpha" if MEMORY_MODEL_MPU
    default "kickos_services_bravo"
EOF
POS="$(kconfig_defaults "$TMP/t_kc" | wc -l | tr -d ' ')"
[ "$POS" -eq 2 ] || fail "the Kconfig scan read $POS of 2 planted defaults; the default leg would go vacuous"
kconfig_defaults "$TMP/t_kc" | grep -qxF "kickos_services_alpha${TAB}$TMP/t_kc" \
    || fail "the Kconfig scan dropped a conditional default, or the file it came from"
cat > "$TMP/t_kcneg" <<'EOF'
    default KICKOS_OTHER
    default "kickos_other_thing"
    select KICKOS_SERVICES_ALPHA
    prompt "the default kickos_services_alpha"
EOF
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$TMP/t_one"
    n="$(kconfig_defaults "$TMP/t_one" | wc -l | tr -d ' ')"
    [ "$n" -eq 0 ] || fail "Kconfig negative control $i reads as a default: $line"
done < "$TMP/t_kcneg"
[ "$i" -eq 4 ] || fail "$i Kconfig negative control(s) ran, expected 4"
printf '%s\n' '# default "kickos_services_ghost"' > "$TMP/t_one"
POS="$(kconfig_defaults "$TMP/t_one" | wc -l | tr -d ' ')"
[ "$POS" -eq 0 ] || fail "the Kconfig scan reads a default written inside a comment, so a
      provider whose Kconfig never names it passes the default leg"
printf '%s\n' '    default "kickos_services_one" # the flat fallback' > "$TMP/t_one"
POS="$(kconfig_defaults "$TMP/t_one" | wc -l | tr -d ' ')"
[ "$POS" -eq 1 ] || fail "the Kconfig scan lost a real default to a trailing comment"
printf '%s\n' '    default "kickos_services_one" default "kickos_services_two"' > "$TMP/t_one"
POS="$(kconfig_defaults "$TMP/t_one" | wc -l | tr -d ' ')"
[ "$POS" -eq 1 ] || fail "the Kconfig scan read $POS of two defaults on one line, and the leading .* is greedy"

# The declaration reader.
cat > "$TMP/t_rows" <<'EOF'
# a declaration spelled in a comment
kickos_services_alpha alphapreset default

EOF
POS="$(decl_rows "$TMP/t_rows" | wc -l | tr -d ' ')"
[ "$POS" -eq 1 ] || fail "the declaration reader counted $POS of 1 real row; a comment or a blank line reads as a declaration"

# --- self-test: the declaration legs, against a planted tree ------------------
# A planted root, so every assertion below is about the RULE and not about the providers
# this tree happens to carry today.
mkdir -p "$TMP/root/system/init/alpha" "$TMP/root/system/init/common" \
    "$TMP/root/system/init/ghost" "$TMP/root/system/plain" "$TMP/root/boards/alpha"
for f in system/init/alpha/a.cc system/init/alpha/e.cc system/init/common/u.cc \
    system/init/ghost/g.cc system/plain/p.cc boards/alpha/board.cmake; do
    : > "$TMP/root/$f"
done
{
    printf 'kickos_services_alpha\tsystem/init/alpha/a.cc\n'
    printf 'kickos_services_uni\tsystem/init/common/u.cc\n'
    printf 'kickos_services_extra\tsystem/init/alpha/e.cc\n'
} > "$TMP/t_prov"
{
    cat "$TMP/t_prov"
    printf 'kickos_services_ghost\tsystem/init/ghost/g.cc\n'
    printf 'kickos_services_plain\tsystem/plain/p.cc\n'
} > "$TMP/t_prov_all"
{
    printf 'alphapreset\talpha\talpha\n'
    printf 'otherpreset\tbravo\tbravo\n'
    printf 'nonepreset\t@none\t@none\n'
} > "$TMP/t_pres"
{
    printf 'kickos_services_alpha\tboards/alpha/Kconfig\n'
    printf 'kickos_services_uni\tKconfig\n'
} > "$TMP/t_kdef"

run_verdict() { # <rows> <providers>; findings land in $TMP/t_found, status is verdict's
    ( cd "$TMP/root" && verdict "$1" "$2" "$TMP/t_pres" "$TMP/t_kdef" \
        "$TMP/t_found" "$TMP/t_decl" ) > /dev/null 2> "$TMP/t_err"
}
found_count() { wc -l < "$TMP/t_found" | tr -d ' '; }

# Every clean row is silent for its OWN reason, so each is read alone as well.
cat > "$TMP/t_clean" <<'EOF'
kickos_services_alpha alphapreset default
kickos_services_uni otherpreset default
kickos_services_extra alphapreset select
EOF
run_verdict "$TMP/t_clean" "$TMP/t_prov" \
    || fail "the verdict legs refused a wholly conforming declaration: $(cat "$TMP/t_err")"
POS="$(found_count)"
[ "$POS" -eq 0 ] || fail "the verdict legs report $POS finding(s) against a conforming declaration; the gate would cry wolf and be switched off: $(cat "$TMP/t_found")"
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$TMP/t_one"
    awk -v p="${line%% *}" -F'\t' '$1 == p' "$TMP/t_prov" > "$TMP/t_one_prov"
    run_verdict "$TMP/t_one" "$TMP/t_one_prov" \
        || fail "clean control $i refused: $(cat "$TMP/t_err")"
    n="$(found_count)"
    [ "$n" -eq 0 ] || fail "clean control $i reports: $(cat "$TMP/t_found")"
done < "$TMP/t_clean"
[ "$i" -eq 3 ] || fail "$i clean control(s) ran, expected 3"

# One planted violation per reported clause, each read ALONE against the one provider row
# it names, so a finding raised by the wrong clause shows up as the wrong text and not as
# a pass. Every row differs from its clean partner above in ONE property.
cat > "$TMP/t_dirty" <<'EOF'
kickos_services_missing alphapreset select|which no kickos_add_board_provider() in this tree creates
kickos_services_plain alphapreset select|is not under an init/<board>/ directory
kickos_services_ghost alphapreset select|which is not a board (no boards/ghost/board.cmake)
kickos_services_alpha nosuchpreset select|which is not a visible configure preset
kickos_services_alpha nonepreset select|which resolves no KICKOS_BOARD
kickos_services_extra otherpreset select|whose board is bravo
kickos_services_extra alphapreset default|neither Kconfig nor boards/alpha/Kconfig spells
kickos_services_alpha alphapreset select|is declared select, but it is a Kconfig default in
EOF
i=0
while IFS= read -r line; do
    i=$((i + 1))
    row="${line%%|*}"
    printf '%s\n' "$row" > "$TMP/t_one"
    awk -v p="${row%% *}" -F'\t' '$1 == p' "$TMP/t_prov_all" > "$TMP/t_one_prov"
    run_verdict "$TMP/t_one" "$TMP/t_one_prov" \
        || fail "dirty control $i took a hard failure instead of reporting: $(cat "$TMP/t_err")"
    n="$(found_count)"
    [ "$n" -eq 1 ] || fail "dirty control $i reports $n finding(s), expected exactly 1: $row"
    grep -qF "${line#*|}" "$TMP/t_found" \
        || fail "dirty control $i reports the wrong clause: $(cat "$TMP/t_found")"
done < "$TMP/t_dirty"
[ "$i" -eq 8 ] || fail "$i dirty control(s) ran, expected 8"

# The other direction, on its own: two providers the planted tree creates and one row
# declares neither.
head -n 1 "$TMP/t_clean" > "$TMP/t_one"
run_verdict "$TMP/t_one" "$TMP/t_prov" || fail "the undeclared-provider leg refused: $(cat "$TMP/t_err")"
POS="$(found_count)"
[ "$POS" -eq 2 ] || fail "the undeclared-provider leg reports $POS of 2 providers the declaration omits"

# The hard failures, each in a subshell, so a control cannot take the whole gate down.
cat > "$TMP/t_hard" <<'EOF'
kickos_services_alpha alphapreset default junk|trailing junk after
not_a_provider alphapreset select|is not a service-list provider name
kickos_services_alpha alphapreset maybe|is not a selection; use default or select
EOF
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "${line%%|*}" > "$TMP/t_one"
    if run_verdict "$TMP/t_one" "$TMP/t_prov"; then
        fail "hard control $i was accepted: ${line%%|*}"
    fi
    grep -qF "${line#*|}" "$TMP/t_err" \
        || fail "hard control $i failed for the wrong reason: $(cat "$TMP/t_err")"
done < "$TMP/t_hard"
[ "$i" -eq 3 ] || fail "$i hard control(s) ran, expected 3"

printf 'kickos_services_alpha\tsystem/init/alpha/gone.cc\n' > "$TMP/t_one_prov"
head -n 1 "$TMP/t_clean" > "$TMP/t_one"
if run_verdict "$TMP/t_one" "$TMP/t_one_prov"; then
    fail "a SOURCE that is not a file was accepted; the scan could be broken and read clean"
fi
grep -qF "which is not a file; the scan is broken" "$TMP/t_err" \
    || fail "the missing-SOURCE control failed for the wrong reason: $(cat "$TMP/t_err")"

head -n 1 "$TMP/t_clean" > "$TMP/t_one"
head -n 1 "$TMP/t_clean" >> "$TMP/t_one"
if run_verdict "$TMP/t_one" "$TMP/t_prov"; then
    fail "a provider declared twice was accepted"
fi
grep -qF "declares a provider twice" "$TMP/t_err" \
    || fail "the duplicate control failed for the wrong reason: $(cat "$TMP/t_err")"

# The mutation the controls exist to survive: turn an exemption off and the count over the
# CLEAN corpus must move, so the control is a near miss. Disabled = a name no planted row
# carries, and the expected count is exact and differs per clause.
NEVER=KICKOS_THIS_NAME_IS_NOWHERE
( cd "$TMP/root" && COMMON_DIR="$NEVER" \
    && verdict "$TMP/t_clean" "$TMP/t_prov" "$TMP/t_pres" "$TMP/t_kdef" \
        "$TMP/t_found" "$TMP/t_decl" ) > /dev/null 2>&1
POS="$(found_count)"
[ "$POS" -eq 1 ] || fail "with the universal-directory exemption disabled the clean corpus reported $POS finding(s), expected 1;
      the control for it is not a near miss and proves nothing"
( cd "$TMP/root" && ROOT_KCONFIG="$NEVER" \
    && verdict "$TMP/t_clean" "$TMP/t_prov" "$TMP/t_pres" "$TMP/t_kdef" \
        "$TMP/t_found" "$TMP/t_decl" ) > /dev/null 2>&1
POS="$(found_count)"
[ "$POS" -eq 1 ] || fail "with the root Kconfig dropped from the default leg the clean corpus reported $POS finding(s), expected 1;
      the board's own Kconfig is answering for the universal provider too"

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

: > "$TMP/providers"
: > "$TMP/awkerr"
while IFS= read -r f; do
    parse_providers "$f" "$(dirname "$f")" >> "$TMP/providers" 2>> "$TMP/awkerr"
    rc=$?
    [ "$rc" -eq 0 ] || [ "$rc" -eq 2 ] || fail "awk exited $rc scanning $f"
done < "$TMP/cand"

if [ -s "$TMP/awkerr" ]; then
    cat "$TMP/awkerr" >&2
    fail "a $CALL(services_...) call could not be read; a provider it creates would need no declaration"
fi
require_nonempty "$TMP/providers" "not one provider came out of the scan"

: > "$TMP/opened"
while IFS= read -r f; do
    opened_calls "$f" >> "$TMP/opened"
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
    kconfig_defaults "$kf" >> "$TMP/kdefaults"
done < "$TMP/kconfigs"
require_nonempty "$TMP/kdefaults" "no Kconfig names a kickos_services_* default; the default leg would be vacuous"

# --- the declarations ----------------------------------------------------------
decl_rows "$DECL" > "$TMP/decl.txt"
require_nonempty "$TMP/decl.txt" "$DECL declares nothing"

COUNTS="$(verdict "$TMP/decl.txt" "$TMP/providers" "$TMP/presets" "$TMP/kdefaults" \
    "$TMP/findings.txt" "$TMP/declared.txt")" || exit 1
N_DEFAULT="${COUNTS%% *}"
N_SELECT="${COUNTS##* }"

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
