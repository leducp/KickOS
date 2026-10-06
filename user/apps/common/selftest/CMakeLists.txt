# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A list: on a split board these have to reach EVERY image, or the parts get different
# suites.
set(_selftest_defs "")

# The two `return <int>;` literals cmake/boot_arena.cmake scraped out of the backend .cc this
# link resolves. Unset on the sim, which reaches no scrape. Both derefs MUST stay quoted:
# get_property leaves the variable UNSET for a property never set, and an unquoted unset name
# compares as its own literal, so the guard would pass and define both macros EMPTY, which
# still satisfies `#if defined`.
get_property(_mpu_min_region_cfg GLOBAL PROPERTY KICKOS_MPU_MIN_REGION_CFG)
get_property(_mpu_region_pow2_cfg GLOBAL PROPERTY KICKOS_MPU_REGION_POW2_CFG)
if(NOT "${_mpu_min_region_cfg}" STREQUAL "" AND NOT "${_mpu_region_pow2_cfg}" STREQUAL "")
  list(APPEND _selftest_defs
    KICKOS_MPU_MIN_REGION_CFG=${_mpu_min_region_cfg}
    KICKOS_MPU_REGION_POW2_CFG=${_mpu_region_pow2_cfg})
endif()

# microbit's arena holds either the UART service's 1 KiB ring or the arena probes, so the choice
# is pinned by name here.
if(KICKOS_BOARD STREQUAL "microbit")
  list(APPEND _selftest_defs KICKOS_SELFTEST_NO_UART_SERVICE=1)
endif()

# A device the kernel never drives on each translating machine, which window_addr and window_get
# map at the address the kernel chooses: the PL031 RTC, the goldfish RTC and the HPET.
if(KICKOS_CHIP STREQUAL "virt_arm64")
  set(_selftest_spare_dev 0x09010000)
elseif(KICKOS_CHIP STREQUAL "virt_rv64")
  set(_selftest_spare_dev 0x00101000)
elseif(KICKOS_CHIP STREQUAL "q35")
  set(_selftest_spare_dev 0xFED00000)
endif()
if(DEFINED _selftest_spare_dev)
  list(APPEND _selftest_defs KICKOS_SELFTEST_SPARE_DEV=${_selftest_spare_dev})
endif()
# The default stack a thread's exit unmaps, which shootdown_per_change sizes its bound by.
list(APPEND _selftest_defs KICKOS_SELFTEST_USER_STACK_SIZE=${KICKOS_USER_STACK_SIZE})
# A bus master no driver of the selftest's composition holds: virtio-mmio slots 0 to 7.
if(KICKOS_CHIP STREQUAL "virt_arm64")
  list(APPEND _selftest_defs KICKOS_SELFTEST_BUS_MASTER_DEV=0x0A000000)
endif()

# main.cc cuts the registration list into contiguous REGIONS. An image carries a contiguous
# RUN of them and elides the rest, so the arms are partitioned between the images and still
# run in the order one image would run them in. main.cc refuses a build whose count differs
# from the one stated here, because a region no image carries runs nowhere and no plan check
# can see it.
set(_selftest_regions 10)

# The images this board ships, as the first region of each: image i carries its first region up
# to the next image's first less one, so the images are contiguous, in order, and cover every
# region once. Order across images does not matter, each one initialising the board and running
# its own arms alone.
#
# A per-board property, not a fleet one, and the binding budget is a different one on each of
# them: 64 KiB of FLASH on the two STM32 parts, the 128 KiB pow2 PMP code window (`_code_size`
# in arch/riscv/chip/esp32c6/esp32c6.ld) on an enforcing ESP32-C6, whose next step up would take
# 128 KiB straight off the thread arena, the 240 KiB node slice on the ESP32-C6's own-image AMP
# partition, the arena that follows .bss on nrf51 and the 56 KiB one between the app window and
# the kernel stack on an enforcing XMC4800 (`xmc4800.ld`), which never takes a block back and is
# spent before region 9's grants and stacks, and the 128 KiB IRAM every ESP32 instruction
# runs from (`IRAM` in arch/xtensa/chip/esp32/esp32.ld), which the bench probes overrun beside a
# console driver or the second core's code. The bench probes take the C6's code window past one
# more cut. None of these is a number the build can read back from the board, so the cut is
# stated here and nowhere else: the chip, KICKOS_HAVE_MPU, KICKOS_AMP_OWN_IMAGE and KICKOS_BENCH
# come from the board's own config, and every gate takes the image list and each image's arm
# count off the targets below rather than restating either.
set(_selftest_firsts 1)
if(KICKOS_CHIP STREQUAL "stm32f103" OR KICKOS_CHIP STREQUAL "stm32f302")
  set(_selftest_firsts 1 2 3 4 5 6 7 8 9)
elseif(KICKOS_CHIP STREQUAL "nrf51")
  set(_selftest_firsts 1 5 7 9 10)
