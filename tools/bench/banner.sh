#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sourced by bench-capture.sh and bench.sh, never executed. Reads the image's commit label out of
# a capture's boot banner (tests/static/check_bench_banner.sh holds it to planted logs), and
# holds a USB device console's identity rows to the flashed image
# (tests/static/check_usb_identity.sh).

# The label itself can arrive damaged, a console dropping bytes out of its row. A lone 8-hex token
# in a KickOS banner IS the commit, so it is recovered rather than reporting no banner on a
# capture that carries one.
# `-dirty` is part of the label and MUST survive: without it a capture taken from a tree with
# uncommitted edits reports as if it were taken at the commit, and the witness is unfalsifiable.
#
# THE LABEL ALONE, in the form cmake/build_stamp.cmake stamps it (`git describe --dirty
# --always`), or empty.
#
# THE LABEL IS A HASH ONLY WHILE NO TAG IS REACHABLE. `git describe` answers `<tag>` on a tree
# sitting on one and `<tag>-<n>-g<hash>` past it, so a recogniser keyed on the hex shape reads a
# capture that carries a banner as carrying none.
BANNER_COMMIT_RE='commit +[A-Za-z0-9._/+-]+'

# A banner title is the name and its version, `unknown` where the build had no version tag,
# whatever spacing or rule surrounds them. The version is the second group.
BANNER_TITLE_ERE='(^|[^[:alnum:]_])KickOS[[:space:]]+(unknown|[0-9]+[.][0-9]+[.][0-9]+[-+.A-Za-z0-9]*)([[:space:]=]|$)'

# banner_title_version <line>: the version a title line carries, or nothing.
banner_title_version() {
  printf '%s\n' "$1" | sed -nE "s/.*$BANNER_TITLE_ERE.*/\\2/p" | head -n 1
}

# A commit row is read only after the last boot's title, so a boot that lost its own row is
# recovered below as damaged rather than credited with an earlier boot's label.
banner_label() { # <log> [expected label]
  _bl_from=$(grep -anE "$BANNER_TITLE_ERE" "$1" | tail -1 | cut -d: -f1)
  _bl=$(tail -n +"${_bl_from:-1}" "$1" | grep -aoE "$BANNER_COMMIT_RE" | tail -1)
  if [ -n "$_bl" ]; then
    printf '%s\n' "${_bl##* }"
    return 0
  fi
  # THE SUFFIX MUST BE RECOVERED WITH THE HASH, and its absence must not read as clean.
  # Damage is byte LOSS, so a row that reached the log as "   c 06ffd64f" may have been
  # "   commit  06ffd64f-dirty" with the suffix eaten. Recovering a bare hash therefore says
  # UNVERIFIED rather than nothing.
  #
  # THE SEARCH IS HELD TO BANNER-BLOCK LINES, WHICH CARRY NO COLON. Eight hex digits is a
  # short enough shape to occur in the report proper, where `cpu clock: 84000000 Hz`
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

# The row <macro> of include/kickos/diag.h <diag> prints with <value>, without its newline.
identity_row() { # <diag> <macro> <value>
  _ir_def=$(grep -E "^#define $2 +\".*\"$" "$1")
  [ -n "$_ir_def" ] || return 1
  _ir_fmt=$(sed -E "s/^#define $2 +\"(.*)\"$/\\1/" <<< "$_ir_def")
  # shellcheck disable=SC2059
  printf "$_ir_fmt" "$3"
}

# The app stamp the identity rows print: of an ELF of several images, the one in the image that
# links kickos::banner_identity, whose size_t mangles per target. Returns 1 where that image
# defines none, and 2, printing why, where a stamp is defined but no image is known to print the
# rows.
identity_app_stamp() { # <elf>
  local _ias_at='^_ZN6kickos15banner_identityEPc[jm]$'
  image_string "$1" kickos_app_stamp "$_ias_at" && return 0
  elf_image_prefix "$1" "$_ias_at" kickos_app_stamp > /dev/null
  case $? in
    1)
      if elf_symbol_at "$1" kickos_app_stamp > /dev/null; then
        echo "defines an app stamp but no banner_identity symbol, which an LTO link may have \
inlined away, so no image is known to print the rows"
        return 2
      fi
      ;;
    2)
      echo "links banner_identity in no image, while the app stamp sits in one or is defined \
more than once"
      return 2
      ;;
  esac
  return 1
}

# A USB device console's capture: its last identity block (kickos/sys/banner_identity.h), the
# banner's title, board, build, app and commit rows a host's configuration opens the stream with,
# names the flashed image, so it stands in for the kernel banner that console never carries. The
# rows are rendered here from kbanner's own formats; an empty <app> is an image with no app stamp,
# whose block has no app row. Prints the refusal and returns 1 otherwise.
identity_verdict() { # <log> <diag> <version> <board> <label> <build> <app>
  _iv_b=$(identity_row "$2" KDIAG_F_BANNER_BOARD "$4") || { echo "no board format in $2"; return 1; }
  _iv_u=$(identity_row "$2" KDIAG_F_BANNER_BUILD "$6") || { echo "no build format in $2"; return 1; }
  _iv_a=""
  if [ -n "$7" ]; then
    _iv_a=$(identity_row "$2" KDIAG_F_BANNER_APP "$7") || { echo "no app format in $2"; return 1; }
  fi
  _iv_c=$(identity_row "$2" KDIAG_F_BANNER_COMMIT "$5") || { echo "no commit format in $2"; return 1; }
  # The last title of any version, so a stale image's block is the one judged.
  _iv_at=$(tr -d '\r' < "$1" | grep -nE "$BANNER_TITLE_ERE" | tail -n 1 | cut -d: -f1)
  if [ -z "$_iv_at" ]; then
    echo "no identity rows: the image on the board printed none once the host configured it, so the capture names no build"
    return 1
  fi
  _iv_block=$(tr -d '\r' < "$1" | sed -n "${_iv_at},$((_iv_at + 4))p")
  _iv_v=$(banner_title_version "$(printf '%s\n' "$_iv_block" | sed -n 1p)")
  if [ "$_iv_v" != "$3" ]; then
    echo "identity row 1 of the last block names version [$_iv_v], not the flashed image's [$3]"
    return 1
  fi
  _iv_n=1
  for _iv_row in "$_iv_b" "$_iv_u" "$_iv_a" "$_iv_c"; do
    if [ -z "$_iv_row" ]; then
      continue
    fi
    _iv_n=$((_iv_n + 1))
    _iv_got=$(printf '%s\n' "$_iv_block" | sed -n "${_iv_n}p")
    if [ "$_iv_got" != "$_iv_row" ]; then
      echo "identity row $_iv_n of the last block reads [$_iv_got], not the flashed image's [$_iv_row]"
      return 1
    fi
  done
  return 0
}
