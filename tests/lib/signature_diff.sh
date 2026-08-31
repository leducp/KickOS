# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The signature-diff driver behind tests/static/check_aspace_sigdiff.sh and
# tests/static/check_entry_sigdiff.sh. SOURCED after tests/lib/gate.sh, never executed:
#   . "$(dirname "$0")/../lib/signature_diff.sh"
# POSIX sh (dash-clean), because /bin/sh is dash on the CI images.
#
# A caller declares one family, then calls sigdiff_run with its own arguments:
#
#   <caller> [<candidate-ref>]
#
#   baseline   the frozen records checked in as the file KOS_SD_RECORDS names
#   candidate  the WORKING TREE when no argument is given
#
# exit 0  PASS, the signature records are identical
# exit 1  FAIL, the comparison could not be made, so the verdict is UNKNOWN and not clean
# exit 2  DIFF, the records differ; the difference is the finding
#
# KOS_SIGDIFF_KEEP=<dir> copies the two record sets out for a report to cite.
#
# KOS_SIGDIFF_REGEN=1 writes the candidate records over KOS_SD_RECORDS and exits 1, having
# taken no verdict. It keeps that file's own comment header and refuses a file that has
# none, so the cost of regenerating stays written where the next reader meets it.
#
#   baseline   the records themselves, checked in, one file per family. A baseline that is
#              absent, empty, or carries a line this driver cannot parse REFUSES. A file
#              read as zero records differs from no candidate and would pass everything.
#   corpus     the candidate's tracked seam headers, named by the caller's own pathspecs in
#              KOS_SD_CORPUS, taken from the working tree or read out of a ref with `git show`
#   comments   tests/lib/strip_comments.awk blanks `//`, `/* */` and every literal first,
#              so a comment naming a member cannot enter the corpus
#   members    by declared IDENTIFIER prefix, never by the section banner: a re-wrapped or
#              deleted banner comment would shrink a banner-keyed corpus to nothing and a
#              caller would then report an empty diff, which is the one false PASS that
#              would make its step worthless
#   floors     a per-kind AND a per-GROUP minimum on BOTH sides, so an extraction that read
#              nothing, or that lost one alternative of the family whole, fails loudly
#              instead of comparing two short sets and passing. The two partitions
#              cross-cut and neither subsumes the other: a kind is a declaration FORM and a
#              group is a family alternative, so a lost alternative can hide inside a kind
#              other alternatives also populate.
#   control    a known-answer fixture classified by the extractor BEFORE either side is read.
#              A floor can only say a corpus was non-empty; nothing else here establishes
#              that the extractor can SEE a difference, and a verdict of no-difference from
#              an instrument never exercised is not evidence. The extractor is the only
#              thing both callers share, so the control lives here and serves both.
#
# WHAT THE CALLER SETS BEFORE sigdiff_run
#   KOS_SD_TITLE         the headline this verdict prints
#   KOS_SD_EXTRACT       the awk extractor
#   KOS_SD_FAMILY        an awk program named after the extractor, whose BEGIN block
#                        replaces the family PREFIX; empty when the extractor's own PREFIX
#                        is already this caller's family
#   KOS_SD_FAMILY_MSG    the refusal when KOS_SD_FAMILY is set and unreadable
#   KOS_SD_RECORDS       the frozen records, as a path from the repo root
#   KOS_SD_CORPUS        the pathspecs the seam headers are read from, one per row,
#                        KOS_SD_CORPUS_ROWS their count
#   KOS_SD_ANCHOR        the seam header whose absence means the wrong corpus
#   KOS_SD_MIN_FILES     the floor on how many files each side's corpus reads
#   KOS_SD_KINDS         the kinds carrying a floor, KOS_SD_MIN_<KIND> each floor
#   KOS_SD_KIND_LABEL    what a kind is called in the floor refusal
#   KOS_SD_MIN_TOTAL     the floor on the whole record set
#   KOS_SD_REPORT_KINDS  the kinds the report line carries, which is not the floored set
#   KOS_SD_PREFIX_FILE   the file the classification refusal names as the place to fix
#   KOS_GROUP_TABLE      rows of <name> <name-regex> <floor>, KOS_GROUP_ROWS their count
#   KOS_SD_MANGLED_TAIL  what a mangled group table costs this caller's verdict
#   sigdiff_family_prose a function printing the membership lines of the report
#   sigdiff_rule_opening a function printing the lines that open the rule paragraph
#
# RUN EVERY CALLER UNDER BOTH /bin/sh AND bash BEFORE TRUSTING IT. The group table was held
# in a variable named GROUPS, which bash owns as an auto-maintained array of the caller's
# group ids: the assignment did not take, the whole table expanded to one gid, every
# per-group floor went empty, and the caller still printed its record counts and its
# verdict and still exited with the code it exits with under dash. The group floors are the
# only thing this instrument adds over a bare read of the header, so under bash the recorded
# evidence was a header read wearing a differ's report, and nothing in the exit code or the
# verdict line said so. The table is parsed ONCE, validated, and its row count checked
# against KOS_GROUP_ROWS, so a shell that mangles it stops there instead.

