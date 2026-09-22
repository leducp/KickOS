#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The byte rules and the whitespace rules of docs/reference/style.md, over the same walk of
# every tracked file:
#
#   the byte    no tracked file holds a byte above 0x7F, and none holds a NUL.
#
#   the line    no trailing whitespace, no CRLF, a final newline, no space immediately before
#               a tab in a line's indent, and no blank line at end of file. The last two are
#               `git diff --check` classes the style page does not name and an audit found in
#               a linker script.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_ascii.sh
#
#   corpus   EVERY tracked file, from `git ls-files`, with no extension filter of any kind,
#            so the next .svg, .rst, .ld or .csv is covered on the day it lands.
#
#   floor    CORPUS_FLOOR, at about half what the tree tracks, applied to the tracked total
#            and to each of the two read counts, because a gate handed nothing must go red
#            rather than clean.
#
#   tally    every tracked file reaches a verdict on BOTH halves, and the two sums are
#            checked against the tracked total, so a walk that ended early is a red refusal
#            and not a smaller headline figure. A file counts towards a half only once the
#            detector for that half has RETURNED SUCCESS: a reader that died writes the same
#            empty record file as a clean one, so an unread file would otherwise raise the
#            tally and the headline figure while asserting nothing.
#
#   control  a planted file per class, scanned by the same detector in the same form the
#            corpus loop runs it, before the corpus is read: the byte class and the byte
#            reader in both directions plus a file it cannot read, the NUL test in both
#            directions, five line classes plus a clean file plus a file it cannot read, and
#            two for the record filter.
#
# THE TWO EXEMPTION MECHANISMS ARE NOT ONE, and collapsing them would move coverage either
# way. exempt_bytes() names a file whose BYTES are not ours, and that file is still read for
# every line class. allowed_records() names the individual records a file may carry, and
# every other class in that same file still reports. So no file is skipped whole by either.
#
#   binary   a tracked file holding a NUL byte is not text. "ASCII only" states nothing about
#            it, so the byte half REFUSES it by name rather than skipping it, and still
#            scans it: the reader maps NUL to an ASCII control byte first, so no byte report
#            is suppressed and no tool outside POSIX is asked for. The line half SKIPS it by
#            name, awk's records over a NUL stream meaning nothing.
#            A file that is both NUL-holding and byte-exempt used to be refused by neither.
#            exempt_nul_files() closes that: it is the one place a NUL is EXPECTED inside a
#            byte-exempt file, and a byte-exempt file holding a NUL that is not named there is
#            refused by name instead of passing through the crack the two skips left between
#            them.
#
# The byte rule is about the BYTE, so its verdict covers bytes above 0x7F and NUL and nothing
# else: an ASCII spelling that is merely WRONG passes (an HTML entity for a dash, `(c)` where
# a real copyright sign was meant), and a control character below 0x20 is ASCII, belonging to
# the line rules below it.
#
# NO FILE NAME REACHES A TOOL AS AN OPERAND. Every scan takes its file on stdin. `awk` reads
# an operand of the form `name=value` as a VARIABLE ASSIGNMENT and then falls through to
# stdin, which here is the file list the corpus loop is reading, so one tracked file named
# like an assignment consumed the rest of the list and every file below it went unread while
# the run printed PASS. The name is prefixed onto each record by hand instead, so a one-file
# invocation reports in the same shape as the sweep.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

require_repo_root

# Sized at about HALF what the tree tracks, so an ordinary deletion still passes while an
# empty walk, a run outside a checkout and a corpus narrowed by an edit here all refuse. The
# tracked total is printed on every run and is not pinned here.
CORPUS_FLOOR=512