elseif(KICKOS_CHIP STREQUAL "xmc4800" AND KICKOS_HAVE_MPU)
  set(_selftest_firsts 1 9)
elseif(KICKOS_CHIP STREQUAL "esp32c6" AND KICKOS_HAVE_MPU AND KICKOS_BENCH)
  set(_selftest_firsts 1 3 5 7 10)
elseif(KICKOS_CHIP STREQUAL "esp32c6" AND (KICKOS_HAVE_MPU OR KICKOS_AMP_OWN_IMAGE))
  set(_selftest_firsts 1 5 7 10)
elseif(KICKOS_CHIP STREQUAL "esp32" AND (KICKOS_BENCH OR KICKOS_KERNEL_CORES GREATER 1))
  set(_selftest_firsts 1 7)
endif()
set(_selftest_bound "")
set(_selftest_prev 0)
foreach(_first IN LISTS _selftest_firsts)
  if(NOT _first GREATER _selftest_prev OR _first GREATER _selftest_regions)
    message(FATAL_ERROR
      "selftest: the images' first regions ${_selftest_firsts} must rise from 1 within the "
      "${_selftest_regions} region(s) main.cc cuts, or an image would carry no region at all, "
      "plan zero arms and pass.")
  endif()
  if(_selftest_prev GREATER 0)
    math(EXPR _last "${_first} - 1")
    list(APPEND _selftest_bound "${_selftest_prev}-${_last}")
  endif()
  set(_selftest_prev ${_first})
endforeach()
list(GET _selftest_firsts 0 _selftest_first)
if(NOT _selftest_first EQUAL 1)
  message(FATAL_ERROR "selftest: the first image must start at region 1, not ${_selftest_first}")
endif()
list(APPEND _selftest_bound "${_selftest_prev}-${_selftest_regions}")

