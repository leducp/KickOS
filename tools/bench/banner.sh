#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sourced by bench-capture.sh, never executed. Reads the image's commit label out of a capture's
# boot banner (tests/static/check_bench_banner.sh holds it to planted logs).

# The label itself can arrive damaged, a console dropping bytes out of its row. A lone 8-hex token
# in a KickOS banner IS the commit, so it is recovered rather than reporting no banner on a
# capture that carries one.
# `-dirty` is part of the label and MUST survive: without it a capture taken from a tree with
# uncommitted edits reports as if it were taken at the commit, and the witness is unfalsifiable.
#
# THE LABEL ALONE, in the form cmake/build_stamp.cmake stamps it (`git describe --dirty
# --always`), or empty. Split out from the line it is printed on because the bench verdict
# below compares it against the tree that built the image, and must run over a planted log as
# well as over this one.
#
# THE LABEL IS A HASH ONLY WHILE NO TAG IS REACHABLE. `git describe` answers `<tag>` on a tree
# sitting on one and `<tag>-<n>-g<hash>` past it, so a recogniser keyed on the hex shape reads a
# capture that carries a banner as carrying none, and bench-capture.sh's plant machinery then
# refuses its own 'good' control and every capture with it. A tagged tree is exactly what a re-measurement
# of an archived campaign runs on.
BANNER_COMMIT_RE='commit +[A-Za-z0-9._/+-]+'

# A board under KICKOS_DIAG_TERSE prints the banner's short column (include/kickos/diag.h): a
# title row `K <version>` and a commit row `c <label>`. Each is the WHOLE line, so both are held
# to the line's start and end, and a commit row counts only after a short title: a prose row
# that lost bytes still reads `   c <hash>` and is recovered below as damaged, never taken here.
# <log> [title|commit]: the label of the last short commit row after the last short title, or
# with `title`/`commit` the line number of the last short row of that kind.
banner_terse() {
  awk -v want="${2:-label}" '
      { sub(/\r$/, "") }
      /^K [0-9]+\.[^ ]*$/ { terse = 1; title = NR; label = ""; next }
      terse && /^c [A-Za-z0-9._\/+-]+$/ { label = substr($0, 3); row = NR }
      END {
        if (want == "title" && title != "") { print title }
        if (want == "commit" && row != "") { print row }
        if (want == "label" && label != "") { print label }
      }' "$1"
}

# A commit row is read only after the last boot's title, so a boot that lost its own row is
# recovered below as damaged rather than credited with an earlier boot's label.
banner_label() { # <log> [expected label]
  _bl_from=$(grep -anE 'KickOS +[0-9]+\.' "$1" | tail -1 | cut -d: -f1)
  _bl_tt=$(banner_terse "$1" title)
  if [ -n "$_bl_tt" ] && { [ -z "$_bl_from" ] || [ "$_bl_tt" -gt "$_bl_from" ]; }; then
    _bl_from=$_bl_tt
  fi
  _bl=$(tail -n +"${_bl_from:-1}" "$1" | grep -aoE "$BANNER_COMMIT_RE" | tail -1)
  if [ -n "$_bl" ]; then
    printf '%s\n' "${_bl##* }"
    return 0
  fi
  _bl=$(banner_terse "$1")
  if [ -n "$_bl" ]; then
    printf '%s\n' "$_bl"
    return 0
  fi
  # THE SUFFIX MUST BE RECOVERED WITH THE HASH, and its absence must not read as clean.
  # Damage is byte LOSS, so a row that reached the log as "   c 06ffd64f" may have been
  # "   commit  06ffd64f-dirty" with the suffix eaten. Recovering a bare hash therefore says
  # UNVERIFIED rather than nothing.
  #
  # THE SEARCH IS HELD TO BANNER-BLOCK LINES, WHICH CARRY NO COLON. Eight hex digits is a
  # short enough shape to occur in the report proper, where `cycle counter: 84000000 Hz`
  # matches it, and a boot whose commit line was dropped WHOLE then recovers a rate and
  # reports a damaged label instead of an absent one. Every banner field is `name  value`; every app and
  # report line is `label: value`.
  _br=$(grep -av ':' "$1" \
    | grep -aoE '(^|[^0-9a-z])[0-9a-f]{8}(-dirty)?([^0-9a-z]|$)' \
    | grep -oE '[0-9a-f]{8}(-dirty)?' | tail -1)
  # A DESCRIBE LABEL CARRIES NO SHAPE TO RECOVER BY: a tag is an arbitrary string, so the only
  # thing the surviving bytes can be held against is the label the build stamped. Last, so a
  # capture of ANOTHER commit still reports as a mismatch rather than as an absence. The CR is
  # part of the last field and is taken off here; a `$` anchor would answer differently to the
  # two greps this chain runs under.
  _bw="${2:-}"
  _bw="${_bw%-dirty}"
  if [ -z "$_br" ] && [ -n "$_bw" ]; then
    _br=$(awk -v want="$_bw" '
        index($0, ":") { next }
        { _t = $NF; sub(/\r/, "", _t)
          if (_t == want || _t == want "-dirty") { hit = _t } }
        END { if (hit != "") { print hit } }' "$1")
  fi
  if [ -z "$_br" ]; then
    return 0
  fi
  case "$_br" in
    *-dirty) printf '%s\n' "$_br" ;;
    *) printf '%s-UNVERIFIED\n' "$_br" ;;
  esac
}

# The line the LAST boot begins at. The banner BLOCK is the first thing an image prints, and its
# title line carries no commit, so a console that drops the commit line whole still leaves an
# anchor the label cannot reach. Where the title went too the commit line stands in, and the
# one-per-boot counts inside the slice are what then refuse a slice spanning two boots.
bench_boot_start() { # <log>
  _bs_t=$(grep -anE 'KickOS +[0-9]+\.' "$1" | tail -1 | cut -d: -f1)
  _bs_c=$(grep -anE "$BANNER_COMMIT_RE" "$1" | tail -1 | cut -d: -f1)
  _bs_tt=$(banner_terse "$1" title)
  if [ -n "$_bs_tt" ] && { [ -z "$_bs_t" ] || [ "$_bs_tt" -gt "$_bs_t" ]; }; then
    _bs_t=$_bs_tt
  fi
  _bs_tc=$(banner_terse "$1" commit)
  if [ -n "$_bs_tc" ] && { [ -z "$_bs_c" ] || [ "$_bs_tc" -gt "$_bs_c" ]; }; then
    _bs_c=$_bs_tc
  fi
  if [ -z "$_bs_c" ]; then
    # The same banner-block restriction banner_label recovers under: a boot start found on a
    # line that label search will not read leaves the slice beginning after its own banner.
    # This one has no expected label to hold the bytes against, so it cannot reach a damaged
    # DESCRIBE label the way banner_label does; what that costs is a refusal for want of a boot
    # start where the title line went too, never a slice that reads across a boundary.
    _bs_c=$(grep -anv ':' "$1" \
      | grep -aE '(^|[^0-9a-z])[0-9a-f]{8}(-dirty)?([^0-9a-z]|$)' | tail -1 | cut -d: -f1)
  fi
  if [ -z "$_bs_t" ]; then
    printf '%s\n' "$_bs_c"
    return 0
  fi
  if [ -z "$_bs_c" ]; then
    printf '%s\n' "$_bs_t"
    return 0
  fi
  if [ "$_bs_c" -gt "$_bs_t" ]; then
    printf '%s\n' "$_bs_c"
    return 0
  fi
  printf '%s\n' "$_bs_t"
}
