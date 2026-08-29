#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot the address-space image under qemu-system-x86_64 with UEFI firmware and assert every
# arm the aspace family owes on a single root register reports.
#
#   tools/run-qemu-x86_64-x5.sh <application.efi> [workdir]
#
# The machine, the EFI system partition and the serial capture come from
# tools/run-qemu-x86_64-common.sh.
#
# Environment:
#   KICKOS_X5_TOKEN     the token every line must carry (default below). Held against the IMAGE,
#                       never scraped from the source.
#   KICKOS_X5_FIRMWARE  `pflash` (split OVMF_CODE_4M plus OVMF_VARS_4M, the default) or
#                       `bios` (the combined /usr/share/ovmf/OVMF.fd through -bios).
#   KICKOS_X5_MACHINE   qemu machine type, default q35.
#   KICKOS_X5_TIMEOUT   seconds, default 120.
#   KICKOS_X86_64_CPU   a -cpu model, default none. Shared by all five witnesses.
#
# THE INVALIDATION ARMS NEED A SELF-TEST BUILD, the seam publishing its invalidation counts
# only there. An image built without it says so on its own line and fails.
#
# POSIX sh (dash-clean).

set -u

KOS_TOOLS=$(cd "$(dirname "$0")" && pwd); . "$KOS_TOOLS/run-qemu-x86_64-common.sh"

KOS_STEP=X5
KOS_TOKEN="${KICKOS_X5_TOKEN:-KICKOS-X5 6b1e04c7 x86_64/q35 aspace}"
KOS_FIRMWARE="${KICKOS_X5_FIRMWARE:-pflash}"
KOS_MACHINE="${KICKOS_X5_MACHINE:-q35}"
KOS_TIMEOUT="${KICKOS_X5_TIMEOUT:-120}"
KOS_WORK_LEAF=x5run
# The image ends itself: arch_shutdown writes isa-debug-exit, so the emulator's exit code
# carries the status, (status << 1) | 1, and 1 is a pass while 3 is the image's own FAIL.
KOS_END=exit

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    fail "usage: run-qemu-x86_64-x5.sh <application.efi> [workdir]"
fi

kos_boot "$1" "${2:-}"

need "the image never reached its arms" "^$TOK arms\$"
need "no pool report line" \
     "^  $TOK pool base=0x[0-9a-f]\{16\} frames=[1-9][0-9]* va=0x[0-9a-f]\{16\}\$"
need "no model report line" \
     "^  $TOK model=0x[0-9a-f]\{16\} levels=[45] tag_bits=[0-9][0-9]* tag_invalidate=[01]\$"
HEX16='0x[0-9a-f]\{16\}'
need "no boot-space report line" \
     "^  $TOK boot root=$HEX16 kernel_slots=[1-9][0-9]* user_lo=$HEX16 user_hi=$HEX16\$"
need "no attribute-table report line" \
     "^  $TOK pat live=$HEX16 power_up=$HEX16\$"
need "no kernel-window report line" \
     "^  $TOK window va=0x[0-9a-f]\{16\} pages=[1-9][0-9]* first_child_entries=[0-9][0-9]*\$"
need "no root report line" \
     "^  $TOK roots boot=0x[0-9a-f]\{16\} a=0x[0-9a-f]\{16\} b=0x[0-9a-f]\{16\}\$"
need "no table-cost report line" "^  $TOK tables per span=[1-9][0-9]*\$"
need "no invalidation report line" \
     "^  $TOK tlbi issued=[1-9][0-9]* elided=[0-9][0-9]*\$"
need "no frame-accounting report line" \
     "^  $TOK frames outstanding=[0-9][0-9]* baseline=[0-9][0-9]* allocations=[1-9][0-9]*\$"
need "no result-class report line" \
     "^  $TOK results ok=[1-9][0-9]* enomem=[1-9][0-9]* ecapacity=0 einval=[1-9][0-9]*\$"