# The files whose BYTES are exempt, NAMED ONE BY ONE and each still asserted present below.
# A pattern here would be open-ended in the one direction that matters: a file added later
# whose name happened to fit it would carry any byte it liked, and a NUL in it would clear
# the line half too, so the tree would gain an unreadable file no half of this gate reports.
# Their lines are still scanned.
#
#   LICENSE
#       verbatim upstream CeCILL-C text, which carries a-grave (C3 A0) twice, in
#       "Commissariat a l'Energie Atomique" and in "vis-a-vis": it cannot be ASCII and stay
#       verbatim. Restore it from cecill.info, never by hand-transliterating.
#
#   the docs/archive/*_meas.md captures
#       archived RAW INSTRUMENT CAPTURES. A measurement is not regenerable, so rewriting a
#       byte inside one falsifies it, NULs off the wire included. Named individually, not by
#       the suffix: docs/archive/ also holds ordinary prose (docs/archive/M1_state.md), and a
#       capture archived tomorrow is a decision to take then rather than one taken here now.
exempt_byte_files() {
    printf '%s\n' \
        'LICENSE' \
        'docs/archive/M1_raw_meas.md' \
        'docs/archive/M3_raw_meas.md' \
        'docs/archive/M4.5_footprint_meas.md' \
        'docs/archive/M4.7-M4.8.1_fleet_selftest_meas.md' \
        'docs/archive/M4.7.9_footprint_meas.md' \
        'docs/archive/M4.7.9_teardown_latency_meas.md'
}

exempt_bytes() { # <file>
    grep -Fxq -e "$1" "$TMP/exempt_names"
    _eb=$?
    case "$_eb" in
        0) return 0 ;;
        1) return 1 ;;
    esac
    fail "exit $_eb from grep while asking whether $1 is byte-exempt: the answer is UNKNOWN,
      and either way round it decides whether this file's bytes are read at all"
}

# The byte-exempt files that are ALSO expected to hold a NUL. Named individually, same
# reasoning as exempt_byte_files() above: a pattern here would let a future exempt file carry
# a NUL unremarked. Of the seven names in exempt_byte_files(), only this one measures NUL-holding
# on this tree; the check below refuses if that ever disagrees.
#
#   docs/archive/M1_raw_meas.md
#       archived raw instrument capture; its NULs are off the wire, same reason it is
#       byte-exempt at all.
exempt_nul_files() {
    printf '%s\n' \
        'docs/archive/M1_raw_meas.md'
}

exempt_nul() { # <file>
    grep -Fxq -e "$1" "$TMP/exempt_nul_names"
    _en=$?
    case "$_en" in
        0) return 0 ;;
        1) return 1 ;;
    esac
    fail "exit $_en from grep while asking whether $1 is NUL-exempt: the answer is UNKNOWN,
      and either way round it decides whether a byte-exempt file's NUL is acknowledged or
      refused"
}

# The line records a tracked file is allowed to carry, one per line, in the shape scan_file
# emits. Every listed record must still be PRESENT: a file that stopped carrying one fails
# here, so the set cannot go stale into a blanket skip. Every record a file carries that is
# not listed reports as an ordinary finding.
#
#   LICENSE
#       verbatim upstream CeCILL-C text. Lines 133 and 279 carry a trailing space; rewriting
#       them makes the file no longer the licence as published. Restore it from cecill.info,
#       never by hand.
allowed_records() { # <file>
    case "$1" in
        LICENSE)
            printf '%s\n' '133: trailing whitespace' '279: trailing whitespace'
            ;;
    esac
}

scratch_dir

exempt_byte_files > "$TMP/exempt_names"
require_nonempty "$TMP/exempt_names" "exempt_byte_files() names nothing, so the controls
      below would assert nothing about the exemption"

exempt_nul_files > "$TMP/exempt_nul_names"
require_nonempty "$TMP/exempt_nul_names" "exempt_nul_files() names nothing, so the controls
      below would assert nothing about the NUL exemption"
# A name here that is not also byte-exempt declares an exemption for a check that never
# reaches it: the main loop below only asks exempt_nul() about a file exempt_bytes() already
# passed.
while IFS= read -r _xn; do
    grep -Fxq -e "$_xn" "$TMP/exempt_names" \
        || fail "exempt_nul_files() names [$_xn], which exempt_byte_files() does not: a NUL
      exemption on a file whose bytes are not exempt asserts nothing, since the byte half
      would refuse that file on its own bytes first"
