# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The ENTRY AND BOOT path family, for tests/static/check_entry_sigdiff.sh. This file is
# handed to awk as a SECOND program after tests/static/aspace_seam.awk:
#
#   awk -f tests/static/aspace_seam.awk -f tests/static/entry_seam_family.awk <stripped>
#
# awk runs BEGIN blocks in the order the programs are named, so the assignment below
# replaces the aspace family's PREFIX and nothing else. The extraction RULES that decide
# what counts as a signature stay the aspace extractor's, byte for byte: a copy of it with
# one line changed lets the two verdicts drift apart silently.
#
# MEMBERSHIP, per F8's split of the seam. This family is the NON-aspace part of
# arch/include/kickos/arch/arch.h that a thread's privileged execution actually runs
# through: context initialisation and the switch, the syscall entries, the fault and
# containment seam, the interrupt triad, the console and timer edges, the per-core identity
# the fast entry resolves, the two user-pointer oracles a syscall validates through, and
# the boot and shutdown lifecycle.
#
# DELIBERATELY OUT, because a family that grows to the whole header proves nothing about
# the path F8 named: the aspace family and its map bits (M6.3's verdict, and
# check_aspace_sigdiff.sh reports it); the region and MPU family; the RAM and reserved
# block description; the peripheral, pinmux, clock tree, bit band, cache maintenance and
# diagnostic LED edges; and the trace, nesting and trap stack witnesses, which observe the
# path rather than being on it.
#
# GROUPS. check_entry_sigdiff.sh carries a group table over the same members and floors
# every group on both sides: an extraction that lost one group whole would otherwise compare
# two sets with no console edge in either and report a clean diff, which is the false PASS a
# total floor does not catch. That script REFUSES a record this file admits and its table
# does not classify, and a record two groups claim, so a member added to PREFIX here without
# a group there fails loudly instead of going unfloored.

BEGIN {
    PREFIX = "^(arch_context|arch_switch|arch_start|arch_ctx_" \
             "|arch_syscall" \
             "|arch_fault_|kickos_fault_|kickos_isr_fault" \
             "|kickos_thread_" \
             "|arch_irq_|arch_in_isr|arch_ipi_|kickos_isr_irq|kickos_isr_timer" \
             "|arch_console_" \
             "|arch_timer_|arch_clock_now" \
             "|arch_init|arch_shutdown|arch_reboot|arch_idle_wait" \
             "|arch_cpu_id|KICKOS_NUM_CORES" \
             "|arch_user_data_writable|arch_user_text_readable" \
             "|KICKOS_ARCH_HAS_IPC_FASTPATH|KICKOS_KERNEL_STACKS)"
}
