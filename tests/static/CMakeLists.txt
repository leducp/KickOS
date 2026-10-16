# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The static gates. Each registers through _add_test, so none requires the build fixture.
# A `tree` gate reads only the source tree; the host gates at the end read this configure's
# own output. A script run bare expects the repository root as its working directory.

kickos_arm_timeouts()

function(kickos_tree_scripts)
  cmake_parse_arguments(TS "" "TIMEOUT" "" ${ARGN})
  foreach(_name IN LISTS TS_UNPARSED_ARGUMENTS)
    _add_test(NAME ${_name} COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_${_name}.sh"
              WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  endforeach()
  kickos_tree_gate(${TS_UNPARSED_ARGUMENTS} TIMEOUT ${TS_TIMEOUT})
endfunction()

kickos_tree_scripts(doc_names extern_c_linkage ascii spdx include_guards shell_special_names
                    ternary panic_banners syscall_return_codes death_stack_seating
                    panic_stack_seat mpu_record park_death_point geometry_emitted atomic_rmw
                    amp_slot_snapshot ci_named_lists awk_portable poll_stop_lines kernel_core_index)
# dash_punct, the heaviest corpus scan, has taken 117 s on a contended box.
kickos_tree_scripts(dash_punct amp_no_xip_pin TIMEOUT 300)
# app_judges runs one judge per planted capture, and has taken 46 s at load 22.
kickos_tree_scripts(app_judges TIMEOUT 300)
kickos_tree_scripts(tap_stream_sets capture_teardown amp_peers sweep_checkout rig_shells
                    TIMEOUT 60)
set_tests_properties(rig_shells PROPERTIES SKIP_RETURN_CODE 77)

_add_test(NAME bench_banner_controls COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_bench_banner.sh"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
_add_test(NAME stlink_capture_controls
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_stlink_capture.sh"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
_add_test(NAME selftest_demands_controls
          COMMAND python3 -B "${CMAKE_CURRENT_SOURCE_DIR}/selftest_demands.py" --self-test
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
_add_test(NAME boot_tp_controls
          COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/check_boot_tp.py" --self-test
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
file(GLOB _kos_gate_fragments "${PROJECT_SOURCE_DIR}/tests/integration/gates/*.cmake")
_add_test(NAME board_predicates
          COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/check_board_predicates.py"
                  ${_kos_gate_fragments} "${PROJECT_SOURCE_DIR}/user/apps/common/CMakeLists.txt"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
_add_test(NAME stamp_lines COMMAND python3 -B "${PROJECT_SOURCE_DIR}/tools/bench/test_stamp_lines.py")
foreach(_check no_eh_runtime no_libnosys ctors_kept image_listing)
  _add_test(NAME ${_check}_controls
            COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_${_check}.sh" --self-test)
endforeach()
_add_test(NAME string_provider_controls
          COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_string_provider.sh"
                  --self-test "${CMAKE_C_COMPILER}" "${CMAKE_AR}" "${CMAKE_NM}")
kickos_tree_gate(bench_banner_controls stlink_capture_controls selftest_demands_controls
                 boot_tp_controls board_predicates stamp_lines no_eh_runtime_controls
                 no_libnosys_controls ctors_kept_controls image_listing_controls
                 string_provider_controls TIMEOUT 60)

_add_test(NAME trap_redzone_walk
          COMMAND python3 -B "${CMAKE_CURRENT_SOURCE_DIR}/test_trap_redzone.py")
_add_test(NAME app_stack_control
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_app_stack.sh" "${CMAKE_CXX_COMPILER}"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
_add_test(NAME gate_lib COMMAND "${PROJECT_SOURCE_DIR}/tests/lib/check_gate.sh" 120
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
_add_test(NAME bench_fleet_controls
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_bench_fleet.sh" "${CMAKE_COMMAND}"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
_add_test(NAME preset_defconfig
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_preset_defconfig.sh"
                  "${CMAKE_COMMAND}" "${PROJECT_SOURCE_DIR}")
_add_test(NAME amp_share
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_amp_share.sh"
                  "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
_add_test(NAME driver_metadata
          COMMAND "${CMAKE_COMMAND}" "-DKICKOS_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                  -P "${CMAKE_CURRENT_SOURCE_DIR}/check_driver_metadata.cmake")
_add_test(NAME qemu_cpu
          COMMAND "${CMAKE_COMMAND}" "-DKICKOS_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                  -P "${CMAKE_CURRENT_SOURCE_DIR}/check_qemu_cpu.cmake")
kickos_tree_gate(trap_redzone_walk app_stack_control gate_lib bench_fleet_controls
                 preset_defconfig amp_share driver_metadata qemu_cpu)

_add_test(NAME chip_protection
          COMMAND "${CMAKE_COMMAND}" "-DKICKOS_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                  "-DKICKOS_SCRATCH=${PROJECT_BINARY_DIR}/chip_protection"
                  -P "${CMAKE_CURRENT_SOURCE_DIR}/check_chip_protection.cmake")
_add_test(NAME selftest_partials
          COMMAND "${CMAKE_COMMAND}" "-DKICKOS_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                  "-DSCRATCH=${PROJECT_BINARY_DIR}/selftest_partials"
                  -P "${CMAKE_CURRENT_SOURCE_DIR}/check_selftest_partials.cmake")
kickos_tree_gate(chip_protection selftest_partials TIMEOUT 60)

kickos_find_kconfig(_kconfig_py _kconfig_found)
_add_test(NAME kconfig_gen
          COMMAND "${_kconfig_py}" -B "${PROJECT_SOURCE_DIR}/tools/kconfig/test_genconfig.py"
                  "${CMAKE_COMMAND}")
_add_test(NAME chip_kconfig
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_chip_kconfig.sh"
                  "${_kconfig_py}" "${PROJECT_BINARY_DIR}"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
kickos_tree_gate(kconfig_gen chip_kconfig TIMEOUT 300)

# A first run fetches the compose tool's locked dependencies into the build directory.
foreach(_mode descriptions compositions arms coverage)
  _add_test(NAME platform_${_mode}
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_platform.sh" "${PROJECT_BINARY_DIR}" ${_mode}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  kickos_tree_gate(platform_${_mode} TIMEOUT 300)
endforeach()
# The walker runs, so the compiler must be the host's, which only the sim builds with.
if(KICKOS_ARCH STREQUAL "sim")
  _add_test(NAME platform_tables
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_platform.sh" "${PROJECT_BINARY_DIR}" tables
                    "${CMAKE_C_COMPILER}" "${PROJECT_BINARY_DIR}/generated/include"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  kickos_tree_gate(platform_tables TIMEOUT 300)
  _add_test(NAME usb_identity_controls
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_usb_identity.sh" "${CMAKE_CXX_COMPILER}"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  kickos_tree_gate(usb_identity_controls)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/check_isolated_cores.cmake")

# Every preset of an arch with a trap-stack header measures its trap descent, and a translating
# one walks the fault-record console route as a clause of it.
file(GLOB _trap_stack_header
     "${PROJECT_SOURCE_DIR}/arch/*/${KICKOS_ARCH}/include/kickos/arch/*_trap_stack.h")
if(_trap_stack_header)
  _add_test(NAME trap_redzone
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_trap_redzone.sh" "${PROJECT_SOURCE_DIR}"
                    "${CMAKE_COMMAND}" "${_fs_preset_key}" "${KICKOS_ARCH}")
  kickos_host_gate(trap_redzone TIMEOUT 1800)
  # The callgraph scratch tree must use the same explicit comparison profile.
  if(KICKOS_FULL_NEWLIB)
    set_tests_properties(trap_redzone PROPERTIES ENVIRONMENT "KICKOS_FULL_NEWLIB=ON")
  endif()
  if(KICKOS_HAVE_ASPACE)
    set_property(TEST trap_redzone APPEND PROPERTY ENVIRONMENT "KICKOS_CONSOLE_REACH=1")
  endif()
endif()

# Several arch directories provide the same kickos/arch/context.h, so the gate takes the one
# this board builds.
if(NOT CMAKE_C_COMPILER)
  message(FATAL_ERROR "No CMAKE_C_COMPILER, so the C-facing headers cannot be compiled as C "
                      "and check_public_headers.sh --tree would have nothing to run. project() "
                      "declares LANGUAGES C; a toolchain file that unsets the C compiler must "
                      "be fixed.")
endif()
set(_c_hdr_roots "${PROJECT_BINARY_DIR}/generated/include")
foreach(_d KICKOS_ARCH_INCLUDE_DIR KICKOS_CHIP_INCLUDE_DIR KICKOS_BOARD_INCLUDE_DIR)
  if(${_d})
    list(APPEND _c_hdr_roots "${${_d}}")
  endif()
endforeach()
_add_test(NAME c_headers
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_public_headers.sh" --tree
                  "${CMAKE_C_COMPILER}" ${_c_hdr_roots}
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
kickos_host_gate(c_headers)

set(_manifest "${PROJECT_BINARY_DIR}/export/manifest.yaml")
_add_test(NAME platform_manifest
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_platform.sh" "${PROJECT_BINARY_DIR}" manifest
                  "${_manifest}"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
# Each packaged driver of this build's catalogue in a composition the board admits.
_add_test(NAME platform_drivers
          COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_platform.sh" "${PROJECT_BINARY_DIR}" drivers
                  "${_manifest}" "${KICKOS_BOARD}"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
kickos_host_gate(platform_manifest platform_drivers TIMEOUT 300)
set(KICKOS_GOLDEN_COMPOSITION "" CACHE STRING
    "A golden composition this build targets, relative to the source root, admitted against its manifest")
if(NOT KICKOS_GOLDEN_COMPOSITION STREQUAL "")
  set(_golden "${PROJECT_SOURCE_DIR}/${KICKOS_GOLDEN_COMPOSITION}")
  _add_test(NAME platform_golden
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_platform.sh" "${PROJECT_BINARY_DIR}" golden
                    "${_manifest}" "${_golden}"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  _add_test(NAME platform_golden_table
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_platform.sh" "${PROJECT_BINARY_DIR}" table
                    "${_manifest}" "${_golden}" "${CMAKE_C_COMPILER}"
                    "${PROJECT_BINARY_DIR}/generated/include"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  kickos_host_gate(platform_golden platform_golden_table TIMEOUT 300)
endif()

# A gate stating no working directory runs in the build root.
get_property(_tests DIRECTORY PROPERTY TESTS)
foreach(_test IN LISTS _tests)
  get_test_property(${_test} WORKING_DIRECTORY _wd)
  if(NOT _wd)
    set_tests_properties(${_test} PROPERTIES WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
  endif()
endforeach()