done < "$TMP/exempt_nul_names"

# EVERY DETECTOR BELOW WRITES ITS RECORDS TO A FILE AND RETURNS A STATUS. A detector that
# died produces no record, which is what a clean file produces too, so a caller reading only
# the records reports an unread file as clean.

# One record per finding, `<line>: <class>`, for every class a line can carry.
#
# A record is judged on the line with any trailing carriage return REMOVED, so a CRLF file
# reports the CR once and does not also report every line as trailing whitespace.
scan_lines() { # <file> <records>
    awk '
        BEGIN { CR = sprintf("%c", 13) }
        {
            line = $0
            if (substr(line, length(line), 1) == CR) {
                print NR ": carriage return at end of line"
                line = substr(line, 1, length(line) - 1)
            }
            if (line ~ /[ \t]$/) { print NR ": trailing whitespace" }
            indent = line
            sub(/[^ \t].*$/, "", indent)
            if (indent ~ / \t/) { print NR ": space before tab in indent" }
            empty = (line == "")
            last = NR
        }
        END {
            if (last > 0 && empty) { print last ": blank line at end of file" }
        }' < "$1" > "$2"
}

# The newline awk cannot see: it splits on records, so a file whose last byte is not a
# newline yields the same records as one whose last byte is.
scan_eof() { # <file> <records>
    : > "$2"
    tail -c 1 < "$1" > "$TMP/lastbyte" || return 1
    wc -l < "$TMP/lastbyte" > "$TMP/lastcount" || return 1
    read -r _se < "$TMP/lastcount" || return 1
    if [ "$_se" -eq 0 ]; then
        printf '%s\n' '$: no newline at end of file' > "$2"
    fi
    return 0
}

scan_file() { # <file> <records>
    scan_lines "$1" "$2" || return 1
    scan_eof "$1" "$TMP/eofrec" || return 1
    cat "$TMP/eofrec" >> "$2" || return 1
    return 0
}

# The byte reader, in the one form the corpus loop runs.
#
# `grep -a` IS NOT POSIX, and it used to sit on the left of a pipeline whose exit status is
# the right-hand tool's: a grep that rejects the option makes the whole byte half empty and
# nothing downstream can tell that from a tree with no high byte. The option was only ever
# there so a NUL did not turn the file "binary" and cost the matches, so the NUL is mapped to
# an ASCII control byte instead, which cannot itself be a high-byte finding and leaves the
# line numbering alone, NUL being no record separator.
scan_bytes() { # <file> <records>
    LC_ALL=C tr '\000' '\001' < "$1" > "$TMP/nulfree" || return 1
    LC_ALL=C grep -n "$HIGH" < "$TMP/nulfree" > "$2"
    _sb=$?
    # 1 is "no line matched", the ordinary case here; anything above it is an error and the
    # file's bytes are then UNKNOWN, not clean.
    [ "$_sb" -le 1 ] || return 1
    return 0
}

# grep cannot carry a NUL in its pattern, so an unequal `tr -d` compare is the test. One
# spelling, read by both halves, rather than a byte count in one and a compare in the other.
#
# THE COMPARE HAS THREE ANSWERS AND ONLY TWO OF THEM ARE A VERDICT: equal, different, and a
# compare that did not happen. Reading anything but equal as "holds a NUL" sends a file the
# tool merely failed on out of the line half, which lowers the scanned count, leaves the two
# sums balancing and passes.
nul_verdict() { # <cmp-status> <file>; 0 holds one, 1 does not
    case "$1" in
        0) return 1 ;;
        1) return 0 ;;
    esac
    fail "exit $1 from cmp while testing $2 for a NUL byte: its verdict is UNKNOWN, not
      binary, and skipping it here leaves the line half asserting nothing about it"
}

