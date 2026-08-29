#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot the ring 3 image under qemu-system-x86_64 with UEFI firmware and assert every arm
# ring 3, the fast syscall entry, the kernel stack that entry loads by hand, the flag mask it
# runs under, fault attribution and the two user-pointer oracles report.
#
# It also carries the CPL3 reachability CENSUS: a walk of the whole live hierarchy counting
# every leaf carrying the user bit. The device, low-legacy and outside counts are asserted
# zero; the reachable translation table and the per-core block are PINNED to what the
# measurement found, each named by role rather than by address.
#
#   tools/run-qemu-x86_64-x4.sh <application.efi> [workdir]
#
# The machine, the EFI system partition and the serial capture come from
# tools/run-qemu-x86_64-common.sh.
#
# Environment:
#   KICKOS_X4_TOKEN     the token every line must carry (default below). Held against the IMAGE,
#                       never scraped from the source.
#   KICKOS_X4_FIRMWARE  `pflash` (split OVMF_CODE_4M plus OVMF_VARS_4M, the default) or
#                       `bios` (the combined /usr/share/ovmf/OVMF.fd through -bios).
#   KICKOS_X4_MACHINE   qemu machine type, default q35.
#   KICKOS_X4_TIMEOUT   seconds, default 120.
#   KICKOS_X86_64_CPU   a -cpu model, default none. Shared by all five witnesses.
#
# POSIX sh (dash-clean).

set -u

KOS_TOOLS=$(cd "$(dirname "$0")" && pwd); . "$KOS_TOOLS/run-qemu-x86_64-common.sh"

KOS_STEP=X4
KOS_TOKEN="${KICKOS_X4_TOKEN:-KICKOS-X4 4f2ba917 x86_64/q35 ring3}"
KOS_FIRMWARE="${KICKOS_X4_FIRMWARE:-pflash}"
KOS_MACHINE="${KICKOS_X4_MACHINE:-q35}"
KOS_TIMEOUT="${KICKOS_X4_TIMEOUT:-120}"
KOS_WORK_LEAF=x4run
# The image ends itself: arch_shutdown writes isa-debug-exit, so the emulator's exit code
# carries the status, (status << 1) | 1, and 1 is a pass while 3 is the image's own FAIL.
KOS_END=exit

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    fail "usage: run-qemu-x86_64-x4.sh <application.efi> [workdir]"
fi

kos_boot "$1" "${2:-}"

need "the image never reached its arms" "^$TOK arms\$"
need "no register report line" \
     "^  $TOK cr4=0x[0-9a-f]\{16\} efer=0x[0-9a-f]\{16\}\$"
need "no image report line" \
     "^  $TOK image=0x[0-9a-f]\{16\} size=0x[0-9a-f]\{16\} sections=[1-9][0-9]*\$"
need "no grant report line" \
     "^  $TOK leaves granted=[1-9][0-9]* already=[0-9][0-9]* tables_exposed=[0-9][0-9]* tables_walked=[1-9][0-9]*\$"
# The CPL3 reachability census. The line shapes below hold the report to being printed; the
# arms further down hold the figures, the walker's floor and its two controls included.
need "no census header line" \
     "^  $TOK census levels=[45] root=0x[0-9a-f]\{16\} tables=[1-9][0-9]* dropped=[0-9][0-9]*\$"
need "no census extent line" \
     "^  $TOK census extents image=0x[0-9a-f]\{16\}\.\.0x[0-9a-f]\{16\} arena=0x[0-9a-f]\{16\}\.\.0x[0-9a-f]\{16\}\$"
need "no census reachable line" \
     "^  $TOK census reachable leaves=[1-9][0-9]* bytes=0x[0-9a-f]\{16\} writable=[0-9][0-9]* nonidentity=[0-9][0-9]*\$"
need "no census overlap line" \
     "^  $TOK census overlap image=[0-9][0-9]* arena=[0-9][0-9]* table=[0-9][0-9]* percpu=[0-9][0-9]* lowlegacy=[0-9][0-9]* apicwindow=[0-9][0-9]* outside=[0-9][0-9]*\$"
need "no pre-grant user-bit line" \
     "^  $TOK census pregrant u_nonleaf=[0-9][0-9]* u_leaf=[0-9][0-9]* u_reachable=[0-9][0-9]* u_dormant=[0-9][0-9]*\$"
need "no post-grant user-bit line" \
     "^  $TOK census postgrant u_nonleaf=[0-9][0-9]* u_leaf=[0-9][0-9]* u_reachable=[0-9][0-9]* u_dormant=[0-9][0-9]*\$"
need "no translation-table answer" \
     "^  $TOK census q1 tables_reachable=[0-9][0-9]* of=[1-9][0-9]* first=0x[0-9a-f]\{16\} first_writable=[01]\$"
need "no device answer" \
     "^  $TOK census q2 device_leaves=[0-9][0-9]* lowlegacy=[0-9][0-9]* apicwindow=[0-9][0-9]*\$"
need "no per-core anchor answer" \
     "^  $TOK census q3 anchor=0x[0-9a-f]\{16\} reachable=[01] writable=[01] kernel_sp_at=0x[0-9a-f]\{16\}\$"