# The suite runs under the kernel's console and under each console driver of this build's
# catalogue that consoles/<board>/ composes, each as its own run of images. An AMP node runs it
# under the kernel's console, as this node of a partition whose other nodes run ampecho, or as
# the shared image's one composition.
set(_selftest_consoles "")
set(_selftest_console_files "")
get_property(_selftest_catalogue GLOBAL PROPERTY KICKOS_DRIVER_CATALOGUE)
if(NOT KICKOS_AMP_NODE)
  file(GLOB _selftest_console_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/consoles/${KICKOS_BOARD}"
       CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/consoles/${KICKOS_BOARD}/*.yaml")
endif()
set(_selftest_amp_dir "systems/${KICKOS_BOARD}/${KICKOS_CONFIG_VARIANT}")
set(_selftest_partition "")
if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  math(EXPR _selftest_last_node "${KICKOS_AMP_NODES} - 1")
  foreach(_node RANGE 0 ${_selftest_last_node})
    if(_node EQUAL KICKOS_AMP_NODE_ID)
      list(APPEND _selftest_partition "${_selftest_amp_dir}/node${_node}.yaml")
    else()
      list(APPEND _selftest_partition "${_selftest_amp_dir}/echo${_node}.yaml")
    endif()
  endforeach()
endif()
foreach(_file IN LISTS _selftest_console_files)
  string(REGEX REPLACE "\\.yaml$" "" _driver "${_file}")
  if(_driver IN_LIST _selftest_catalogue)
    list(APPEND _selftest_consoles ${_driver})
  endif()
endforeach()
# reply_abandoned_cap creates two endpoints of main's own beside each one main holds: every
# crossing the partition names and, under a console driver, the console's.
set(_selftest_main_endpoints ${KICKOS_AMP_PORT_COUNT})
if(_selftest_consoles)
  math(EXPR _selftest_main_endpoints "${_selftest_main_endpoints} + 1")
endif()
math(EXPR _selftest_endpoint_peak "${_selftest_main_endpoints} + 2")
if(KICKOS_TASK_ENDPOINT_BUDGET LESS _selftest_endpoint_peak)
  message(FATAL_ERROR
    "selftest: main holds up to ${_selftest_main_endpoints} endpoint(s) and reply_abandoned_cap "
    "creates 2 more, past KICKOS_TASK_ENDPOINT_BUDGET ${KICKOS_TASK_ENDPOINT_BUDGET}")
endif()
set(_selftest_images "")
set(_selftest_region_lo "")
set(_selftest_region_hi "")
set(_selftest_image_console "")
foreach(_console kernel ${_selftest_consoles})
  set(_console_img selftest)
  if(NOT _console STREQUAL "kernel")
    set(_console_img selftest_${_console})
  endif()
  set(_part 0)
  foreach(_run IN LISTS _selftest_bound)
    math(EXPR _part "${_part} + 1")
    string(REPLACE "-" ";" _run "${_run}")
    list(GET _run 0 _part_lo)
    list(GET _run 1 _part_hi)
    set(_part_img "${_console_img}_p${_part}")
    if(_part EQUAL 1)
      set(_part_img ${_console_img})
    endif()
    list(APPEND _selftest_images ${_part_img})
    list(APPEND _selftest_region_lo ${_part_lo})
    list(APPEND _selftest_region_hi ${_part_hi})
    list(APPEND _selftest_image_console ${_console})
  endforeach()
endforeach()
list(LENGTH _selftest_bound _selftest_parts)

list(APPEND _selftest_defs KICKOS_SELFTEST_REGIONS=${_selftest_regions})

set(_selftest_srcs main.cc selftest_common.cc
                   "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
# Four blocks of arms sit in files of their own. Each still carries the `#if` its block had in
# main.cc, so a file compiled where its guard is off is empty and costs only the header; the
# conditions here spare that parse and must therefore MATCH those guards. Getting one wrong
# cannot shrink a suite: the registration list names every arm, so a file this list omits while
# main.cc still registers its arms is an undefined reference at link.
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  list(APPEND _selftest_srcs selftest_aspace.cc)
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION
   AND KICKOS_ARCH STREQUAL "x86_64")
  list(APPEND _selftest_srcs selftest_vector.cc)
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  list(APPEND _selftest_srcs selftest_amp.cc)
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  list(APPEND _selftest_srcs selftest_smp.cc)
endif()
# spi_mock.cc is a stand-in SPI backend under t_bus_device_slots, gated exactly like that arm. It
# DEFINES the public kos_spi_* names while the image may also carry a chip driver, which is
# legal only because the SPI service drivers rename their class symbols;
# tests/static/check_class_backend.sh asserts it per link.
if(KICKOS_ENABLE_SELFTEST)
  list(APPEND _selftest_srcs spi_mock.cc)
endif()

# No arm throws or reads a type at run time. A source property, so it follows the leaf's own
# options and holds on a leaf that compiles with exceptions and RTTI.
set_source_files_properties(${_selftest_srcs} PROPERTIES
  COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")

set(_img_index 0)
foreach(_img IN LISTS _selftest_images)
  list(GET _selftest_region_lo ${_img_index} _img_lo)
  list(GET _selftest_region_hi ${_img_index} _img_hi)
  list(GET _selftest_image_console ${_img_index} _img_console)
  math(EXPR _img_index "${_img_index} + 1")
  add_executable(${_img} ${_selftest_srcs})
  if(_img_console STREQUAL "kernel" AND _selftest_partition)
    kickos_app_system(${_img} PARTITION ${_selftest_partition} SYSTEM selftest_system)
  elseif(_img_console STREQUAL "kernel" AND KICKOS_AMP_NODE)
    kickos_app_system(${_img} COMPOSITION ${_selftest_amp_dir}/node0.yaml SYSTEM selftest_system)
  elseif(_img_console STREQUAL "kernel")
    kickos_app_system(${_img} COMPOSITION system.yaml SYSTEM selftest_system)
  else()
    kickos_app_system(${_img} COMPOSITION consoles/${KICKOS_BOARD}/${_img_console}.yaml
                      SYSTEM selftest_${_img_console}_system)
    target_compile_definitions(${_img} PRIVATE KICKOS_SELFTEST_CONSOLE_DRIVER=1)
  endif()
  kickos_emit_image(${_img})
  target_include_directories(${_img} PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")
  target_compile_definitions(${_img} PRIVATE ${_selftest_defs}
    KICKOS_SELFTEST_FIRST_REGION=${_img_lo} KICKOS_SELFTEST_LAST_REGION=${_img_hi})
  # Recorded on the target, so a gate picks an image's skip and partial sets by the regions
  # it carries and its console, instead of by a name written out a second time.
  set_target_properties(${_img} PROPERTIES
    KICKOS_SELFTEST_FIRST_REGION "${_img_lo}" KICKOS_SELFTEST_LAST_REGION "${_img_hi}"
    KICKOS_SELFTEST_CONSOLE "${_img_console}"
    KICKOS_IMAGE_JUDGE tests/integration/check_tap_stream.sh)
  # At the fleet Debug default this app's .text overflows the 64K-flash parts.
  if(NOT KICKOS_ARCH STREQUAL "sim")
    target_compile_options(${_img} PRIVATE -Os)
  endif()
endforeach()
set_property(GLOBAL PROPERTY KICKOS_SELFTEST_IMAGES "${_selftest_images}")

# The linked image and its map, for the host gates that read them. Only the map names the
# archive MEMBER each symbol was resolved from, which is what proves a backend's definition beat
# its fallback. x86_64's link rule writes <image>.map itself (tools/x86_64-link.sh).
#
# Recorded on the target: the gates that read the map live on the test side, and a path
# restated there would be a second truth that drifts from the -Map this link actually passed.
foreach(_img IN LISTS _selftest_images)
  set(_img_elf "$<TARGET_FILE:${_img}>")
  if(NOT KICKOS_ARCH STREQUAL "x86_64")
    target_link_options(${_img} PRIVATE "-Wl,-Map=${_img_elf}.map")
    set_property(TARGET ${_img} APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_img_elf}.map")
  endif()
  set_target_properties(${_img} PROPERTIES
    KICKOS_IMAGE_ELF "${_img_elf}" KICKOS_IMAGE_MAP "${_img_elf}.map")
endforeach()
if(KICKOS_ARCH STREQUAL "x86_64")
  # OVMF places an image at its preferred base when it can, so every other image boots with
  # firmware's relocation pass idle. 1 TiB is past any q35 RAM this tree boots, so firmware
  # has to move this one, and the app window's words are then relocated twice.
  add_executable(selftest_rebased $<TARGET_OBJECTS:selftest>)
  set_target_properties(selftest_rebased PROPERTIES LINKER_LANGUAGE CXX
    ADDITIONAL_CLEAN_FILES "$<TARGET_FILE:selftest_rebased>.map")
  target_link_libraries(selftest_rebased PRIVATE $<TARGET_PROPERTY:selftest,LINK_LIBRARIES>)
  target_link_options(selftest_rebased PRIVATE $<TARGET_PROPERTY:selftest,LINK_OPTIONS>
                                               "LINKER:--image-base=0x10000000000")
  set_target_properties(selftest PROPERTIES
    KICKOS_REBASED_IMAGE_FILE "$<TARGET_FILE:selftest_rebased>")
endif()

# An arm whose body left main.cc has EXTERNAL linkage, so the compiler no longer reports it
# unused when its TAP_ADD line goes. Every such arm is declared in selftest.h and registered in
# main.cc, and this holds the two sets equal, which is the guard -Werror=unused-function gives
# the arms that stayed. It reads the files rather than a list, so neither side can be satisfied
# by a name written here.
#
# A commented-out TAP_ADD reads as a registration to a scan that has no notion of comments, and
# the two comment forms need different answers. A /* */ block can span lines, so it is stripped
# from the whole file. A // line comment cannot be stripped safely, a // inside a string literal
# being indistinguishable here, so the match is anchored after a newline and leading blank
# instead: a registration commented out with // has the // between the two and does not match.
# Drop that anchor and the scan reports a //-commented arm as registered, which is the state
# this check exists to prevent.
#
# The match runs on the text rather than on a CMake list of "lines": this file has macro bodies
# that continue a line with a trailing backslash, and turning a newline into a list-separating
# ";" makes that backslash read as an ESCAPED semicolon, silently merging the continued lines
# into one element and losing every match inside them.
file(READ main.cc _selftest_main_raw)
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _selftest_main_nocomment "${_selftest_main_raw}")
string(REGEX MATCHALL "\n[ \t]*TAP_ADD(_IRQ)?\\(\"[^\"]*\", *[A-Za-z_][A-Za-z0-9_]*\\)" _selftest_added "${_selftest_main_nocomment}")
set(_selftest_registered "")
foreach(_row IN LISTS _selftest_added)
  if(_row MATCHES "TAP_ADD(_IRQ)?\\(\"[^\"]*\", *([A-Za-z_][A-Za-z0-9_]*)\\)")
    list(APPEND _selftest_registered "${CMAKE_MATCH_2}")
  endif()