holds_nul() { # <file>
    LC_ALL=C tr -d '\000' < "$1" > "$TMP/nulstripped" \
        || fail "cannot read $1 to test it for a NUL byte, so its verdict is UNKNOWN, not clean"
    cmp -s "$TMP/nulstripped" "$1"
    _hn=$?
    nul_verdict "$_hn" "$1"
}

# Drops the records allowed_records() names for <file> from the record file <records>, and
# refuses when one of them is no longer there: an entry that stopped matching would hide
# whatever took its place.
apply_allowed() { # <file> <records>
    allowed_records "$1" > "$TMP/allow"
    while IFS= read -r _a; do
        grep -Fxq -e "$_a" "$2" \
            || fail "$1 no longer carries the record [$_a] that allowed_records() names, so
      that entry hides whatever replaced it. Re-measure the file and update the list."
    done < "$TMP/allow"
    # grep exits 1 when nothing survives the filter, which is the ordinary case here; only
    # an exit above that is an error, and taking it for "nothing left" would empty the record
    # set and report the file clean.
    grep -Fxv -f "$TMP/allow" "$2" > "$TMP/kept"
    _rc=$?
    [ "$_rc" -le 1 ] || fail "exit $_rc from grep while filtering the records of $1: its
      verdict is UNKNOWN, not clean"
    mv "$TMP/kept" "$2"
}

# --- the byte detector, before it is asked to report an absence ---------------
# Built with printf: a literal 0x80..0xFF range cannot be typed into this file, which is
# itself part of the corpus below. Proven both ways every run, because a shell that left
# the escapes unexpanded and a locale other than C each break the range silently.
HIGH="$(printf '[\200-\377]')"
printf 'caf\351\n' | LC_ALL=C grep -q "$HIGH" \
    || fail "the high-byte class matches no high byte; the scan below would report clean on anything"
if printf 'a plain ASCII line\n' | LC_ALL=C grep -q "$HIGH"; then
    fail "the high-byte class matches pure ASCII; every file in the tree would report"
fi

mkdir "$TMP/ctl" || fail "cannot create $TMP/ctl"

# The NUL test is what routes a file away from the line half, so a test that answered no to
# everything would send a binary file through awk and report its records as findings.
printf 'a line\000with a NUL in it\n' > "$TMP/ctl/nul"
printf 'a line with no NUL in it\n'   > "$TMP/ctl/nonul"
holds_nul "$TMP/ctl/nul" \
    || fail "the NUL test does not see a NUL, so a binary file would be read as text and
      awk's records over it reported as line findings"
if holds_nul "$TMP/ctl/nonul"; then
    fail "the NUL test sees a NUL in a file that holds none, so every file would be refused
      by the byte half and skipped by the line half"
fi

# The compare's three answers, driven by hand: a planted file reaches only the two that are
# a verdict, and it is the third, a compare that did not happen, that used to pass for
# "binary" and take the file quietly out of the line half.
if nul_verdict 0 "$TMP/ctl/nonul"; then
    fail "an equal compare reads as a NUL, so every file would be refused by the byte half
      and skipped by the line half"
fi
nul_verdict 1 "$TMP/ctl/nul" \
    || fail "an unequal compare reads as no NUL, so a binary file would be read as text and
      awk's records over it reported as line findings"
if ( nul_verdict 2 "$TMP/ctl/nul" ) >/dev/null 2>&1; then
    fail "a compare that did not happen reads as a verdict, so a file the tool merely failed
      on leaves the line half, lowers the scanned count and the run still passes"
fi

# THE CLASS IS NOT THE READER. The two matches above say the range is built; they say
# nothing about the reader the corpus loop actually calls, its options included, and a reader
# a tool rejects writes the same empty record file as a tree with no high byte in it. So the
# reader is run here in exactly that form, over a planted file that also holds a NUL, since
# that is the case the options exist for. The third run is a file it cannot read: a detector
# whose failure passes for silence is a detector that reports every unread file clean.
printf 'a caf\351 line\000and a NUL in it\n' > "$TMP/ctl/high"
printf 'a plain ASCII line\n'                 > "$TMP/ctl/nohigh"

