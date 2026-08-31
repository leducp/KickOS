#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The CAPABILITY ABI's signature diff, frozen before the first object kind lands and
# measured against that baseline after. M6.5's expected result is a NEGATIVE one: frame and
# page-table objects enter the capability layer and the layer stays ADDRESS-SPACE
# AGNOSTIC, which is the property the spike's QW-5 asked be preserved and the one thing
# M6.5 could plausibly break. Section 3.4b's rule applies unchanged: an empty signature
# diff needs a baseline, so the baseline is taken BEFORE the first object kind lands, not
# after.
#
#   tests/static/check_cap_sigdiff.sh [<candidate-ref>]
#
# tests/lib/signature_diff.sh drives the comparison and documents the corpus, the record
# kinds, the exit codes, KOS_SIGDIFF_KEEP and KOS_SIGDIFF_REGEN. This file declares the CAP
# family: its corpus, its identifier prefixes, its floors, its baseline records and the prose
# of its report.
#
# THE EXTRACTOR IS tests/static/aspace_seam.awk VERBATIM, with
# tests/static/cap_seam_family.awk named after it so its BEGIN block replaces the family
# PREFIX and nothing else.
#
# EXPECTED VERDICT: PASS, throughout M6.5. This is the entry differ's posture and not the
# aspace differ's: check_aspace_sigdiff.sh reports a diff for a milestone that was changing
# its seam and is deliberately off the ctest ladder, where this one asserts the capability ABI
# did NOT gain an addressing concept and a diff is a finding that must not land quietly.
# Where a member is added on purpose, the diff IS the finding and the baseline moves WITH the
# commit that adds it, never ahead of it.

set -eu
. "$(dirname "$0")/../lib/gate.sh"
. "$(dirname "$0")/../lib/signature_diff.sh"

HERE="$(dirname "$0")"

KOS_SD_TITLE="capability ABI signature diff"
KOS_SD_EXTRACT="$HERE/aspace_seam.awk"
KOS_SD_FAMILY="$HERE/cap_seam_family.awk"
KOS_SD_FAMILY_MSG="unreadable: $KOS_SD_FAMILY; the family is undefined and the extractor
      would fall back to the ASPACE prefix, reporting M6.3's verdict under this name"
KOS_SD_RECORDS="tests/static/cap_seam_records.txt"
KOS_SD_PREFIX_FILE="cap_seam_family.awk"

# The pathspecs the family is read from. Both are C-compatible headers, which is what the
# aspace extractor parses; the kernel-side C++ header is deliberately NOT here and
# cap_seam_family.awk says why in its own blind-spot note.
KOS_SD_CORPUS_ROWS=2
KOS_SD_CORPUS="
user/include/kickos/sys/abi.h
system/include/kickos/sys/cap_index.h
"
KOS_SD_MIN_FILES=2

# The seam header the family lives in today. Its absence on either side means the corpus was
# built from the wrong path, which would otherwise read as a clean empty diff.
KOS_SD_ANCHOR="user/include/kickos/sys/abi.h"

# Per-kind minimum record counts. Set below today's figures so a legitimate removal of a
# member does not trip them, and far above zero so a broken extraction does. This family
# declares no function and no function-like macro, the capability ABI reaching a program as
# types, constants and syscall numbers with its calls being the generic kos_* wrappers, so those
# kinds carry no floor: requiring one would fail on a correct tree.
KOS_SD_KINDS="ENUMERATOR MACRO TAG TYPEDEF"
KOS_SD_KIND_LABEL="kind "
KOS_SD_MIN_ENUMERATOR=11
KOS_SD_MIN_MACRO=2
KOS_SD_MIN_TAG=3
KOS_SD_MIN_TYPEDEF=1
KOS_SD_MIN_TOTAL=18
KOS_SD_REPORT_KINDS="ENUMERATOR MACRO TAG TYPEDEF FUNC"

# The group table. One line per group: <name> <name-regex> <floor>. Every record the family
# admits must match exactly one of these, which is what keeps this table and the PREFIX in
# cap_seam_family.awk from drifting apart.
#
# `frame` carries a floor of 0 and matches nothing today. It is the group M6.5's own objects
# land in, so their arrival is a diff against this baseline rather than a classification
# refusal against this table.
#
# NOT NAMED `GROUPS`: that identifier is a bash special variable (the caller's group ids), and
# an assignment to it does not take. KOS_GROUP_ROWS is the count the driver checks the parse
# against, so a shell that mangles this text fails loudly instead of dropping the floors.
KOS_GROUP_ROWS=7
KOS_GROUP_TABLE="
handle     ^(kos_cap_t|KOS_CAP_NONE)                                              1
rights     ^(kos_cap_rights|KOS_CAP_(WAIT|SIGNAL|TRANSFER))                       3
grant      ^(kos_cap_grant|KOS_SPAWN_DELEGATED)                                   1
authority  ^(kos_cap_authority|KOS_AUTH_)                                         5
index      ^(kos_cap_index|KOS_CAP_(STDOUT|CLOCK|FIRST_DYNAMIC|AUTHORITY))        4
syscall    ^KOS_SYS_(HANDLE_CLOSE|CAP_NARROW)                                     1
frame      ^(kos_frame|KOS_FRAME|kos_ptab|KOS_PTAB|KOS_SYS_(FRAME|PTAB|MAP|UNMAP)) 0
"
KOS_SD_MANGLED_TAIL="every per-group floor below would be empty and a family that lost a whole
      group would compare two short sets and report clean"

sigdiff_family_prose() {
    echo "   family    the capability layer as a USER PROGRAM sees it: the handle type, the"
    echo "             rights mirror, the grant record a spawn carries, the authority word, the"
    echo "             well-known index plane, and the close and narrow syscalls. Membership by"
    echo "             declared IDENTIFIER, in tests/static/cap_seam_family.awk, wherever it"
    echo "             stands. The frame and page-table prefixes match nothing yet and are here"
    echo "             so what M6.5 adds arrives as a diff rather than as silence."
    echo "   NOT here  the KERNEL-SIDE cap layer. kernel/include/kickos/cap.h is C++ and this"
    echo "             extractor reads a C header, so CapType, CapRights, CapEntry and the"
    echo "             resolve chokepoint are out of this verdict; their bit budget is held by"
    echo "             the static_assert set in that header instead. Also out: the aspace and"
    echo "             entry families, which have differs of their own."
}

sigdiff_rule_opening() {
    echo "   the rule, which is the aspace differ's extractor unchanged. REPORTED as a"
    echo "   signature difference:"
}

sigdiff_run "$@"