endforeach()

# A declaration can split `void` from its name across a newline, which a per-line scan cannot
# see: fold newlines to spaces before matching, across the whole header. A declaration's own
# trailing ";" can then double up with MATCHALL's list separator and leave an empty element
# behind, which is dropped rather than fed to the name extraction below.
file(READ selftest.h _selftest_h_raw)
string(REGEX REPLACE "\n" " " _selftest_h_flat "${_selftest_h_raw}")
string(REGEX MATCHALL "void[ \t]+t_[A-Za-z0-9_]*\\([ \t]*\\)[ \t]*;" _selftest_declared_rows "${_selftest_h_flat}")
list(REMOVE_ITEM _selftest_declared_rows "")
foreach(_row IN LISTS _selftest_declared_rows)
  string(REGEX MATCH "(t_[A-Za-z0-9_]*)\\(" _m "${_row}")
  if(NOT "${CMAKE_MATCH_1}" IN_LIST _selftest_registered)
    message(FATAL_ERROR
      "selftest.h declares the arm ${CMAKE_MATCH_1}, which no TAP_ADD line in main.cc "
      "registers. Its body is in a file of its own, so it has external linkage and the "
      "compiler cannot report it unused: the arm would be built into every image and run in "
      "none. Register it, or delete the declaration and the body together.")
  endif()
endforeach()
list(LENGTH _selftest_declared_rows _selftest_declared_n)
message(STATUS "selftest: ${_selftest_declared_n} arm(s) declared in selftest.h, all registered")