scan_bytes "$TMP/ctl/high" "$TMP/ctl/brec" \
    || fail "the byte reader failed on a planted file, so every byte of the tree below would
      be UNREAD and the sweep would report it clean"
case "$(cat "$TMP/ctl/brec")" in
    1:*) ;;
    *) fail "the byte reader reported no line 1 for a planted high byte beside a NUL, so a
      byte in a file holding one would go unreported" ;;
esac

scan_bytes "$TMP/ctl/nohigh" "$TMP/ctl/brec" \
    || fail "the byte reader failed on a pure ASCII file, so its verdict on a clean tree is
      an error rather than silence"
[ ! -s "$TMP/ctl/brec" ] \
    || fail "the byte reader reported on a pure ASCII file, so every file in the tree would
      report"

if scan_bytes "$TMP/ctl/absent" "$TMP/ctl/brec" 2>/dev/null; then
    fail "the byte reader answered success for a file it could not read, so a stage that died
      would be tallied as a file scanned clean"
fi

echo "== control: the high-byte class, the byte reader and the NUL test each fire on a planted file, none on a clean one, and a reader that cannot read refuses =="

# --- the line detector, before it is asked to report an absence ---------------
# One planted file per class and one clean file, each scanned by scan_file above. A class
# whose control does not fire is a class this run cannot report on.
printf 'a line ending in a space \n'                > "$TMP/ctl/trail"
printf ' %sindented after a space\n' "$TAB"      > "$TMP/ctl/sbt"
printf 'a line ending in a carriage return\r\n'     > "$TMP/ctl/crlf"
printf 'a line\n\n'                                 > "$TMP/ctl/blank"
printf 'a line with no newline after it'            > "$TMP/ctl/nonl"
printf 'a clean line\n%sa clean indent\n' "$TAB"   > "$TMP/ctl/clean"

control() { # <name> <expected-record>
    scan_file "$TMP/ctl/$1" "$TMP/ctl/lrec" \
        || fail "the detector failed on the planted file for '$2', so the sweep below would
      report the tree clean of that class"
    _got="$(cat "$TMP/ctl/lrec")"
    if [ "$_got" != "$2" ]; then
        fail "the control for '$2' reported [$_got] instead: this detector cannot see that
      class, so the sweep below would report the tree clean of it"
    fi
}

control trail "1: trailing whitespace"
control sbt   "1: space before tab in indent"
control crlf  "1: carriage return at end of line"
control blank "2: blank line at end of file"
control nonl  "\$: no newline at end of file"

scan_file "$TMP/ctl/clean" "$TMP/ctl/lrec" \
    || fail "the detector failed on a file carrying none of these classes, so its verdict on
      a clean tree is an error rather than silence"
CLEAN_HITS="$(cat "$TMP/ctl/lrec")"
[ -z "$CLEAN_HITS" ] \
    || fail "the detector reported [$CLEAN_HITS] on a file that carries none of these
      classes, so every file in the tree would report"

if scan_file "$TMP/ctl/absent" "$TMP/ctl/lrec" 2>/dev/null; then
    fail "the line detector answered success for a file it could not read, so a stage that
      died would be tallied as a file scanned clean"
fi

echo "== control: all five line classes fire on a planted file, none on a clean one, and a detector that cannot read refuses =="

# --- the filter, before it is asked to hide anything --------------------------
# The suppression is a detector too, so it gets the same treatment: it must pass an unlisted
# record through, and it must refuse a listed one that is no longer in the file.
allowed_records LICENSE > "$TMP/ctl/allow"
require_nonempty "$TMP/ctl/allow" "allowed_records() names nothing for LICENSE, so the two
      controls below would assert nothing about the filter"