for a in granule_is_4k levels_four_or_five levels_match_control_register \
         model_granule_bore_out model_physical_range_bore_out \
         model_identifier_matches_record model_physical_bits_reported \
         model_one_granule_reported model_identifier_width_is_the_record \
         memtype_normal memtype_nocache memtype_device memtype_unknown_refused \
         boot_space_answered boot_space_is_the_installed_root kernel_half_has_slots \
         user_half_measured boot_maps_this_image \
         boot_maps_conventional_memory_identically boot_walk_handles_a_large_leaf \
         user_half_unmapped_in_boot map_into_boot_refused unmap_from_boot_refused \
         destroy_boot_is_a_no_op destroy_null_is_a_no_op \
         create_a create_costs_one_frame create_b two_spaces_are_distinct \
         kernel_half_in_a kernel_half_in_b conventional_memory_in_a \
         user_half_empty_in_a user_half_empty_in_b \
         kernel_window_anchored kernel_window_outside_the_user_half \
         map_at_the_window_refused \
         kernel_window_starts_unmapped kernel_window_unmapped_in_a \
         kernel_window_map kernel_edit_seen_from_boot kernel_edit_seen_from_a \
         kernel_edit_seen_from_b kernel_window_reads_under_a kernel_window_reads_under_b \
         kernel_remap_under_a kernel_remap_reads_under_a kernel_remap_reads_under_b \
         kernel_remap_seen_from_boot_walk kernel_window_unmap kernel_unmap_seen_from_a \
         kernel_unmap_seen_from_b kernel_unmap_faults_under_boot \
         kernel_unmap_fault_was_a_translation_fault kernel_unmap_fault_named_the_window \
         kernel_window_unmap_twice_refused \
         map_a map_b switch_code_data_mapped_under_a switch_code_data_mapped_under_b \
         activate_a_wrote_the_root activate_b_wrote_the_root \
         activate_boot_restored_the_root root_a_is_the_handle root_b_is_the_handle \
         no_identifier_in_any_root one_address_reads_a one_address_reads_b \
         one_address_faults_under_boot boot_fault_was_a_translation_fault \
         boot_fault_named_the_address one_address_reads_a_again \
         write_through_a_reached_the_frame \
         refuse_null_space refuse_misaligned_va refuse_zero_pages refuse_misaligned_pa \
         refuse_write_and_execute refuse_no_read refuse_unknown_right \
         refuse_unknown_memtype refuse_kernel_half_address \
         refuse_pa_past_the_physical_width refuse_pa_range_past_the_physical_width \
         kernel_window_map_past_the_physical_width_refused \
         refuse_range_past_the_user_half refuse_page_count_that_wraps \
         nothing_was_mapped_by_a_refusal unmap_refuses_a_partial_range \
         the_partial_range_is_still_mapped unmap_refuses_an_unmapped_page \
         leaf_memtype_normal_is_write_back leaf_memtype_nocache_is_uncached_minus \
         leaf_memtype_device_is_uncacheable \
         memtype_decode_power_up_normal_selects_field_zero \
         memtype_decode_power_up_nocache_selects_field_two \
         memtype_decode_power_up_device_selects_field_three \
         memtype_decode_permuted_normal_selects_field_four \
         memtype_decode_permuted_nocache_selects_field_five \
         memtype_decode_permuted_device_selects_field_six \
         memtype_decode_takes_the_first_field_that_matches \
         memtype_decode_refuses_a_table_with_no_write_back \
         memtype_decode_answers_the_other_types_of_that_table \
         memtype_decode_refuses_a_table_with_no_uncached_minus \
         memtype_decode_refuses_a_table_with_no_uncacheable \
         memtype_decode_refuses_an_unknown_type \
         attribute_table_in_use_is_the_live_msr \
         leaf_bits_match_the_decode_on_the_live_table \
         span_map span_first_page span_page_at_the_table_boundary span_last_page \
         span_page_before_is_unmapped span_page_after_is_unmapped span_unmap \
         span_first_page_gone span_last_page_gone \
         cost_of_a_clean_map_measured the_measuring_space_came_back \
         out_of_frames_on_the_first_table first_table_refusal_installed_nothing \
         space_still_maps_after_a_refusal and_the_mapping_answers cleanup_unmap \
         first_table_refusal_leaked_no_frame out_of_frames_partway \
         partway_refusal_left_no_partial_mapping partway_refusal_leaked_no_frame \
         acquire_answers_the_frames_own_address acquire_reads_the_frame \
         acquire_carries_the_offset acquire_of_an_unmapped_page_is_null \
         acquire_of_a_null_space_is_null acquire_floor_is_met \
         acquire_after_release_still_answers frame_at_agrees_with_acquire \
         frame_at_drops_the_offset frame_at_of_an_unmapped_page_is_zero \
         frame_at_of_a_null_space_is_zero frame_at_separates_the_two_spaces \
         fresh_map_issues_one replacing_a_live_page_issues_two and_the_replacement_took \
         unmap_issues_one map_into_a_space_this_core_is_not_on_elides \
         borrowed_page_unmapped_before_destroy the_lent_frame_survived_the_unmap \
         a_shared_slot_diverged_by_an_accessed_bit the_diverged_slot_kept_its_table \
         kernel_half_survives_a_destroy the_shared_window_survives_a_destroy \
         the_other_space_still_maps_its_own_page \
         the_other_space_still_reads_its_own_page \
         second_borrowed_page_unmapped_before_destroy \
         kernel_half_survives_the_second_destroy \
         the_window_survives_the_second_destroy a_frame_for_the_space_to_own \
         mapped_and_left_mapped destroy_reclaimed_the_frame_the_space_mapped \
         every_frame_came_back every_result_class_but_capacity_was_reached \
         capacity_refusal_is_unproducible root_register_ends_on_the_boot_space
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
