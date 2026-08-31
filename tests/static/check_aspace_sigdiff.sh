#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The address-space seam's signature diff, and M6.3's deliverable is a NEGATIVE result:
# the arch_aspace_* family's signatures do not move when a second, deliberately unlike
# backend is fitted to them, and where one moves, that diff IS the finding. Section 3.4b
# requires the API to exist before the second backend starts, so the verdict is a
# comparison against the frozen records in tests/static/aspace_seam_records.txt, which is
# the seam as the first backend froze it.
#
#   tests/static/check_aspace_sigdiff.sh [<candidate-ref>]
#
# tests/lib/signature_diff.sh drives the comparison and documents the corpus, the record
# kinds, the exit codes, KOS_SIGDIFF_KEEP and KOS_SIGDIFF_REGEN. This file declares the
# ASPACE family: its identifier prefixes, its seam header, its floors, its baseline records
# and the prose of its report.
#
# The group floors close a false PASS no per-kind floor can see: the two partitions cross-cut,
# so dropping one alternative of the extractor's PREFIX can cost a single record and still
# clear every per-kind minimum.

set -eu
. "$(dirname "$0")/../lib/gate.sh"
. "$(dirname "$0")/../lib/signature_diff.sh"

HERE="$(dirname "$0")"

KOS_SD_TITLE="address-space seam signature diff"
KOS_SD_EXTRACT="$HERE/aspace_seam.awk"
KOS_SD_FAMILY=""
KOS_SD_FAMILY_MSG=""
KOS_SD_RECORDS="tests/static/aspace_seam_records.txt"
KOS_SD_PREFIX_FILE="aspace_seam.awk"

# The seam header the family lives in today. Its absence on either side means the corpus
# was built from the wrong path, which would otherwise read as a clean empty diff.
# The pathspecs the family is read from. Declared here rather than inside the driver so a
# caller cannot inherit a corpus it never named; KOS_SD_CORPUS_ROWS is what the driver checks
# the parse against, and KOS_SD_MIN_FILES floors what the walk actually read.
KOS_SD_CORPUS_ROWS=1
KOS_SD_CORPUS="
arch/include/kickos/arch
"
KOS_SD_MIN_FILES=3

KOS_SD_ANCHOR="arch/include/kickos/arch/arch.h"

# Per-kind minimum record counts. Set below today's figures so a legitimate removal of a
# member does not trip them, and far above zero so a broken extraction does.
KOS_SD_KINDS="FUNC ENUMERATOR MACRO TYPEDEF TAG"
KOS_SD_KIND_LABEL=""
KOS_SD_MIN_FUNC=8
KOS_SD_MIN_ENUMERATOR=8
KOS_SD_MIN_MACRO=5
KOS_SD_MIN_TYPEDEF=1
KOS_SD_MIN_TAG=1
KOS_SD_MIN_TOTAL=24
KOS_SD_REPORT_KINDS="FUNC ENUMERATOR MACRO TYPEDEF TAG"

# The group table. One line per group: <name> <name-regex> <floor>. The regexes are the
# alternatives of PREFIX in tests/static/aspace_seam.awk, one group each, which is what makes
# the driver's classification check bind this table to the family definition: an alternative
# added there with no group here fails loudly instead of going unfloored.
#
# A group of ONE carries a floor of 1, so a legitimate removal of that member does trip it.
# That is deliberate: dropping the only record an alternative contributes is exactly the loss
# no kind floor can see. Removing such a member means re-deciding its floor here.
#
# NOT NAMED `GROUPS`: bash owns that identifier as the caller's group ids, and an assignment to
# it does not take. KOS_GROUP_ROWS is the count the driver checks the parse against, so a shell
# that mangles this text fails loudly instead of dropping every floor.
KOS_GROUP_ROWS=5
KOS_GROUP_TABLE="
calls     ^arch_aspace          12
codes     ^ARCH_ASPACE          11
memtag    ^arch_map              1
mapbits   ^ARCH_MAP              5
physaddr  ^arch_phys_addr        1
"
KOS_SD_MANGLED_TAIL="every per-group floor below would be empty and the verdict would be a bare header
      read with none of the family membership this instrument adds"

sigdiff_family_prose() {
    echo "   family    identifiers matching arch_aspace / ARCH_ASPACE / arch_map / ARCH_MAP /"
    echo "             arch_phys_addr, wherever they stand in the seam headers"
}

sigdiff_rule_opening() {
    echo "   the rule. REPORTED as a signature difference:"
}

sigdiff_run "$@"