{ cat "$TMP/ctl/allow"; printf '%s\n' '7: carriage return at end of line'; } > "$TMP/ctl/rec"
apply_allowed LICENSE "$TMP/ctl/rec"
[ "$(cat "$TMP/ctl/rec")" = "7: carriage return at end of line" ] \
    || fail "the filter reported [$(cat "$TMP/ctl/rec")] where only the unlisted record was
      expected, so it drops records it was never given"

sed -n '2,$p' "$TMP/ctl/allow" > "$TMP/ctl/rec"
if ( apply_allowed LICENSE "$TMP/ctl/rec" ) >/dev/null 2>&1; then
    fail "the filter accepted a file missing a record allowed_records() names, so a stale
      entry would silently hide the record that replaced it"
fi

# The byte exemption is a filter too. The negative control is a name shaped like the
# capture suffix the list used to be written as, so a pattern put back here fails here.
exempt_bytes LICENSE \
    || fail "the byte exemption does not fire on a file it names, so LICENSE reports its own
      verbatim text as a finding"
if exempt_bytes "docs/archive/NOT_A_CAPTURE_meas.md"; then
    fail "the byte exemption fires on a file it does not name, so a file added later could
      carry any byte at all and a NUL in it would clear the line half too"
fi

# The NUL exemption is the filter that closes the crack between the two skips above: a
# byte-exempt file's NUL either matches a name here or the walk below refuses it by name.
exempt_nul "docs/archive/M1_raw_meas.md" \
    || fail "the NUL exemption does not fire on the one file it names, so the walk below
      would refuse a real, legitimate NUL-holding capture"
if exempt_nul "docs/archive/M3_raw_meas.md"; then
    fail "the NUL exemption fires on a file it does not name, so a byte-exempt file could gain
      an unacknowledged NUL and still pass"
fi

echo "== control: the filter passes an unlisted record and refuses a listed one gone missing, the byte exemption fires only on a file it names, and the NUL exemption fires only on a file it names =="

# --- the corpus ---------------------------------------------------------------
corpus_all "$TMP/all"

N_TRACKED="$(wc -l < "$TMP/all" | tr -d ' ')"
[ "$N_TRACKED" -ge "$CORPUS_FLOOR" ] \
    || fail "$N_TRACKED tracked file(s), beneath the floor of $CORPUS_FLOOR: this is not the
      KickOS worktree, or the corpus was narrowed. A corpus that size asserts nothing."

# Every exempted name must still be tracked, the way allowed_records() entries must still be
# present: a name kept past the file's deletion or rename is a hole waiting for the next file
# to take that path.
while IFS= read -r _x; do
    grep -Fxq -e "$_x" "$TMP/all" \
        || fail "exempt_byte_files() names [$_x], which the tree no longer tracks, so that
      entry exempts whatever lands on that path next. Drop it from the list."
done < "$TMP/exempt_names"

# Every declared NUL exemption must still HOLD a NUL: a stale entry here is the mirror of the
# one above, an acknowledgment that no longer matches the file and would sit there asserting
# nothing while a real, unacknowledged NUL elsewhere is what this exemption exists to catch.
while IFS= read -r _xn; do
    holds_nul "$_xn" \
        || fail "exempt_nul_files() names [$_xn], which does not hold a NUL: a stale
      acknowledgment here is not evidence of anything. Drop it from the list or re-measure."
done < "$TMP/exempt_nul_names"