# The EXACT number of tap::add calls main.cc reaches in this posture. tap.cc plans `1..N` from the
# RUNTIME registry, so plan and case count shrink together when an arm is deleted and only an
# expectation from OUTSIDE the image can see it. The conditions mirror the #if guards around the
# registration block at the bottom of main.cc, one clause per guard.
set(_tap_arms 110) # unconditional, incl. prio_self_raise_lower, prio_self_boosted, prio_self_ceiling, console_publish_handout, console_publish_narrow, task_authority, task_watch_reports, task_exit_entry_return, task_exit_member_exit, task_dead_after_sweep, task_dead_after_every_sweep, task_slay_after_every_sweep, endpoint_handout, endpoint_handout_parked, endpoint_zero_accept, notify_no_line, notify_bind_busy, reply_recv_loop, reply_recv_no_reply, reply_recv_timeout, reply_recv_bad_ep_wakes_caller, service_survives_client_fault, ipc_one_buffer_both_ends, readable_global, endpoint_send_timeout, recv_timeout, timed_arg_refusals, reply_recv_lens_clamp, 3 x call_timeout_*, reply_stale_caller, reply_abandoned_cap, thread_join, join_stale_gen, join_timeout, task_handles, task_member_refusals, task_creator_gate, task_group_kill, 3 x thread_slay_*, 2 x task_slay_*
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 16") # bus_device_slots, uart_service, reboot_priv, 13 x irq_*
  math(EXPR _tap_arms "${_tap_arms} + 4") # call_reg_fastpath, reply_recv_notify, reply_recv_notify_park, irq_wait_timeout (region 4)
  math(EXPR _tap_arms "${_tap_arms} + 4") # doorbell_xpoke, irq_kernel_line_reserved, prio_ceiling_refused, prio_ceiling_narrow_only
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 3") # endpoint_bound, grant_reserved, dev_window_exclusive
endif()
if(KICKOS_ENABLE_SELFTEST AND NOT KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms "${_tap_arms} + 2") # ram_alloc_zeroed, uncached_grant_sync
endif()
if(KICKOS_ENABLE_SELFTEST AND (KICKOS_HAVE_MPU OR DEFINED _selftest_spare_dev))
  math(EXPR _tap_arms "${_tap_arms} + 1") # window_get
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms "${_tap_arms} + 1") # stackbase_arena
endif()
# Both enforcing backends and neither flat one, which is the arm's own guard.
if(KICKOS_MEMORY_ENFORCED)
  math(EXPR _tap_arms "${_tap_arms} + 1") # cross_task_block
endif()
if(KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms "${_tap_arms} + 1") # caller_stack_arena
endif()
if(KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms "${_tap_arms} + 1") # task_exit_driver_trap
endif()
if(KICKOS_MEMORY_ENFORCED AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms "${_tap_arms} + 5") # window_memory_ro, task_exit_member_fault, task_exit_entry_killed, task_exit_cancelled_fault, task_exit_implicit_fault
endif()
if(NOT KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms "${_tap_arms} + 1") # region_mode
endif()
if(KICKOS_ARCH STREQUAL "sim")
  math(EXPR _tap_arms "${_tap_arms} + 2") # periph_reg_write_mask, window_list
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms "${_tap_arms} + 58") # frame_run_slot_recycle, call_reply_undisclosed, cap_objects, cap_map, recv_buf_unmapped, cap_map_over_stack, stack_grant_refused, stack_guard_intact, stack_handoff_refused, stack_slot_returns, cap_map_pins_run, cap_share, aspace_seam, aspace_model, aspace_map_cycle, aspace_translate, aspace_refusals, aspace_span, aspace_acquire_dup, aspace_balance, aspace_domain_balance, aspace_forced_unwind, aspace_churn, stack_is_frames, 2 x aspace_two_spaces_*, process_private_data, task_siblings_share, task_handoff_readback, task_handoff_donor_exits, task_handoff_slice, reservation_teardown, frame_scrub_cross_task, 2 x spawn_refusal_frees_*, parked_frame_hostile, split_access, process_ipc_same_addr, process_call_reply, grant_kernel_word_refused, self_grant_retype, uncached_alias_sync, uncached_teardown, presync_retried, presync_race, presync_flip, presync_slain, presync_staged, presync_cancel, presync_late_params, out_of_lock_windows, reent_seating, aspace_acquire_balance, map_tlbi_elided, aspace_active_cores, app_pointers_relocated, process_data_from_image, process_data_template
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1
   AND KICKOS_ARCH STREQUAL "x86_64")
  math(EXPR _tap_arms "${_tap_arms} + 1") # shootdown_per_change
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms "${_tap_arms} + 1") # crowd_room_in_root_space
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms "${_tap_arms} + 2") # fault_kills_task, kernel_state_unreachable
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION
   AND DEFINED _selftest_spare_dev)
  math(EXPR _tap_arms "${_tap_arms} + 1") # window_addr
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION
   AND KICKOS_ARCH STREQUAL "x86_64")
  math(EXPR _tap_arms "${_tap_arms} + 5") # port_window, vector_survives_block, vector_survives_preempt, vector_starts_clean, vector_fault_contained
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION
   AND KICKOS_ARCH STREQUAL "x86_64" AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms "${_tap_arms} + 1") # vector_survives_migrate