# Every caller lives one level below tests/, so the comment stripper sits beside this file.
KOS_SD_STRIP="$(dirname "$0")/../lib/strip_comments.awk"

sigdiff_preflight() {
    [ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
    # `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
    [ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
    command -v git >/dev/null 2>&1 || fail "git not found; neither side can be read"
    [ -r "$KOS_SD_STRIP" ] \
        || fail "unreadable: $KOS_SD_STRIP; nothing below could strip a comment"
    [ -r "$KOS_SD_EXTRACT" ] || fail "unreadable: $KOS_SD_EXTRACT; there is no extractor"
    if [ -n "$KOS_SD_FAMILY" ]; then
        [ -r "$KOS_SD_FAMILY" ] || fail "$KOS_SD_FAMILY_MSG"
    fi
}

sigdiff_resolve_candidate() { # <candidate-ref-or-empty>
    if [ -z "$1" ]; then
        KOS_SD_CAND_SHA=""
        KOS_SD_CAND_DESC="working tree"
        return
    fi
    case "$1" in
        spike/*)
            fail "a spike branch is not a candidate"
            ;;
    esac
    KOS_SD_CAND_SHA="$(git rev-parse --verify "$1^{commit}" 2>/dev/null)" \
        || fail "candidate ref does not resolve to a commit: $1"
    KOS_SD_CAND_DESC="$KOS_SD_CAND_SHA"
}

# Every kind tests/static/aspace_seam.awk emits. A baseline line carrying anything else is a
# line this driver cannot classify, and the floors below would then count it as missing.
KOS_SD_KIND_SET="ENUMERATOR FUNC FUNCNAMES MACRO MACROFN OBJ TAG TYPEDEF"

# The baseline side, whole, or nothing. Each refusal below covers one way a file can arrive
# short while still being read: gone, empty, a line the parse drops, a line duplicated by a
# merge. All four leave a corpus that differs from less of the candidate than it should.
sigdiff_read_baseline() {
    [ -r "$KOS_SD_RECORDS" ] \
        || fail "unreadable: $KOS_SD_RECORDS, which holds the whole baseline side"
    : > "$TMP/base.all"
    awk -F"$TAB" -v KINDS="$KOS_SD_KIND_SET" -v OUT="$TMP/base.all" '
        BEGIN {
            n = split(KINDS, k, " ")
            for (i = 1; i <= n; i++) { ok[k[i]] = 1 }
        }
        /^#/ { next }
        /^[ \t]*$/ { next }
        {
            if (NF != 4) {
                printf("line %d: %d TAB-separated field(s), 4 expected\n", NR, NF)
                next
            }
            if (!($1 in ok)) {
                printf("line %d: kind \"%s\" is none of: %s\n", NR, $1, KINDS)
                next
            }
            if ($2 == "" || $3 == "" || $4 == "") {
                printf("line %d: an empty field\n", NR)
                next
            }
            if ($0 in seen) {
                printf("line %d: repeats line %d\n", NR, seen[$0])
                next
            }
            seen[$0] = NR
            print > OUT
        }
    ' "$KOS_SD_RECORDS" > "$TMP/base.bad" 2> "$TMP/base.awkerr" \
        || fail "awk failed over $KOS_SD_RECORDS, so the baseline is UNKNOWN"
    if [ -s "$TMP/base.awkerr" ]; then
        echo "FAIL: reading $KOS_SD_RECORDS drew output on stderr, so the baseline is" >&2
        echo "      UNKNOWN and the diff below would be taken over part of it:" >&2
        sed 's/^/      /' "$TMP/base.awkerr" >&2
        exit 1
    fi
    if [ -s "$TMP/base.bad" ]; then
        echo "FAIL: $KOS_SD_RECORDS does not parse, so the baseline is UNKNOWN. Every" >&2
        echo "      record is four TAB-separated fields, <kind> <name> <detail> <guard>:" >&2
        sed 's/^/      /' "$TMP/base.bad" >&2
        exit 1
    fi
    KOS_SD_BASE_N="$(grep -c . "$TMP/base.all" || true)"
    if [ "$KOS_SD_BASE_N" -eq 0 ]; then
        fail "$KOS_SD_RECORDS holds no record. An empty baseline matches every candidate,
      so this run would report no difference over a family it never read"
    fi
}

# The candidate records, over the baseline file, with that file's own comment header kept.
sigdiff_regenerate() {
    [ -r "$KOS_SD_RECORDS" ] \
        || fail "unreadable: $KOS_SD_RECORDS; regeneration keeps that file's own header and
      there is none here to keep"
    awk '/^#/ { print; next } /^[ \t]*$/ { print; next } { exit }' "$KOS_SD_RECORDS" \
        > "$TMP/header" || fail "cannot read the header of $KOS_SD_RECORDS"
    grep -q '^#' "$TMP/header" \
        || fail "$KOS_SD_RECORDS opens with no comment header, which is where what this
      file costs to regenerate is written"
    cp "$TMP/header" "$TMP/regen" || fail "cannot stage a new $KOS_SD_RECORDS"
    LC_ALL=C sort "$TMP/cand.all" >> "$TMP/regen" || fail "cannot sort the candidate records"
    cp "$TMP/regen" "$KOS_SD_RECORDS" || fail "cannot write $KOS_SD_RECORDS"
    echo "== $KOS_SD_TITLE =="
    printf '   REGENERATED %s from %s, %s record(s)\n' \
        "$KOS_SD_RECORDS" "$KOS_SD_CAND_DESC" "$(grep -c . "$TMP/cand.all" || true)"
    echo "   No verdict was taken. Read what this moved in git before keeping it."
    exit 1
}

# The group table, parsed ONCE into $TMP/groups as three TAB-separated fields per row, and
# every consumer below reads that file. Fed by a here-document rather than a pipeline so the
# loop runs in THIS shell: a refusal inside a pipeline's subshell would print and be ignored.
sigdiff_group_table() {
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
      it, so $KOS_SD_MANGLED_TAIL"
    fi
}

# The corpus pathspecs, parsed ONCE into $TMP/corpus and checked against the count the caller
# declares. Fed by a here-document for the reason sigdiff_group_table is: a refusal inside a
# pipeline's subshell would print and be ignored. A pathspec that word-splits differently under
# one shell would silently narrow the corpus, which is the GROUPS failure one file over:
# there it emptied the floors, here it would empty the seam and compare two short sets clean.
sigdiff_corpus_table() {
    : > "$TMP/corpus"
    while read -r _p _extra; do
        if [ -z "$_p" ]; then
            continue
        fi
        if [ -n "$_extra" ]; then
            fail "corpus row \"$_p $_extra\" is not a single pathspec. A path holding a space
      cannot be carried here"
        fi
        case "$_p" in
            /* | *..*)
                fail "corpus pathspec \"$_p\" is not relative to the repo root"
                ;;
        esac
        printf '%s\n' "$_p" >> "$TMP/corpus"
    done <<KOS_CORPUS_END
$KOS_SD_CORPUS
KOS_CORPUS_END
    _rows="$(grep -c . "$TMP/corpus" || true)"
    if [ "$_rows" -ne "$KOS_SD_CORPUS_ROWS" ]; then
        fail "the corpus table parsed to $_rows pathspec(s), not $KOS_SD_CORPUS_ROWS. This shell
      mangled it, so the seam below would be read from part of its own headers"
    fi
}

# The extractor's positive control, run before either side is extracted and before any
# floor. The fixture is PLANTED rather than tracked: a corpus file can be truncated or
# swapped without a gate noticing, and this one cannot be without editing this file.
#
# The parameter forms are the point of it. An UNNAMED parameter whose type ends in a plain
# identifier once lost that identifier, so `const kos_ctl_flags` and `const kos_ctl_word`
# both extracted as `const`: two unlike types held one record and a real type change read as
# no change. Every such form here carries a NAMED twin that must extract to the SAME record,
# which is the parameter-rename clause of the printed rule. The forms that were never wrong
# are here one each, so a repair that breaks one of them fails here too.
#
# CONTROL_MIN is the exact record count, not a margin: the fixture only changes when someone
# edits this file, so a drop is an edit that removed coverage and a rise is one that added
# it. Removing a form means re-deciding the number.
KOS_SD_CONTROL_MIN=47

sigdiff_control() {
    mkdir "$TMP/ctl" || fail "cannot create $TMP/ctl"
    cat > "$TMP/ctl/family.awk" <<'KOS_CTL_FAMILY_END'
BEGIN { PREFIX = "^(kos_ctl|KOS_CTL)" }
KOS_CTL_FAMILY_END
    cat > "$TMP/ctl/fixture.h" <<'KOS_CTL_FIXTURE_END'
typedef unsigned long kos_ctl_word;
typedef unsigned long kos_ctl_addr_t;
typedef unsigned long kos_ctl_flags;

struct kos_ctl_space;

enum kos_ctl_kind {
    KOS_CTL_KIND_ONE = 0,
    KOS_CTL_KIND_TWO,
    KOS_CTL_KIND_WIDE = 1 << 4,
    KOS_CTL_KIND_BOTH = 1 << 4 | 3,
    KOS_CTL_KIND_OPEN = KOS_CTL_LIMIT
};

#define KOS_CTL_LIMIT (1 << 12)
#define KOS_CTL_ROUND(n) (((n) + 7) & ~7)

extern kos_ctl_word kos_ctl_counter;

void kos_ctl_tag_unnamed(struct kos_ctl_space);
void kos_ctl_tag_named(struct kos_ctl_space sp);
void kos_ctl_alias_unnamed(const kos_ctl_flags);
void kos_ctl_alias_named(const kos_ctl_flags fl);
void kos_ctl_enum_unnamed(enum kos_ctl_kind);
void kos_ctl_enum_named(enum kos_ctl_kind kind);
void kos_ctl_qual_tag_unnamed(const struct kos_ctl_space);
void kos_ctl_qual_tag_named(const struct kos_ctl_space sp);

void kos_ctl_word_unnamed(kos_ctl_word);
void kos_ctl_suffix_unnamed(kos_ctl_addr_t);
void kos_ctl_base_unnamed(unsigned long);
void kos_ctl_base_named(unsigned long len);
void kos_ctl_ptr_unnamed(struct kos_ctl_space *);
void kos_ctl_ptr_named(struct kos_ctl_space * sp);
void kos_ctl_void(void);
enum kos_ctl_kind kos_ctl_mixed(struct kos_ctl_space * sp, const kos_ctl_flags, unsigned long n);

#ifdef KOS_CTL_GUARDED
void kos_ctl_guarded(kos_ctl_word);
#endif
KOS_CTL_FIXTURE_END
    # Four fields per row, written with a visible separator so no literal tab lives in this
    # file; the records the extractor prints are converted to the same shape below.
    cat > "$TMP/ctl/expect" <<'KOS_CTL_EXPECT_END'
ENUMERATOR :: KOS_CTL_KIND_BOTH :: = 19 :: -
ENUMERATOR :: KOS_CTL_KIND_ONE :: = 0 :: -
ENUMERATOR :: KOS_CTL_KIND_OPEN :: = expr:KOS_CTL_LIMIT :: -
ENUMERATOR :: KOS_CTL_KIND_TWO :: = 1 :: -
ENUMERATOR :: KOS_CTL_KIND_WIDE :: = 16 :: -
FUNC :: kos_ctl_alias_named :: void (const kos_ctl_flags) :: -
FUNC :: kos_ctl_alias_unnamed :: void (const kos_ctl_flags) :: -
FUNC :: kos_ctl_base_named :: void (unsigned long) :: -
FUNC :: kos_ctl_base_unnamed :: void (unsigned long) :: -
FUNC :: kos_ctl_enum_named :: void (enum kos_ctl_kind) :: -
FUNC :: kos_ctl_enum_unnamed :: void (enum kos_ctl_kind) :: -
FUNC :: kos_ctl_guarded :: void (kos_ctl_word) :: defined(KOS_CTL_GUARDED)
FUNC :: kos_ctl_mixed :: enum kos_ctl_kind (struct kos_ctl_space *, const kos_ctl_flags, unsigned long) :: -
FUNC :: kos_ctl_ptr_named :: void (struct kos_ctl_space *) :: -
FUNC :: kos_ctl_ptr_unnamed :: void (struct kos_ctl_space *) :: -
FUNC :: kos_ctl_qual_tag_named :: void (const struct kos_ctl_space) :: -
FUNC :: kos_ctl_qual_tag_unnamed :: void (const struct kos_ctl_space) :: -
FUNC :: kos_ctl_suffix_unnamed :: void (kos_ctl_addr_t) :: -
FUNC :: kos_ctl_tag_named :: void (struct kos_ctl_space) :: -
FUNC :: kos_ctl_tag_unnamed :: void (struct kos_ctl_space) :: -
FUNC :: kos_ctl_void :: void (void) :: -
FUNC :: kos_ctl_word_unnamed :: void (kos_ctl_word) :: -
FUNCNAMES :: kos_ctl_alias_named :: void (const kos_ctl_flags fl) :: -
FUNCNAMES :: kos_ctl_alias_unnamed :: void (const kos_ctl_flags) :: -
FUNCNAMES :: kos_ctl_base_named :: void (unsigned long len) :: -
FUNCNAMES :: kos_ctl_base_unnamed :: void (unsigned long) :: -
FUNCNAMES :: kos_ctl_enum_named :: void (enum kos_ctl_kind kind) :: -
FUNCNAMES :: kos_ctl_enum_unnamed :: void (enum kos_ctl_kind) :: -
FUNCNAMES :: kos_ctl_guarded :: void (kos_ctl_word) :: defined(KOS_CTL_GUARDED)
FUNCNAMES :: kos_ctl_mixed :: enum kos_ctl_kind (struct kos_ctl_space * sp, const kos_ctl_flags, unsigned long n) :: -
FUNCNAMES :: kos_ctl_ptr_named :: void (struct kos_ctl_space * sp) :: -
FUNCNAMES :: kos_ctl_ptr_unnamed :: void (struct kos_ctl_space *) :: -
FUNCNAMES :: kos_ctl_qual_tag_named :: void (const struct kos_ctl_space sp) :: -
FUNCNAMES :: kos_ctl_qual_tag_unnamed :: void (const struct kos_ctl_space) :: -
FUNCNAMES :: kos_ctl_suffix_unnamed :: void (kos_ctl_addr_t) :: -
FUNCNAMES :: kos_ctl_tag_named :: void (struct kos_ctl_space sp) :: -
FUNCNAMES :: kos_ctl_tag_unnamed :: void (struct kos_ctl_space) :: -
FUNCNAMES :: kos_ctl_void :: void (void) :: -
FUNCNAMES :: kos_ctl_word_unnamed :: void (kos_ctl_word) :: -
MACRO :: KOS_CTL_LIMIT :: = 4096 :: -
MACROFN :: KOS_CTL_ROUND :: (n) (((n) + 7) & ~7) :: -
OBJ :: kos_ctl_counter :: extern kos_ctl_word :: -
TAG :: kos_ctl_kind :: enum complete :: -
TAG :: kos_ctl_space :: struct incomplete :: -
TYPEDEF :: kos_ctl_addr_t :: unsigned long :: -
TYPEDEF :: kos_ctl_flags :: unsigned long :: -
TYPEDEF :: kos_ctl_word :: unsigned long :: -
KOS_CTL_EXPECT_END
    KOS_SD_CONTROL_N="$(grep -c . "$TMP/ctl/expect" || true)"
    if [ "$KOS_SD_CONTROL_N" -ne "$KOS_SD_CONTROL_MIN" ]; then
        fail "the control expects $KOS_SD_CONTROL_N record(s), not $KOS_SD_CONTROL_MIN. An
      emptied or truncated expectation matches an extractor that read nothing, so the
      control would assert nothing and the verdict below would rest on it anyway"
    fi
    awk -f "$KOS_SD_STRIP" "$TMP/ctl/fixture.h" > "$TMP/ctl/stripped" 2> "$TMP/ctl/err"         || fail "strip_comments.awk refused the control fixture, so the extractor below was
      never exercised: $(cat "$TMP/ctl/err")"
    awk -f "$KOS_SD_EXTRACT" -f "$TMP/ctl/family.awk" "$TMP/ctl/stripped"         > "$TMP/ctl/raw" 2>> "$TMP/ctl/err"         || fail "the extractor refused the control fixture, so it has classified nothing and
      an empty diff below would mean nothing: $(cat "$TMP/ctl/err")"
    if [ -s "$TMP/ctl/err" ]; then
        echo "FAIL: the control fixture drew output on stderr, so its verdict is UNKNOWN:" >&2
        sed 's/^/      /' "$TMP/ctl/err" >&2
        exit 1
    fi
    awk -F"$TAB" -v OFS=" :: " '{ $1 = $1; print }' "$TMP/ctl/raw"         | LC_ALL=C sort > "$TMP/ctl/got"
    if ! cmp -s "$TMP/ctl/expect" "$TMP/ctl/got"; then
        echo "FAIL: the extractor does not classify the control fixture as expected, so it" >&2
        echo "      cannot be trusted to see a difference and the verdict below is UNKNOWN." >&2
        echo "      < expected, > extracted:" >&2
        diff "$TMP/ctl/expect" "$TMP/ctl/got" | sed 's/^/      /' >&2 || true
        exit 1
    fi
}

# The extractor, with the caller's family program named after it when it has one, so awk's
# second BEGIN block replaces the family PREFIX.
sigdiff_awk() { # <stripped-file>
    if [ -n "$KOS_SD_FAMILY" ]; then
        awk -f "$KOS_SD_EXTRACT" -f "$KOS_SD_FAMILY" "$1"
    else
        awk -f "$KOS_SD_EXTRACT" "$1"
    fi
}

# Records for one side into $TMP/<tag>.all. An empty ref means the working tree.
sigdiff_extract_side() { # <tag> <ref-or-empty>
    _tag="$1"
    _ref="$2"
    _n=0
    : > "$TMP/$_tag.files"
    : > "$TMP/$_tag.all"
    : > "$TMP/$_tag.refused"
    if [ -n "$_ref" ]; then
        # shellcheck disable=SC2046
        git ls-tree -r --name-only -- "$_ref" $(cat "$TMP/corpus") \
            > "$TMP/$_tag.ls" || fail "git ls-tree failed for $_ref"
    else
        # `git ls-files`, not find: an untracked scratch header is neither read nor counted.
        # shellcheck disable=SC2046
        git ls-files -- $(cat "$TMP/corpus") > "$TMP/$_tag.ls" \
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
        if awk -f "$KOS_SD_STRIP" "$TMP/src" > "$TMP/stripped" 2>> "$TMP/$_tag.refused"; then
            :
        else
            _rc=$?
            [ "$_rc" -eq 2 ] || fail "strip_comments.awk exited $_rc on $_tag:$f"
            printf 'strip refused %s\n' "$f" >> "$TMP/$_tag.refused"
            continue
        fi
        if sigdiff_awk "$TMP/stripped" >> "$TMP/$_tag.all" 2>> "$TMP/$_tag.refused"; then
            :
        else
            _rc=$?
            [ "$_rc" -eq 2 ] || fail "the extractor exited $_rc on $_tag:$f"
            printf 'extract refused %s\n' "$f" >> "$TMP/$_tag.refused"
        fi
    done < "$TMP/$_tag.ls"
    if [ -s "$TMP/$_tag.refused" ]; then
        echo "FAIL: $_tag could not be scanned, so its verdict is UNKNOWN, not clean:" >&2
        sed 's/^/      /' "$TMP/$_tag.refused" >&2
        exit 1
    fi
    grep -Fxq "$KOS_SD_ANCHOR" "$TMP/$_tag.files" \
        || fail "$_tag corpus does not contain $KOS_SD_ANCHOR; the corpus was built from
      the wrong path"
    # The anchor proves ONE file was read. A family whose corpus spans several paths can lose
    # every other one and still clear it, so the count carries a floor of its own.
    if [ "$_n" -lt "$KOS_SD_MIN_FILES" ]; then
        fail "$_tag corpus is $_n file(s), below the floor of $KOS_SD_MIN_FILES. The rest of
      this family's seam was not read, and a diff over part of it is not a clean one"
    fi
    eval "KOS_SD_FILES_$_tag=$_n"
}

sigdiff_kind_count() { # <tag> <kind>
    awk -F"$TAB" -v K="$2" '$1 == K { n++ } END { print n + 0 }' "$TMP/$1.sig"
}

sigdiff_group_count() { # <tag> <name-regex>
    awk -F"$TAB" -v RE="$2" '$2 ~ RE { n++ } END { print n + 0 }' "$TMP/$1.sig"
}

sigdiff_split_side() { # <tag>
    awk -F"$TAB" '$1 != "FUNCNAMES"' "$TMP/$1.all" | LC_ALL=C sort > "$TMP/$1.sig"
    awk -F"$TAB" '$1 == "FUNCNAMES"' "$TMP/$1.all" | LC_ALL=C sort > "$TMP/$1.names"
}

# Every record classifies into exactly one group, or the table and the family PREFIX have
# drifted apart. A member the family admits and no group claims would sit outside every
# group floor, which is the hole those floors exist to close.
sigdiff_check_classified() { # <tag>
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
        echo "      script or the PREFIX in $KOS_SD_PREFIX_FILE:$_bad" >&2
        exit 1
    fi
}

# The floors. Applied to EACH side: comparing two empty record sets yields no difference,
# which is the false PASS this whole instrument exists to rule out.
sigdiff_check_floors() { # <tag>
    _t="$1"
    _bad=""
    for _k in $KOS_SD_KINDS; do
        _have="$(sigdiff_kind_count "$_t" "$_k")"
        eval "_min=\$KOS_SD_MIN_$_k"
        if [ "$_have" -lt "$_min" ]; then
            _bad="$_bad
      $KOS_SD_KIND_LABEL$_k: $_have record(s), floor $_min"
        fi
    done
    _tot="$(wc -l < "$TMP/$_t.sig" | tr -d ' ')"
    if [ "$_tot" -lt "$KOS_SD_MIN_TOTAL" ]; then
        _bad="$_bad
      total: $_tot record(s), floor $KOS_SD_MIN_TOTAL"
    fi
    while IFS="$TAB" read -r _g _re _fl; do
        _have="$(sigdiff_group_count "$_t" "$_re")"
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

sigdiff_report() {
    eval "_cand_files=\$KOS_SD_FILES_cand"
    echo "== $KOS_SD_TITLE =="
    echo "   baseline  $KOS_SD_RECORDS ($KOS_SD_BASE_N frozen record(s))"
    echo "   candidate $KOS_SD_CAND_DESC ($_cand_files seam header(s))"
    printf '   control   %s known-answer record(s), every one as expected\n' \
        "$KOS_SD_CONTROL_N"
    printf '   members   %s baseline signature record(s), %s candidate\n' \
        "$(wc -l < "$TMP/base.sig" | tr -d ' ')" "$(wc -l < "$TMP/cand.sig" | tr -d ' ')"
    _kinds=""
    for _k in $KOS_SD_REPORT_KINDS; do
        if [ -n "$_kinds" ]; then
            _kinds="$_kinds, "
        fi
        _kinds="$_kinds$_k $(sigdiff_kind_count base "$_k")/$(sigdiff_kind_count cand "$_k")"
    done
    printf '             %s\n' "$_kinds"
    while IFS="$TAB" read -r _g _re _fl; do
        printf '             group %-9s %s/%s (floor %s)\n' \
            "$_g" "$(sigdiff_group_count base "$_re")" \
            "$(sigdiff_group_count cand "$_re")" "$_fl"
    done < "$TMP/groups"
    sigdiff_family_prose
    echo
    sigdiff_rule_opening
    cat <<'KOS_RULE_END'
     a parameter TYPE, a return type, a parameter COUNT or a parameter ORDER;
     an enumeration constant's VALUE; an object-like macro's VALUE; a typedef's
     underlying type; a member added, removed or renamed; a preprocessor guard
     around a member.
   NOT reported as a signature difference:
     a parameter RENAME; the order declarations appear in a file; which seam header
     a member is declared in; comment text and whitespace; a value expression
     rewritten without changing the value it evaluates to.
   A value the extractor cannot evaluate is carried as its canonical TEXT, so an
   unevaluable expression is compared strictly rather than assumed unchanged.
KOS_RULE_END
    echo
}

sigdiff_run() { # <candidate-ref>, optional
    sigdiff_preflight
    sigdiff_resolve_candidate "${1:-}"
    scratch_dir
    sigdiff_corpus_table
    sigdiff_group_table
    sigdiff_control
    if [ -n "${KOS_SIGDIFF_REGEN:-}" ]; then
        sigdiff_extract_side cand "$KOS_SD_CAND_SHA"
        sigdiff_split_side cand
        sigdiff_check_classified cand
        sigdiff_check_floors cand
        sigdiff_regenerate
    fi
    sigdiff_read_baseline
    sigdiff_extract_side cand "$KOS_SD_CAND_SHA"
    sigdiff_split_side base
    sigdiff_split_side cand
    sigdiff_check_classified base
    sigdiff_check_classified cand
    sigdiff_check_floors base
    sigdiff_check_floors cand

    if [ -n "${KOS_SIGDIFF_KEEP:-}" ]; then
        mkdir -p "$KOS_SIGDIFF_KEEP" || fail "cannot create $KOS_SIGDIFF_KEEP"
        cp "$TMP/base.sig" "$TMP/cand.sig" "$TMP/base.names" "$TMP/cand.names" \
           "$KOS_SIGDIFF_KEEP/" || fail "cannot copy records to $KOS_SIGDIFF_KEEP"
    fi

    sigdiff_report

    _rc=0
    if cmp -s "$TMP/base.sig" "$TMP/cand.sig"; then
        echo "PASS: no signature difference"
    else
        echo "DIFF: the signature records moved. This diff IS the finding." >&2
        echo "      < baseline, > candidate" >&2
        diff "$TMP/base.sig" "$TMP/cand.sig" | sed 's/^/      /' >&2 || true
        _rc=2
    fi

    # Only when the verdict above is PASS: a rename is then the ONLY thing that moved, and
    # saying so is what keeps "not a signature change" from reading as "nothing happened".
    if [ "$_rc" -eq 0 ] && ! cmp -s "$TMP/base.names" "$TMP/cand.names"; then
        echo
        echo "NOTE: parameter NAMES differ and nothing else does. Not a signature change by the"
        echo "      rule above, so the verdict stays PASS. Shown so a rename is not silent:"
        diff "$TMP/base.names" "$TMP/cand.names" | sed 's/^/      /' || true
    fi

    exit "$_rc"
}