: > "$TMP/bytes"
: > "$TMP/lines"
: > "$TMP/binary"
: > "$TMP/notext"
: > "$TMP/empty"
: > "$TMP/exempt"
: > "$TMP/exempt_nul"
: > "$TMP/unacked_nul"
: > "$TMP/allowed"
N_BYTE=0
N_LINE=0
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    [ -r "$f" ] || fail "tracked file is unreadable, so its verdict is UNKNOWN, not clean: $f"
    # An empty file carries no byte, no line and no missing final newline, so it reaches a
    # verdict on both halves at once.
    if [ ! -s "$f" ]; then
        printf '%s\n' "$f" >> "$TMP/empty"
        continue
    fi
    if holds_nul "$f"; then
        _nul=1
    else
        _nul=0
    fi

    if exempt_bytes "$f"; then
        printf '%s\n' "$f" >> "$TMP/exempt"
        # A byte-exempt file's bytes are never scanned, and a NUL routes it out of the line
        # half two lines below, so without this check the combination is refused by neither:
        # exempt_nul_files() is the one place that NUL is expected, and anywhere else it is a
        # finding by name.
        if [ "$_nul" -eq 1 ]; then
            if exempt_nul "$f"; then
                printf '%s\n' "$f" >> "$TMP/exempt_nul"
            else
                printf '%s\n' "$f" >> "$TMP/unacked_nul"
            fi
        fi
    else
        # The tally counts a SUCCESSFUL read, never a loop entry: a reader that died leaves
        # the same empty record file as a clean one, and would otherwise raise the headline
        # figure for a file this run never looked at.
        scan_bytes "$f" "$TMP/brec" \
            || fail "the byte reader failed on $f, so its bytes are UNREAD and not clean"
        N_BYTE=$((N_BYTE + 1))
        if [ "$_nul" -eq 1 ]; then
            printf '%s\n' "$f" >> "$TMP/binary"
        fi
        while IFS= read -r b; do
            printf '%s:%s\n' "$f" "$b"
        done < "$TMP/brec" >> "$TMP/bytes"
    fi

    if [ "$_nul" -eq 1 ]; then
        printf '%s\n' "$f" >> "$TMP/notext"
        continue
    fi
    scan_file "$f" "$TMP/rec" \
        || fail "the line detector failed on $f, so its lines are UNREAD and not clean"
    N_LINE=$((N_LINE + 1))
    if [ -n "$(allowed_records "$f")" ]; then
        printf '%s\n' "$f" >> "$TMP/allowed"
        apply_allowed "$f" "$TMP/rec"
    fi
    while IFS= read -r r; do
        printf '%s:%s\n' "$f" "$r"
    done < "$TMP/rec" >> "$TMP/lines"
done < "$TMP/all"

N_EMPTY="$(wc -l < "$TMP/empty" | tr -d ' ')"
N_NOTEXT="$(wc -l < "$TMP/notext" | tr -d ' ')"
N_EXEMPT="$(wc -l < "$TMP/exempt" | tr -d ' ')"

echo "== checked $N_BYTE of $N_TRACKED tracked file(s), every byte of each =="
echo "== checked $N_LINE of $N_TRACKED tracked file(s), every line of each =="

[ "$N_BYTE" -ge "$CORPUS_FLOOR" ] \
    || fail "$N_BYTE file(s) read for bytes, beneath the floor of $CORPUS_FLOOR: the walk did
      not reach the tree, and a byte this run never looked at is unread rather than absent."
[ "$N_LINE" -ge "$CORPUS_FLOOR" ] \
    || fail "$N_LINE file(s) read for lines, beneath the floor of $CORPUS_FLOOR: the walk did
      not reach the tree, and a class this run never looked for is unread rather than absent."

# Each half separately, so a file counted in one bucket and dropped from the other cannot be
# hidden by a sum that happens to come out right.
[ $((N_BYTE + N_EXEMPT + N_EMPTY)) -eq "$N_TRACKED" ] \
    || fail "$((N_BYTE + N_EXEMPT + N_EMPTY)) of $N_TRACKED tracked file(s) reached a verdict
      on the byte half ($N_BYTE read, $N_EXEMPT exempt, $N_EMPTY empty): the walk ended before
      the list did, so the remainder is unread and this run asserts nothing about it."
[ $((N_LINE + N_NOTEXT + N_EMPTY)) -eq "$N_TRACKED" ] \
    || fail "$((N_LINE + N_NOTEXT + N_EMPTY)) of $N_TRACKED tracked file(s) reached a verdict
      on the line half ($N_LINE read, $N_NOTEXT holding a NUL, $N_EMPTY empty): the walk ended
      before the list did, so the remainder is unread and this run asserts nothing about it."

