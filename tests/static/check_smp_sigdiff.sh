#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The SMP SEAM's signature diff, frozen at the tree that has NO SMP backend in it. The four
# members below are what a second core is reached through today: the core count, the per-core
# identity, and the two halves of the cross-core doorbell. A baseline taken after the first
# backend lands would measure that backend against itself.
#
#   tests/static/check_smp_sigdiff.sh [<candidate-ref>]
#
# tests/lib/signature_diff.sh drives the comparison and documents the corpus, the record
# kinds, the exit codes, KOS_SIGDIFF_KEEP and KOS_SIGDIFF_REGEN. This file declares the SMP
# family: its corpus, its identifier prefixes, its floors, its baseline records and the prose
# of its report.
#
# THE EXTRACTOR IS tests/static/aspace_seam.awk VERBATIM, with
# tests/static/smp_seam_family.awk named after it so its BEGIN block replaces the family
# PREFIX and nothing else.
#
# THESE FOUR MEMBERS LEFT THE ENTRY FAMILY AT M7.0. MOVED, not copied: two differs asserting
# one signature are two authorities on one fact, and the moment they disagree neither is
# evidence.
#
# EXPECTED VERDICT: a DIFF, at the commit that gives the doorbell its first backend or gives
# the cross-core lock its seam member. That diff is the finding and the baseline moves WITH
# the commit that earns it, never ahead of it. Until then, PASS.

set -eu
. "$(dirname "$0")/../lib/gate.sh"
. "$(dirname "$0")/../lib/signature_diff.sh"

HERE="$(dirname "$0")"

KOS_SD_TITLE="SMP seam signature diff"
KOS_SD_EXTRACT="$HERE/aspace_seam.awk"
KOS_SD_FAMILY="$HERE/smp_seam_family.awk"
KOS_SD_FAMILY_MSG="unreadable: $KOS_SD_FAMILY; the family is undefined and the extractor
      would fall back to the ASPACE prefix, reporting M6.3's verdict under this name"
KOS_SD_RECORDS="tests/static/smp_seam_records.txt"
KOS_SD_PREFIX_FILE="smp_seam_family.awk"

# The pathspecs the family is read from. The whole arch include directory rather than the one
# header the members stand in today, so a member moved into a new seam header stays in the
# corpus instead of leaving it as a deletion.
KOS_SD_CORPUS_ROWS=1
KOS_SD_CORPUS="
arch/include/kickos/arch
"
KOS_SD_MIN_FILES=3

# The seam header the family lives in today. Its absence on either side means the corpus was
# built from the wrong path, which would otherwise read as a clean empty diff.
KOS_SD_ANCHOR="arch/include/kickos/arch/arch.h"

# Per-kind minimum record counts. Set below today's figures so a legitimate removal of a
# member does not trip them, and far above zero so a broken extraction does. This family
# declares no tag, no enumerator, no typedef and no object: a core index is a uint32_t and a
# core set is a bitmask in one, so those kinds carry no floor and requiring one would fail on
# a correct tree.
KOS_SD_KINDS="FUNC MACRO MACROFN"
KOS_SD_KIND_LABEL="kind "
KOS_SD_MIN_FUNC=2
KOS_SD_MIN_MACRO=1
KOS_SD_MIN_MACROFN=2
KOS_SD_MIN_TOTAL=6
KOS_SD_REPORT_KINDS="FUNC MACRO MACROFN TAG TYPEDEF OBJ"

# The group table. One line per group: <name> <name-regex> <floor>. Every record the family
# admits must match exactly one of these, which is what keeps this table and the PREFIX in
# smp_seam_family.awk from drifting apart.
#
# Each group is floored on both sides, because check_entry_sigdiff.sh shows what a family
# without per-group floors reports when an extraction loses one group whole: two sets with no
# doorbell in either compare clean, and the total floor does not catch it.
#
# Every member here has TWO records, the multi-core declaration and the single-core fold,
# except the core count which has one. The floors sit one member below each group so removing
# a member is a diff rather than a refusal.
#
# NOT NAMED `GROUPS`: that identifier is a bash special variable (the caller's group ids), and
# an assignment to it does not take. KOS_GROUP_ROWS is the count the driver checks the parse
# against, so a shell that mangles this text fails loudly instead of dropping the floors.
KOS_GROUP_ROWS=3
KOS_GROUP_TABLE="
extent    ^KICKOS_NUM_CORES     1
identity  ^arch_cpu_id          1
doorbell  ^arch_ipi_            2
"
KOS_SD_MANGLED_TAIL="every per-group floor below would be empty and a family that lost the
      doorbell whole would compare two short sets and report clean"

sigdiff_family_prose() {
    echo "   family    the SMP seam as the kernel reaches a second core through it: the core"
    echo "             count, the per-core identity, and the two halves of the cross-core"
    echo "             doorbell. Both arms of every member, the multi-core declaration and the"
    echo "             single-core fold, so a change to the fold is a difference too."
    echo "             Membership by declared IDENTIFIER, in tests/static/smp_seam_family.awk,"
    echo "             wherever it stands."
    echo "   NOT here  the cross-core lock, which has no seam member yet and joins this family"
    echo "             in the commit that declares it. Also out: the per-core structures above"
    echo "             the seam, which are C++ and out of this extractor's reach; the entry,"
    echo "             aspace and capability families, which have differs of their own; and"
    echo "             what a backend DOES behind a member, a signature being the shape of a"
    echo "             call and not its ordering."
}

sigdiff_rule_opening() {
    echo "   the rule, which is the aspace differ's extractor unchanged. REPORTED as a"
    echo "   signature difference:"
}

sigdiff_run "$@"