endif()
# Mirrors the C guard, which is the AMP posture and not the core count: an own-image node drives
# one core and registers these all the same.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  # amp_far_call and amp_far_reply_guard are in this count on every posture: they skip by name
  # where no peer answers rather than being compiled out.
  math(EXPR _tap_arms "${_tap_arms} + 23") # amp_far_deliver_fault, amp_window, amp_reset_record, amp_port_seating, amp_port_unnamed, amp_far_call, amp_far_reply_guard, amp_far_reply_empty, amp_far_service, amp_far_refusal_answered, amp_deferred_doorbell, amp_inbound_reply, amp_far_undisclosed, amp_far_infoless, amp_reply_band, amp_mint_reply_port, amp_reply_reserve, amp_probe_crossing_holder, amp_local_port_slot_held, amp_far_slot_reuse, amp_far_reset_answers, amp_far_answer_deferred, amp_far_tail_recovery
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  math(EXPR _tap_arms "${_tap_arms} + 1") # amp_share_crossing
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE AND KICKOS_MEMORY_ENFORCED
   AND NOT KICKOS_AMP_USER_SHARE_SIZE EQUAL 0)
  math(EXPR _tap_arms "${_tap_arms} + 2") # amp_share_seated, amp_share_window
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms "${_tap_arms} + 26") # resched_reaches_pinned_caller, prio_self_lower_moves_waiter, pin_places, pin_wrong_core_never, unpin_restores, affinity_zero_defaults, affinity_undriven_refused, affinity_dead_handle_refused, migrate_running, pin_same_task_ok, pin_cross_task_refused, grant_narrows, grant_wider_refused, grant_after_member_refused, isolated_single_grant_ok, isolated_unpinned_never, isolated_unpin_excludes, isolated_takes_pinned, isolated_mixed_mask_ok, grant_second_narrow_only, grant_inherited_by_child_task, slice_preempts_every_core, threads_reach_every_core, irq_cross_core_wake, irq_reclaim_stale_raise, reent_per_thread_cores
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1 AND KICKOS_ARCH STREQUAL "x86_64")
  math(EXPR _tap_arms "${_tap_arms} + 1") # fp_enabled_every_core
endif()

# The same count per REGION. The regions are separated by the `#undef TAP_ADD` lines in
# main.cc: region 1 ends after sem_raii, region 2 after endpoint_handout, region 3 after
# reply_recv_timeout, region 4 after call_donation_hold, region 5 after cap_reply_slot_reuse,
# region 6 after task_exit_member_exit, region 7 after task_slay_after_every_sweep, region 8
# after the IRQ arms, region 9 after caller_stack, region 10 is the rest.
#
# These MUST stay independent expressions, never derived from _tap_arms above: the equality
# asserted below is only a check while both sides are stated separately, and a new arm has to
# move a clause on each side.
set(_tap_arms_r1 16) # region 1, unconditional, svc_roundtrip through sem_raii: the scheduler/sync/time/console arms

set(_tap_arms_r2 18) # region 2, unconditional, mutex_basic through endpoint_handout: 10 x mutex_*, 2 x prio_self_*, 6 x endpoint_*

set(_tap_arms_r3 17) # region 3, unconditional: endpoint_handout_parked, endpoint_zero_accept, endpoint_send_timeout, recv_timeout, timed_arg_refusals, reply_recv_lens_clamp, 3 x call_timeout_*, reply_stale_caller, reply_abandoned_cap, call_infoless_revert, call_close_reply, call_happy, reply_recv_loop, reply_recv_no_reply, reply_recv_timeout

set(_tap_arms_r4 9) # region 4, unconditional, reply_recv_bad_ep_wakes_caller through call_donation_hold, incl. reply_recv_bad_ep_wakes_caller, service_survives_client_fault, call_from_root, call_truncation, call_double_reply, call_server_death, call_prepop_death, call_donation, call_donation_hold
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_r4 "${_tap_arms_r4} + 4") # reply_recv_notify_park, reply_recv_notify, irq_wait_timeout, call_reg_fastpath
endif()

set(_tap_arms_r5 11) # region 5, unconditional: call_donation_slow, call_donation_pending, endpoint_crossdomain, cap_index0, cap_chunk_span, cap_gen_reuse, cap_child_width, cap_reply_bound_fast, cap_reply_bound_slow, cap_reply_release_close, cap_reply_slot_reuse
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_r5 "${_tap_arms_r5} + 2") # bus_device_slots, uart_service
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_r5 "${_tap_arms_r5} + 1") # endpoint_bound
endif()