if [ -s "$TMP/exempt" ]; then
    echo "== lines scanned, bytes exempt by name (see exempt_bytes() for the reason each carries) =="
    sed 's/^/   /' "$TMP/exempt"
fi
if [ -s "$TMP/allowed" ]; then
    echo "== scanned, with the records allowed_records() names dropped and asserted present =="
    sed 's/^/   /' "$TMP/allowed"
fi
if [ -s "$TMP/empty" ]; then
    echo "== not scanned, an empty file carries no byte, no line and no missing final newline =="
    sed 's/^/   /' "$TMP/empty"
fi
if [ -s "$TMP/notext" ]; then
    echo "== lines not scanned, a NUL byte makes it not text (the byte half refuses one it scans) =="
    sed 's/^/   /' "$TMP/notext"
fi
if [ -s "$TMP/exempt_nul" ]; then
    echo "== byte-exempt AND holds a NUL, acknowledged by name (see exempt_nul_files()) =="
    sed 's/^/   /' "$TMP/exempt_nul"
fi

RC=0

if [ -s "$TMP/bytes" ]; then
    # cat -v, or the report re-emits the bytes it complains about and the terminal renders
    # them as whatever looked fine to whoever committed them.
    cat -v "$TMP/bytes" >&2
    echo "" >&2
    echo "per-file finding count:" >&2
    cut -d: -f1 "$TMP/bytes" | sort | uniq -c | sort -rn >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/bytes" | tr -d ' ') line(s) hold a byte above 0x7F." >&2
    echo "      Spell it in ASCII: a comma or a single - for an em dash, -> for an arrow," >&2
    echo "      straight quotes, \"section\" for a section sign. Never a double hyphen:" >&2
    echo "      check_dash_punct.sh refuses that. The M-x pairs above are how cat -v shows the byte." >&2
    RC=1
fi

if [ -s "$TMP/binary" ]; then
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/binary" | tr -d ' ') tracked file(s) hold a NUL byte, so they are not text" >&2
    echo "      and this rule says nothing about them. Their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/binary" >&2
    echo "      Drop the file, strip the NULs, or classify it in this script with the reason" >&2
    echo "      the tree tracks a non-text file." >&2
    RC=1
fi

if [ -s "$TMP/unacked_nul" ]; then
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/unacked_nul" | tr -d ' ') byte-exempt tracked file(s) hold a NUL" >&2
    echo "      that exempt_nul_files() does not name, so neither half of this gate refuses" >&2
    echo "      them: the byte half skips them by exempt_bytes(), and the line half skips any" >&2
    echo "      NUL-holding file. Their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/unacked_nul" >&2
    echo "      Add the file to exempt_nul_files() if the NUL is a legitimate raw-capture byte," >&2
    echo "      or strip the NUL if it is not." >&2
    RC=1
fi

if [ -s "$TMP/lines" ]; then
    cat "$TMP/lines" >&2
    echo "" >&2
    echo "per-class finding count:" >&2
    sed 's/^.*: //' "$TMP/lines" | sort | uniq -c | sort -rn >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/lines" | tr -d ' ') whitespace finding(s)." >&2
    echo "      No trailing whitespace, no CRLF, and a final newline on every tracked file." >&2
    echo "      A space before a tab renders at the reader's tab stop and not at yours, and" >&2
    echo "      a blank line before end of file is what \`git diff --check\` calls one." >&2
    echo "      A \$ in place of a line number is the end of the file rather than a line." >&2
    RC=1
fi

[ "$RC" -eq 0 ] || exit 1

echo "PASS: no tracked file holds a byte above 0x7F, none holds a NUL, and none carries"
echo "      trailing whitespace, a CRLF, a space before a tab in its indent, a blank line"
echo "      at end of file, or a missing final newline"
