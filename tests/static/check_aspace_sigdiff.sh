#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The address-space seam's signature diff, which is what docs/design-m6-mmu.md step R5
# reports. F8 makes M6.3's deliverable a NEGATIVE result: the arch_aspace_* family's
# signatures do not move when a second, deliberately unlike backend is fitted to them, and
# where one moves, that diff IS the finding. Section 3.4b requires the API to exist before
# the second backend starts, so the verdict is a comparison against a named baseline commit
# rather than a reading of the header.
#
#   tests/static/check_aspace_sigdiff.sh [<baseline-ref> [<candidate-ref>]]
#
#   baseline   tests/static/aspace_seam_baseline.txt when no argument is given
#   candidate  the WORKING TREE when no second argument is given
#
# exit 0  PASS, the signature records are identical
# exit 1  FAIL, the comparison could not be made, so the verdict is UNKNOWN and not clean
# exit 2  DIFF, the records differ; the difference is the finding
#
# KOS_SIGDIFF_KEEP=<dir> copies the two record sets out for a report to cite.
#
#   corpus     the tracked seam headers, arch/include/kickos/arch/*.h, read out of each ref
#              with `git show` so neither side depends on the checkout
#   comments   tests/lib/strip_comments.awk blanks `//`, `/* */` and every literal first,
#              so a comment naming arch_aspace_map cannot enter the corpus
#   members    by declared IDENTIFIER prefix, in tests/static/aspace_seam.awk, NOT by the
#              section banner: a re-wrapped or deleted banner comment would shrink a
#              banner-keyed corpus to nothing and this script would then report an empty
#              diff, which is the one false PASS that would make step R5 worthless
#   floors     a per-kind AND a per-GROUP minimum on BOTH sides below, so an extraction
#              that read nothing, or that lost one alternative of the family whole, fails
#              loudly instead of comparing two empty sets and passing. The two partitions
#              cross-cut and neither subsumes the other: a kind is a declaration FORM and a
#              group is a family alternative, so a lost alternative can hide inside a kind
#              other alternatives also populate. Measured on this family: of the five
#              alternatives in the extractor's PREFIX, dropping `arch_map` costs exactly one
#              TAG record of three and clears every per-kind floor, which is the false PASS
#              the group floors close.
#
# RUN THIS UNDER BOTH /bin/sh AND bash BEFORE TRUSTING IT. The group table was held in a
# variable named GROUPS, which bash owns as an auto-maintained array of the caller's group ids:
# the assignment did not take, the whole table expanded to one gid, every per-group floor went
# empty, and the script still printed its record counts and its diff and still exited 2. The
# group floors are the only thing this instrument adds over a bare read of the header, so under
# bash the recorded evidence was a header read wearing this differ's report, and nothing in the
# exit code or the DIFF line said so. The table is now parsed ONCE, validated, and its row count
# checked against KOS_GROUP_ROWS, so a shell that mangles it stops here instead.

set -eu
. "$(dirname "$0")/../lib/gate.sh"

HERE="$(dirname "$0")"
STRIP="$HERE/../lib/strip_comments.awk"
EXTRACT="$HERE/aspace_seam.awk"
BASEFILE="$HERE/aspace_seam_baseline.txt"

# The seam header the family lives in today. Its absence on either side means the corpus
# was built from the wrong path, which would otherwise read as a clean empty diff.
ANCHOR="arch/include/kickos/arch/arch.h"

# Per-kind minimum record counts. Set below today's figures so a legitimate removal of a
# member does not trip them, and far above zero so a broken extraction does.
MIN_FUNC=8
MIN_ENUMERATOR=8
MIN_MACRO=5
MIN_TYPEDEF=1
MIN_TAG=1
MIN_TOTAL=24

