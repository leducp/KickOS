#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The SMP SEAM's signature diff. The members below are what a second core is reached through:
# the core count, the count one kernel schedules on, the per-core identity, the two halves of
# the cross-core doorbell, the two halves of the cross-core kernel lock, and the four the
# kernel supplies for a shared kernel's backend to call.
#
# THE BASELINE MOVES ONLY WITH A COMMIT THAT DELIBERATELY MOVES A MEMBER: rewritten at any
# other, it measures a backend against itself.
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
# THE FOUR KERNEL-SUPPLIED MEMBERS HAVE NO SINGLE-CORE ARM, where every other member folds: a
# kernel scheduling one core has no far side to call them, so their absence below one core is
# the fold. A MACROFN arm appearing for one of them would be a difference.
#
# EVERY MEMBER BELONGS TO EXACTLY ONE FAMILY: two differs asserting one signature are two
# authorities on one fact, and the moment they disagree neither is evidence.

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

# The pathspecs the family is read from: the whole arch include directory, so a member moved
# into a new seam header stays in the corpus instead of leaving it as a deletion.
KOS_SD_CORPUS_ROWS=1
KOS_SD_CORPUS="
arch/include/kickos/arch
"
KOS_SD_MIN_FILES=3

# The seam header the family lives in today. Its absence on either side means the corpus was
# built from the wrong path, which would otherwise read as a clean empty diff.
KOS_SD_ANCHOR="arch/include/kickos/arch/arch.h"

# Per-kind minimum record counts. Set below today's figures so a legitimate removal of a
# member does not trip them, and far above zero so a broken extraction does. The family declares
# no tag, enumerator, typedef or object, so those kinds carry no floor.
KOS_SD_KINDS="FUNC MACRO MACROFN"
KOS_SD_KIND_LABEL="kind "
KOS_SD_MIN_FUNC=4
KOS_SD_MIN_MACRO=2
KOS_SD_MIN_MACROFN=4
KOS_SD_MIN_TOTAL=12
KOS_SD_REPORT_KINDS="FUNC MACRO MACROFN TAG TYPEDEF OBJ"

# The group table. One line per group: <name> <name-regex> <floor>. Every record the family
# admits must match exactly one of these, which is what keeps this table and the PREFIX in
# smp_seam_family.awk from drifting apart.
#
# Each group is floored on both sides: an extraction that loses one group whole compares two
# sets with no doorbell in either and reads clean, which the total floor does not catch.
#
# Every member here has TWO records, the multi-core declaration and the single-core fold,
# except the two counts which have one each. The floors sit one member below each group so
# removing a member is a diff rather than a refusal.
#
# NOT NAMED `GROUPS`: that identifier is a bash special variable (the caller's group ids), and
# an assignment to it does not take. KOS_GROUP_ROWS is the count the driver checks the parse
# against, so a shell that mangles this text fails loudly instead of dropping the floors.
KOS_GROUP_ROWS=5
KOS_GROUP_TABLE="
extent    ^KICKOS_(NUM|KERNEL)_CORES   1
identity  ^arch_cpu_id                 1
doorbell  ^arch_ipi_                   2
lock      ^arch_kernel_(un)?lock       2
peer      ^kickos_(switch_unlock|kernel_core_)  3
"
KOS_SD_MANGLED_TAIL="every per-group floor below would be empty and a family that lost the
      doorbell whole would compare two short sets and report clean"

sigdiff_family_prose() {
    echo "   family    the SMP seam as the kernel reaches a second core through it: the core"
    echo "             count, the count one kernel schedules on, the per-core identity, the two"
    echo "             halves of the cross-core doorbell, the two halves of the cross-core"
    echo "             kernel lock, and the set the kernel supplies for a shared kernel's"
    echo "             backend to call. Both arms of every member that HAS two, the multi-core"
    echo "             declaration and the single-core fold, so a change to the fold is a"
    echo "             difference too. Membership by declared IDENTIFIER, in"
    echo "             tests/static/smp_seam_family.awk, wherever it stands."
    echo "   NOT here  the per-core structures above the seam, which are C++ and out of this"
    echo "             extractor's reach; the entry, aspace and capability families, which have"
    echo "             differs of their own; and what a backend DOES behind a member. That last"
    echo "             one covers the coupling: the lock and the doorbell are one design, and"
    echo "             neither the acquire loop's servicing of a pending doorbell nor the far"
    echo "             side's taking no lock is visible in a signature."
}

sigdiff_rule_opening() {
    echo "   the rule, which is the aspace differ's extractor unchanged. REPORTED as a"
    echo "   signature difference:"
}

sigdiff_run "$@"