set(_tap_arms_r6 14) # region 6, unconditional: console_publish_priv, shutdown_priv, writable_global, readable_global, authority_cap, task_authority, periph_enable_unheld, periph_reg_write_unheld, privileged_spawn_refused, thread_join, join_stale_gen, join_timeout, task_exit_entry_return, task_exit_member_exit
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_r6 "${_tap_arms_r6} + 1") # reboot_priv
endif()
if(KICKOS_ARCH STREQUAL "sim")
  math(EXPR _tap_arms_r6 "${_tap_arms_r6} + 2") # periph_reg_write_mask, window_list
endif()

set(_tap_arms_r7 14) # region 7, unconditional, task_dead_after_sweep through task_slay_after_every_sweep, incl. task_dead_after_sweep, task_dead_after_every_sweep, task_handles, task_member_refusals, task_creator_gate, task_group_kill, prio_self_ceiling, task_watch_reports, 3 x thread_slay_*, 3 x task_slay_*

set(_tap_arms_r8 2) # region 8, unconditional, notify_no_line through the IRQ arms: notify_no_line, notify_bind_busy
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_r8 "${_tap_arms_r8} + 13") # 13 x irq_*
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms_r8 "${_tap_arms_r8} + 2") # irq_cross_core_wake, irq_reclaim_stale_raise
endif()

set(_tap_arms_r9 4) # region 9, unconditional, caller_stack_arena through caller_stack: mem_self_grant_nonpow2, domain_share, mmio_grant, caller_stack
if(KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms_r9 "${_tap_arms_r9} + 1") # caller_stack_arena
endif()
if(NOT KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms_r9 "${_tap_arms_r9} + 1") # region_mode
endif()
if(KICKOS_MEMORY_ENFORCED)
  math(EXPR _tap_arms_r9 "${_tap_arms_r9} + 1") # cross_task_block
endif()
if(KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms_r9 "${_tap_arms_r9} + 1") # task_exit_driver_trap
endif()
if(KICKOS_MEMORY_ENFORCED AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms_r9 "${_tap_arms_r9} + 5") # window_memory_ro, task_exit_member_fault, task_exit_entry_killed, task_exit_cancelled_fault, task_exit_implicit_fault
endif()
if(KICKOS_ENABLE_SELFTEST AND (KICKOS_HAVE_MPU OR DEFINED _selftest_spare_dev))
  math(EXPR _tap_arms_r9 "${_tap_arms_r9} + 1") # window_get
endif()
if(KICKOS_ENABLE_SELFTEST AND NOT KICKOS_HAVE_ASPACE)
  math(EXPR _tap_arms_r9 "${_tap_arms_r9} + 2") # ram_alloc_zeroed, uncached_grant_sync
endif()

set(_tap_arms_r10 5) # region 10, unconditional, stackbase_arena through mem_self_grant, incl. ipc_one_buffer_both_ends, confused_deputy, console_publish_handout, console_publish_narrow, mem_self_grant
if(KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 4") # doorbell_xpoke, irq_kernel_line_reserved, prio_ceiling_refused, prio_ceiling_narrow_only
endif()
if(KICKOS_HAVE_MPU)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 1") # stackbase_arena
endif()
if(KICKOS_HAVE_MPU AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 2") # grant_reserved, dev_window_exclusive
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 58") # frame_run_slot_recycle, call_reply_undisclosed, cap_objects, cap_map, recv_buf_unmapped, cap_map_over_stack, stack_grant_refused, stack_guard_intact, stack_handoff_refused, stack_slot_returns, cap_map_pins_run, cap_share, aspace_seam, aspace_model, aspace_map_cycle, aspace_translate, aspace_refusals, aspace_span, aspace_acquire_dup, aspace_balance, aspace_domain_balance, aspace_forced_unwind, aspace_churn, stack_is_frames, 2 x aspace_two_spaces_*, process_private_data, task_siblings_share, task_handoff_readback, task_handoff_donor_exits, task_handoff_slice, reservation_teardown, frame_scrub_cross_task, 2 x spawn_refusal_frees_*, parked_frame_hostile, split_access, process_ipc_same_addr, process_call_reply, grant_kernel_word_refused, self_grant_retype, uncached_alias_sync, uncached_teardown, presync_retried, presync_race, presync_flip, presync_slain, presync_staged, presync_cancel, presync_late_params, out_of_lock_windows, reent_seating, aspace_acquire_balance, map_tlbi_elided, aspace_active_cores, app_pointers_relocated, process_data_from_image, process_data_template
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1
   AND KICKOS_ARCH STREQUAL "x86_64")
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 1") # shootdown_per_change
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 1") # crowd_room_in_root_space
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 2") # fault_kills_task, kernel_state_unreachable
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION
   AND DEFINED _selftest_spare_dev)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 1") # window_addr
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION
   AND KICKOS_ARCH STREQUAL "x86_64")
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 5") # port_window, vector_survives_block, vector_survives_preempt, vector_starts_clean, vector_fault_contained
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION
   AND KICKOS_ARCH STREQUAL "x86_64" AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 1") # vector_survives_migrate
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 23") # amp_far_deliver_fault, amp_window, amp_reset_record, amp_port_seating, amp_port_unnamed, amp_far_call, amp_far_reply_guard, amp_far_reply_empty, amp_far_service, amp_far_refusal_answered, amp_deferred_doorbell, amp_inbound_reply, amp_far_undisclosed, amp_far_infoless, amp_reply_band, amp_mint_reply_port, amp_reply_reserve, amp_probe_crossing_holder, amp_local_port_slot_held, amp_far_slot_reuse, amp_far_reset_answers, amp_far_answer_deferred, amp_far_tail_recovery
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 1") # amp_share_crossing
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE AND KICKOS_MEMORY_ENFORCED
   AND NOT KICKOS_AMP_USER_SHARE_SIZE EQUAL 0)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 2") # amp_share_seated, amp_share_window
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 24") # resched_reaches_pinned_caller, prio_self_lower_moves_waiter, pin_places, pin_wrong_core_never, unpin_restores, affinity_zero_defaults, affinity_undriven_refused, affinity_dead_handle_refused, migrate_running, pin_same_task_ok, pin_cross_task_refused, grant_narrows, grant_wider_refused, grant_after_member_refused, isolated_single_grant_ok, isolated_unpinned_never, isolated_unpin_excludes, isolated_takes_pinned, isolated_mixed_mask_ok, grant_second_narrow_only, grant_inherited_by_child_task, slice_preempts_every_core, threads_reach_every_core, reent_per_thread_cores
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1 AND KICKOS_ARCH STREQUAL "x86_64")
  math(EXPR _tap_arms_r10 "${_tap_arms_r10} + 1") # fp_enabled_every_core