# The group table. One line per group: <name> <name-regex> <floor>. The regexes are the
# alternatives of PREFIX in tests/static/aspace_seam.awk, one group each, which is what makes
# the classification check below bind this table to the family definition: an alternative
# added there with no group here fails loudly instead of going unfloored.
#
# A group of ONE carries a floor of 1, so a legitimate removal of that member does trip it.
# That is deliberate and it is the difference from the per-kind floors above: dropping the
# only record an alternative contributes is exactly the loss no kind floor can see, and a
# family this small cannot tell that apart from a legitimate removal. Re-deciding the floor
# is the right cost for removing `arch_phys_addr_t` or the memory-type enum from the seam.
#
# NOT NAMED `GROUPS`: bash owns that identifier as the caller's group ids, and an assignment to
# it does not take. KOS_GROUP_ROWS is the count build_group_table below checks the parse
# against, so a shell that mangles this text fails loudly instead of dropping every floor.
KOS_GROUP_ROWS=5
KOS_GROUP_TABLE="
calls     ^arch_aspace          12
codes     ^ARCH_ASPACE          11
memtag    ^arch_map              1
mapbits   ^ARCH_MAP              5
physaddr  ^arch_phys_addr        1
"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; neither side can be read"
[ -r "$STRIP" ] || fail "unreadable: $STRIP; nothing below could strip a comment"
[ -r "$EXTRACT" ] || fail "unreadable: $EXTRACT; there is no extractor"

BASE="${1:-}"
if [ -z "$BASE" ]; then
    [ -r "$BASEFILE" ] || fail "unreadable: $BASEFILE, and no baseline ref was given"
    BASE="$(sed -e 's/#.*$//' -e '/^[[:space:]]*$/d' "$BASEFILE" | head -n1 | tr -d '[:space:]')"
    [ -n "$BASE" ] || fail "$BASEFILE names no ref"
fi
CAND="${2:-}"