need "no unprivileged read-back of the table" \
     "^  $TOK census ring3 table_va=0x[0-9a-f]\{16\} read=[01] word=0x[0-9a-f]\{16\} ring0_word=0x[0-9a-f]\{16\}\$"
need "no unprivileged read-back of the per-core block" \
     "^  $TOK census ring3 anchor_va=0x[0-9a-f]\{16\} read=[01] word=0x[0-9a-f]\{16\} ring0_word=0x[0-9a-f]\{16\}\$"
need "no pinned-table header line" \
     "^  $TOK census pin tables_reachable=[0-9][0-9]* pinned=1 listed=[0-9][0-9]* unlisted=[0-9][0-9]* kernel_window_l3=0x[0-9a-f]\{16\}\$"
# By ROLE, not address: a link moves every address on these two lines.
need "the reachable table is not the kernel window's" \
     "^  $TOK census pin table pa=0x[0-9a-f]\{16\} writable=[01] role=kernel-window-l3\$"
need "the anchor is not the per-core block" \
     "^  $TOK census pin anchor pa=0x[0-9a-f]\{16\} leaves=[0-9][0-9]* pinned=1 role=per-core-block\$"
if grep -q "^  $TOK census pin table pa=0x[0-9a-f]\{16\} writable=[01] role=UNPINNED\$" "$PLAIN"; then
    fail "a reachable translation table that the record does not name: $(grep "census pin table pa=" "$PLAIN" | grep UNPINNED | head -1)"
fi
need "no unprivileged register line" \
     "^  $TOK user cs=0x[0-9a-f]\{16\} ss=0x[0-9a-f]\{16\} flags=0x[0-9a-f]\{16\}\$"
need "no dispatch line" \
     "^  $TOK dispatch rsp=0x[0-9a-f]\{16\} flags=0x[0-9a-f]\{16\} cs=0x[0-9a-f]\{16\}\$"

for a in efer_sce_set fmask_clears_if fmask_clears_iopl \
         star_syscall_cs star_sysret_base lstar_in_image \
         smep_smap_clear user_leaves_granted \
         walked_tables_census_nonempty walked_tables_not_user_reachable \
         census_table_record_complete census_hierarchy_floor \
         census_found_reachable_leaves census_found_the_image census_found_the_arena \
         census_control_admits_a_reachable_leaf census_control_refuses_the_root_table \
         census_no_apic_window_leaves census_no_low_legacy_leaves \
         census_no_leaves_outside_image_or_arena \
         census_pin_reachable_tables_listed census_pin_one_reachable_table \
         census_pin_table_is_the_kernel_window census_pin_one_table_bearing_leaf \
         census_pin_anchor_is_the_percpu_block census_pin_one_percpu_leaf \
         census_pin_one_image_leaf census_pin_reachable_set_is_image_and_arena \
         census_pin_table_and_anchor_writable \
         oracle_read_admits_rodata oracle_read_admits_text oracle_read_admits_data \
         oracle_read_refuses_null oracle_read_refuses_arena oracle_read_refuses_past_image \
         oracle_read_admits_long_range oracle_read_refuses_cross_section \
         oracle_read_zero_len \
         oracle_write_admits_data oracle_write_refuses_text oracle_write_refuses_rodata \
         oracle_write_refuses_arena oracle_write_zero_len \
         cr0_read_returns_at_ring0 \
         ctx_frame_on_block ctx_kernel_sp_kept \
         ring3_cs_is_user ring3_cpl_is_three ring3_ss_is_user \
         ring3_runs_with_interrupts ring3_iopl_zero \
         syscall_on_kernel_block syscall_off_user_stack syscall_frame_below_block_top \
         syscall_interrupts_masked syscall_runs_at_ring0 syscall_not_in_isr \
         syscall_args_delivered syscall_result64 syscall_result_low \
         tss_rsp0_is_block_top percpu_sp_is_block_top \
         syscall_blocks_and_resumes syscall_block_result \
         ring3_cannot_raise_iopl ring3_keeps_interrupts_on_return \
         ring3_preempted ring3_exits_through_trap \
         kfault_seen kfault_on_kernel_block kfault_cs_is_kernel kfault_not_attributed \
         cr0_read_refused cr0_read_general_protection cr0_read_frame_cs_is_user \
         cr0_read_attributed_to_thread cr0_read_frame_on_kernel_block \
         cr0_read_death_stub_privileged cr0_read_death_stub_on_block cr0_read_thread_died \
         port_write_refused port_write_general_protection port_write_frame_cs_is_user \
         port_write_attributed_to_thread port_write_frame_on_kernel_block \
         port_write_death_stub_privileged port_write_death_stub_on_block \
         port_write_thread_died \
         sysret_reached_ring3 sysret_cs_is_user sysret_ss_is_user \
         ring0_syscall_result ring0_syscall_dispatched ring0_syscall_on_caller_stack
do
    arm_ok "$a"
done

if grep -q "$TOK FAIL" "$PLAIN"; then
    fail "the image reported its own failure"
fi
need "the image did not reach its PASS" "^$TOK PASS\$"

kos_require_clean_exit

echo "PASS: $KOS_TOKEN, every arm reported ok"
echo "      serial: $PLAIN"