endif()

# Every region main.cc cuts owes a count, and the counts must add up to the whole suite. That
# is what makes the partition TOTAL and DISJOINT, and it is the whole of what makes splitting
# the suite safe: a region with no count, or a sum that misses an arm, means an arm that runs
# in no image at all, which reconciles with itself and reads green.
set(_tap_arms_regions "")
foreach(_region RANGE 1 ${_selftest_regions})
  if(NOT DEFINED _tap_arms_r${_region})
    message(FATAL_ERROR
      "selftest: main.cc cuts ${_selftest_regions} region(s), and region ${_region} has no "
      "arm count in ${CMAKE_CURRENT_LIST_FILE}. Declare one, do not lower the region count.")
  endif()
  list(APPEND _tap_arms_regions "${_tap_arms_r${_region}}")
endforeach()
set(_tap_arms_split 0)
foreach(_region_arms IN LISTS _tap_arms_regions)
  math(EXPR _tap_arms_split "${_tap_arms_split} + ${_region_arms}")
endforeach()
if(NOT _tap_arms_split EQUAL _tap_arms)
  string(REPLACE ";" " + " _tap_arms_sum "${_tap_arms_regions}")
  message(FATAL_ERROR
    "selftest arm partition is not total: the per-region counts ${_tap_arms_sum} = "
    "${_tap_arms_split}, but the whole suite registers ${_tap_arms} arms in this posture. "
    "An arm was added, deleted or moved across a region boundary in main.cc without its "
    "per-region clause moving with it. Fix the counts in ${CMAKE_CURRENT_LIST_FILE}, do "
    "not relax this check.")
endif()

# tap.cc sizes its registry from KICKOS_TAP_MAX_TESTS. Per IMAGE and not per board, so it
# cannot go through _selftest_defs, which applies one list to every image. An arm added
# without its clause overflows the registry and run_all() reports tap_registry_overflow.
#
# The SAME number is what a gate outside the image expects the run to plan, so each image
# records its own on itself and the gate reads it off the image it boots: a gate that picked
# the images by hand would be a second statement of which region is in which.
set(_img_index 0)
set(_selftest_plan "")
foreach(_img IN LISTS _selftest_images)
  list(GET _selftest_region_lo ${_img_index} _img_lo)
  list(GET _selftest_region_hi ${_img_index} _img_hi)
  math(EXPR _img_index "${_img_index} + 1")
  set(_img_arms 0)
  foreach(_region RANGE ${_img_lo} ${_img_hi})
    math(EXPR _img_arms "${_img_arms} + ${_tap_arms_r${_region}}")
  endforeach()
  target_compile_definitions(${_img} PRIVATE KICKOS_TAP_MAX_TESTS=${_img_arms})
  set_target_properties(${_img} PROPERTIES KICKOS_TAP_ARMS "${_img_arms}")
  list(APPEND _selftest_plan "${_img} plans ${_img_arms} (region(s) ${_img_lo}-${_img_hi})")
endforeach()
if(_selftest_parts GREATER 1)
  string(REPLACE ";" ", " _selftest_plan "${_selftest_plan}")
  message(STATUS
    "selftest: split into ${_selftest_parts} images over ${_selftest_regions} regions: "
    "${_selftest_plan} (${_tap_arms} together)")
endif()