case "$BASE" in
    spike/*)
        fail "a spike branch is not a baseline"
        ;;
esac

BASE_SHA="$(git rev-parse --verify "$BASE^{commit}" 2>/dev/null)" \
    || fail "baseline ref does not resolve to a commit: $BASE"
if [ -n "$CAND" ]; then
    CAND_SHA="$(git rev-parse --verify "$CAND^{commit}" 2>/dev/null)" \
        || fail "candidate ref does not resolve to a commit: $CAND"
    CAND_DESC="$CAND_SHA"
else
    CAND_SHA=""
    CAND_DESC="working tree"
fi

scratch_dir

# The group table, parsed ONCE into $TMP/groups as three TAB-separated fields per row, and every
# consumer below reads that file. Fed by a here-document rather than a pipeline so the loop runs
# in THIS shell: a refusal inside a pipeline's subshell would print and be ignored.
build_group_table() {
    : > "$TMP/groups"
    while read -r _g _re _fl _extra; do
        if [ -z "$_g" ]; then
            continue
        fi
        if [ -z "$_re" ] || [ -z "$_fl" ] || [ -n "$_extra" ]; then
            fail "group table row \"$_g $_re $_fl $_extra\" is not
      <name> <name-regex> <floor>, three whitespace-separated fields"
        fi
        case "$_fl" in
            '' | *[!0-9]*)
                fail "group table floor \"$_fl\" for group \"$_g\" is not a number"
                ;;
        esac
        printf '%s\t%s\t%s\n' "$_g" "$_re" "$_fl" >> "$TMP/groups"
    done <<KOS_TABLE_END
$KOS_GROUP_TABLE
KOS_TABLE_END
    _rows="$(grep -c . "$TMP/groups" || true)"
    if [ "$_rows" -ne "$KOS_GROUP_ROWS" ]; then
        fail "the group table parsed to $_rows row(s), not $KOS_GROUP_ROWS. This shell mangled
      it, so every per-group floor below would be empty and the verdict would be a bare header
      read with none of the family membership this instrument adds"
    fi
}

build_group_table

# Records for one side into $TMP/<tag>.all. An empty ref means the working tree.
extract_side() { # <tag> <ref-or-empty>
    _tag="$1"
    _ref="$2"
    _n=0
    : > "$TMP/$_tag.files"
    : > "$TMP/$_tag.all"
    : > "$TMP/$_tag.refused"
    if [ -n "$_ref" ]; then
        git ls-tree -r --name-only -- "$_ref" arch/include/kickos/arch/ \
            > "$TMP/$_tag.ls" || fail "git ls-tree failed for $_ref"
    else
        # `git ls-files`, not find: an untracked scratch header is neither read nor counted.
        git ls-files -- 'arch/include/kickos/arch/*' > "$TMP/$_tag.ls" \
            || fail "git ls-files failed"
    fi
    while IFS= read -r f; do
        case "$f" in
            *.h) ;;
            *) continue ;;
        esac
        printf '%s\n' "$f" >> "$TMP/$_tag.files"
        _n=$((_n + 1))
        if [ -n "$_ref" ]; then
            git show "$_ref:$f" > "$TMP/src" 2>/dev/null \
                || fail "git show $_ref:$f failed"
        else
            [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
            cat "$f" > "$TMP/src"
        fi
        # A refusal from either awk means the file could not be read, NOT that it is clean.
        if awk -f "$STRIP" "$TMP/src" > "$TMP/stripped" 2>> "$TMP/$_tag.refused"; then
            :
        else
            _rc=$?
            [ "$_rc" -eq 2 ] || fail "strip_comments.awk exited $_rc on $_tag:$f"
            printf 'strip refused %s\n' "$f" >> "$TMP/$_tag.refused"
            continue
        fi
        if awk -f "$EXTRACT" "$TMP/stripped" >> "$TMP/$_tag.all" 2>> "$TMP/$_tag.refused"; then
            :
        else
            _rc=$?
            [ "$_rc" -eq 2 ] || fail "aspace_seam.awk exited $_rc on $_tag:$f"
            printf 'extract refused %s\n' "$f" >> "$TMP/$_tag.refused"
        fi
    done < "$TMP/$_tag.ls"
    if [ -s "$TMP/$_tag.refused" ]; then
        echo "FAIL: $_tag could not be scanned, so its verdict is UNKNOWN, not clean:" >&2
        sed 's/^/      /' "$TMP/$_tag.refused" >&2
        exit 1
    fi
    grep -Fxq "$ANCHOR" "$TMP/$_tag.files" \
        || fail "$_tag corpus does not contain $ANCHOR; the corpus was built from the wrong path"
    eval "${_tag}_FILES=$_n"
}

kind_count() { # <tag> <kind>
    awk -F"$TAB" -v K="$2" '$1 == K { n++ } END { print n + 0 }' "$TMP/$1.sig"
}

group_count() { # <tag> <name-regex>
    awk -F"$TAB" -v RE="$2" '$2 ~ RE { n++ } END { print n + 0 }' "$TMP/$1.sig"
}

# Every record classifies into exactly one group, or the table and the extractor's PREFIX
# have drifted apart. A member the family admits and no group claims would sit outside every
# group floor, which is the hole those floors exist to close.
check_classified() { # <tag>
    _t="$1"
    awk -F"$TAB" '{ print $2 }' "$TMP/$_t.sig" | LC_ALL=C sort -u > "$TMP/$_t.members"
    _bad=""
    while IFS= read -r _m; do
        [ -n "$_m" ] || continue
        while IFS="$TAB" read -r _g _re _fl; do
            if printf '%s\n' "$_m" | grep -Eq "$_re"; then
                printf '%s\n' "$_g"
            fi
        done < "$TMP/groups" > "$TMP/hits"
        _hits="$(tr '\n' ' ' < "$TMP/hits" | sed 's/ *$//')"
        _nh="$(grep -c . "$TMP/hits" || true)"
        if [ "$_nh" -ne 1 ]; then
            _bad="$_bad
      $_m: $_nh group(s) [$_hits]"
        fi
    done < "$TMP/$_t.members"
    if [ -n "$_bad" ]; then
        echo "FAIL: the $_t records do not classify one-to-one against the group table, so" >&2
        echo "      the group floors below do not cover the family. Fix the table in this" >&2
        echo "      script or the PREFIX in aspace_seam.awk:$_bad" >&2
        exit 1
    fi
}

# The floors. Applied to EACH side: comparing two empty record sets yields no difference,
# which is the false PASS this whole instrument exists to rule out.
check_floors() { # <tag>
    _t="$1"
    _bad=""
    for _k in FUNC ENUMERATOR MACRO TYPEDEF TAG; do
        _have="$(kind_count "$_t" "$_k")"
        eval "_min=\$MIN_$_k"
        if [ "$_have" -lt "$_min" ]; then
            _bad="$_bad
      $_k: $_have record(s), floor $_min"
        fi
    done
    _tot="$(wc -l < "$TMP/$_t.sig" | tr -d ' ')"
    if [ "$_tot" -lt "$MIN_TOTAL" ]; then
        _bad="$_bad
      total: $_tot record(s), floor $MIN_TOTAL"
    fi
    while IFS="$TAB" read -r _g _re _fl; do
        _have="$(group_count "$_t" "$_re")"
        if [ "$_have" -lt "$_fl" ]; then
            printf '      group %s: %s record(s), floor %s\n' "$_g" "$_have" "$_fl"
        fi
    done < "$TMP/groups" > "$TMP/$_t.lowgroups"
    if [ -s "$TMP/$_t.lowgroups" ]; then
        _bad="$_bad
$(cat "$TMP/$_t.lowgroups")"
    fi
    if [ -n "$_bad" ]; then
        echo "FAIL: the $_t extraction is below its floor, so it read part of the family or" >&2
        echo "      none of it. An empty or short corpus must not compare clean:$_bad" >&2
        exit 1
    fi
}

split_side() { # <tag>
    awk -F"$TAB" '$1 != "FUNCNAMES"' "$TMP/$1.all" | LC_ALL=C sort > "$TMP/$1.sig"
    awk -F"$TAB" '$1 == "FUNCNAMES"' "$TMP/$1.all" | LC_ALL=C sort > "$TMP/$1.names"
}

extract_side base "$BASE_SHA"
extract_side cand "$CAND_SHA"
split_side base
split_side cand
check_classified base
check_classified cand
check_floors base
check_floors cand

if [ -n "${KOS_SIGDIFF_KEEP:-}" ]; then
    mkdir -p "$KOS_SIGDIFF_KEEP" || fail "cannot create $KOS_SIGDIFF_KEEP"
    cp "$TMP/base.sig" "$TMP/cand.sig" "$TMP/base.names" "$TMP/cand.names" \
       "$KOS_SIGDIFF_KEEP/" || fail "cannot copy records to $KOS_SIGDIFF_KEEP"
fi

echo "== address-space seam signature diff =="
echo "   baseline  $BASE_SHA ($base_FILES seam header(s))"
echo "   candidate $CAND_DESC ($cand_FILES seam header(s))"
echo "   members   $(wc -l < "$TMP/base.sig" | tr -d ' ') baseline signature record(s),"\
     "$(wc -l < "$TMP/cand.sig" | tr -d ' ') candidate"
echo "             FUNC $(kind_count base FUNC)/$(kind_count cand FUNC)," \
     "ENUMERATOR $(kind_count base ENUMERATOR)/$(kind_count cand ENUMERATOR)," \
     "MACRO $(kind_count base MACRO)/$(kind_count cand MACRO)," \
     "TYPEDEF $(kind_count base TYPEDEF)/$(kind_count cand TYPEDEF)," \
     "TAG $(kind_count base TAG)/$(kind_count cand TAG)"
while IFS="$TAB" read -r _g _re _fl; do
    printf '             group %-9s %s/%s (floor %s)\n' \
        "$_g" "$(group_count base "$_re")" "$(group_count cand "$_re")" "$_fl"
done < "$TMP/groups"
echo "   family    identifiers matching arch_aspace / ARCH_ASPACE / arch_map / ARCH_MAP /"
echo "             arch_phys_addr, wherever they stand in the seam headers"
echo
echo "   the rule. REPORTED as a signature difference:"
echo "     a parameter TYPE, a return type, a parameter COUNT or a parameter ORDER;"
echo "     an enumeration constant's VALUE; an object-like macro's VALUE; a typedef's"
echo "     underlying type; a member added, removed or renamed; a preprocessor guard"
echo "     around a member."
echo "   NOT reported as a signature difference:"
echo "     a parameter RENAME; the order declarations appear in a file; which seam header"
echo "     a member is declared in; comment text and whitespace; a value expression"
echo "     rewritten without changing the value it evaluates to."
echo "   A value the extractor cannot evaluate is carried as its canonical TEXT, so an"
echo "   unevaluable expression is compared strictly rather than assumed unchanged."
echo

RC=0
if cmp -s "$TMP/base.sig" "$TMP/cand.sig"; then
    echo "PASS: no signature difference"
else
    echo "DIFF: the signature records moved. Per F8 this diff IS the finding." >&2
    echo "      < baseline, > candidate" >&2
    diff "$TMP/base.sig" "$TMP/cand.sig" | sed 's/^/      /' >&2 || true
    RC=2
fi

# Only when the verdict above is PASS: a rename is then the ONLY thing that moved, and
# saying so is what keeps "not a signature change" from reading as "nothing happened".
if [ "$RC" -eq 0 ] && ! cmp -s "$TMP/base.names" "$TMP/cand.names"; then
    echo
    echo "NOTE: parameter NAMES differ and nothing else does. Not a signature change by the"
    echo "      rule above, so the verdict stays PASS. Shown so a rename is not silent:"
    diff "$TMP/base.names" "$TMP/cand.names" | sed 's/^/      /' || true
fi

exit "$RC"
