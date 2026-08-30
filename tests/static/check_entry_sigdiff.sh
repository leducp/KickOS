#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The ENTRY AND BOOT path seam's signature diff, which is what docs/design-m6-mmu.md step X6
# reports. F8 splits the seam in two and gives each half its own backend: RV64 falsifies the
# address-space family, which tests/static/check_aspace_sigdiff.sh measures for step R5, and
# x86_64 falsifies "a syscall entry that loads no stack pointer, and adopting an already-live
# translation regime handed over by firmware". This script measures THAT half. Running the
# aspace differ and calling its answer the x86_64 verdict would report on the family M6.3
# already owns.
#
#   tests/static/check_entry_sigdiff.sh [<candidate-ref>]
#
# tests/lib/signature_diff.sh drives the comparison and documents the corpus, the record
# kinds, the exit codes, KOS_SIGDIFF_KEEP and KOS_SIGDIFF_REGEN. This file declares the
# ENTRY family: its identifier prefixes, its seam header, its floors, its baseline records
# and the prose of its report.
#
# THE EXTRACTOR IS tests/static/aspace_seam.awk VERBATIM, with
# tests/static/entry_seam_family.awk named after it so its BEGIN block replaces the family
# PREFIX and nothing else. Forking the extractor lets the two verdicts come to disagree about
# what a signature IS; only membership may differ.
#
# THE BASELINE IS THIS FAMILY'S OWN RECORD FILE, tests/static/entry_seam_records.txt, holding
# the entry and boot path seam as it stood before either new backend. The two families hold
# different record sets, and each verdict moves when its own seam moves.

set -eu
. "$(dirname "$0")/../lib/gate.sh"
. "$(dirname "$0")/../lib/signature_diff.sh"

HERE="$(dirname "$0")"

KOS_SD_TITLE="entry and boot path seam signature diff"
KOS_SD_EXTRACT="$HERE/aspace_seam.awk"
KOS_SD_FAMILY="$HERE/entry_seam_family.awk"
KOS_SD_FAMILY_MSG="unreadable: $KOS_SD_FAMILY; the family is undefined and the extractor
      would fall back to the ASPACE prefix, reporting M6.3's verdict under this name"
KOS_SD_RECORDS="tests/static/entry_seam_records.txt"
KOS_SD_PREFIX_FILE="entry_seam_family.awk"

# The seam header the family lives in today. Its absence on either side means the corpus was
# built from the wrong path, which would otherwise read as a clean empty diff.
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
# member does not trip them, and far above zero so a broken extraction does. This family
# declares no complete tag and no enumerator in the seam headers, the context structure being
# opaque above the seam and every result being a plain int or bool, so those two kinds carry
# no floor: requiring one would fail on a correct tree.
KOS_SD_KINDS="FUNC TYPEDEF"
KOS_SD_KIND_LABEL="kind "
KOS_SD_MIN_FUNC=40
KOS_SD_MIN_TYPEDEF=1
KOS_SD_MIN_TOTAL=45
KOS_SD_REPORT_KINDS="FUNC MACRO MACROFN TYPEDEF OBJ"

# The group table. One line per group: <name> <name-regex> <floor>. Every record the family
# admits must match exactly one of these, which is what keeps this table and the PREFIX in
# entry_seam_family.awk from drifting apart.
#
# NOT NAMED `GROUPS`: that identifier is a bash special variable (the caller's group ids), and
# an assignment to it does not take. KOS_GROUP_ROWS is the count the driver checks the parse
# against, so a shell that mangles this text fails loudly instead of dropping the floors.
KOS_GROUP_ROWS=10
KOS_GROUP_TABLE="
context   ^(arch_context|arch_switch|arch_start|arch_ctx_)                          4
syscall   ^(arch_syscall|KICKOS_ARCH_HAS_IPC_FASTPATH)                              2
fault     ^(arch_fault_|kickos_fault_|kickos_isr_fault|KICKOS_KERNEL_STACKS)        8
contain   ^kickos_thread_                                                           3
irq       ^(arch_irq_|arch_in_isr|arch_ipi_|kickos_isr_irq|kickos_isr_timer)        11
console   ^arch_console_                                                            5
time      ^(arch_timer_|arch_clock_now)                                             2
boot      ^(arch_init|arch_shutdown|arch_reboot|arch_idle_wait)                     3
percore   ^(arch_cpu_id|KICKOS_NUM_CORES)                                           2
oracle    ^arch_user_(data_writable|text_readable)                                  1
"
KOS_SD_MANGLED_TAIL="every per-group floor below would be empty and the verdict would be the aspace
      differ's with none of the membership this instrument adds"

sigdiff_family_prose() {
    echo "   family    the NON-aspace seam a thread's privileged execution runs through:"
    echo "             context and switch, the syscall entries, fault and containment, the"
    echo "             interrupt triad, the console and timer edges, the per-core identity, the"
    echo "             user-pointer oracles, and boot and shutdown. Membership by declared"
    echo "             IDENTIFIER, in tests/static/entry_seam_family.awk, wherever it stands."
    echo "   NOT here  the aspace family and its map bits, which is M6.3's verdict and what"
    echo "             tests/static/check_aspace_sigdiff.sh reports; the region and MPU family;"
    echo "             the RAM, reserved block, peripheral, clock, cache and diagnostic edges;"
    echo "             and the trace and witness hooks, which observe the path, not run on it."
}

sigdiff_rule_opening() {
    echo "   the rule, which is the aspace differ's extractor unchanged. REPORTED as a"
    echo "   signature difference:"
}

sigdiff_run "$@"
