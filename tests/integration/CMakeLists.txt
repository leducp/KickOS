# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Every in-tree CTest entry that rides an APP TARGET: a boot gate under an emulator, a run on
# the host sim, or a reader of a linked ELF. An application is not a test, so an app file
# declares its target and stops; the entries live here, beside the scripts they run.
#
# Added by the root CMakeLists.txt AFTER add_subdirectory(user/apps), which is what makes
# `if(TARGET <app>)` below the real answer rather than a guess: the parent that selects an app
# has already decided, so a fragment reads the decision instead of restating its condition.
#
# include() and not add_subdirectory(): ONE directory scope, so the root's add_test wrapper
# arms its decline pass once and reads the `host` label every site here sets, and a fragment
# can read a figure an earlier one computed.
#
# Two board groupings recur, and both are arch/chip facts rather than lists to maintain:
#   the four QEMU MPS2 FPGA images (an386/M4F, an505/M33, an500/M7, an385/M3) are exactly the
#     boards QEMU runs as an MPS2 machine, KICKOS_QEMU_MPS2, which the root derives from
#     kickos_qemu_machine;
#   the two emulatable arm64 boards (qemu-arm64, imx8mp-evk) are exactly the armv8a fleet,
#     KICKOS_ARCH STREQUAL "armv8a".
#
# NO board name conditions an emulator gate: kickos_add_qemu_test is total and registers nothing
# where there is no emulator, and a predicate states the fact the CLAIM rests on (an arch, a
# Kconfig symbol, the machine, an image). tests/static/check_board_predicates.py refuses one.

# kickos_emulator_judged(<image>...)
#   States that an emulator gate holds <image>'s verdict and no capture can: an exit status, a
#   panic, a reboot or a deliberate fault. The image listing then reads `emulator` where a test
#   this build runs names it, and `emulator-owed` where none does, which the bench reports as
#   owed.
function(kickos_emulator_judged)
  foreach(_image IN LISTS ARGN)
    if(NOT TARGET ${_image})
      message(FATAL_ERROR "kickos_emulator_judged(${_image}): no such image")
    endif()
    get_target_property(_judge ${_image} KICKOS_IMAGE_JUDGE)
    if(_judge)
      message(FATAL_ERROR "kickos_emulator_judged(${_image}): ${_judge} already judges its capture")
    endif()
    set_target_properties(${_image} PROPERTIES KICKOS_IMAGE_JUDGE emulator)
  endforeach()
endfunction()

# kickos_inapplicable(<image> <why>)
#   States that this build's posture voids <image>'s claim, so nothing can witness it here. The
#   image listing reads `inapplicable` with <why> as its argument.
function(kickos_inapplicable image why)
  if(NOT TARGET ${image})
    message(FATAL_ERROR "kickos_inapplicable(${image}): no such image")
  endif()
  get_target_property(_judge ${image} KICKOS_IMAGE_JUDGE)
  if(_judge)
    message(FATAL_ERROR "kickos_inapplicable(${image}): already ${_judge}")
  endif()
  set_target_properties(${image} PROPERTIES KICKOS_IMAGE_JUDGE inapplicable
                                            KICKOS_IMAGE_JUDGE_ARGS "${why}")
endfunction()

# kickos_unbooted(<image> <why>)
#   States why no test of this build boots <image>, which the root's image listing otherwise
#   refuses where the build boots its images: a test that boots it as a tree of its own builds
#   it, or a run that measures rather than judges.
function(kickos_unbooted image why)
  if(NOT TARGET ${image})
    message(FATAL_ERROR "kickos_unbooted(${image}): no such image")
  endif()
  set_target_properties(${image} PROPERTIES KICKOS_IMAGE_UNBOOTED "${why}")
endfunction()

# The board name as a CTest-name prefix, for the gates whose ctest name is the claim rather
# than the image and so cannot take kickos_add_qemu_test's derived name.
string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")

# EVERY fragment in gates/, found rather than listed: a name list is a second place to edit
# and a fragment missing from it registers nothing while the configure stays green.
# CONFIGURE_DEPENDS is what makes an added or deleted fragment re-run this configure instead of
# being picked up at the next unrelated one.
# SORTED, because a fragment may read a figure an earlier one computed and the glob's own order
# is the filesystem's.
file(GLOB _kickos_gates CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gates/*.cmake")
list(SORT _kickos_gates)
if(NOT _kickos_gates)
  message(FATAL_ERROR "no fragment in ${CMAKE_CURRENT_LIST_DIR}/gates/, so this directory "
                      "would register no integration gate at all")
endif()
foreach(_gate IN LISTS _kickos_gates)
  include("${_gate}")
endforeach()

add_test(NAME ${_tag}_image_listing
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_image_listing.sh"
          "${PROJECT_BINARY_DIR}/kickos-images.txt" "${PROJECT_SOURCE_DIR}" "${CMAKE_CTEST_COMMAND}"
          "${PROJECT_BINARY_DIR}" "${CMAKE_EXECUTABLE_SUFFIX_CXX}")
kickos_host_gate(${_tag}_image_listing TIMEOUT 30)
add_test(NAME ${_tag}_host_gate_boots
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_host_gate_boots.sh" "${PROJECT_SOURCE_DIR}"
          "${CMAKE_CTEST_COMMAND}" "${PROJECT_BINARY_DIR}")
kickos_host_gate(${_tag}_host_gate_boots TIMEOUT 30)
add_test(NAME ${_tag}_image_judges_proved
  COMMAND sh "${PROJECT_SOURCE_DIR}/tests/static/check_app_judges.sh" --listing
          "${PROJECT_BINARY_DIR}/kickos-images.txt")
kickos_host_gate(${_tag}_image_judges_proved TIMEOUT 30)

# KICKOS_RUN_IMAGES: each image a test this build runs names, the test being neither a host gate
# nor declined, which the image listing reads. Deferred behind every site's set_tests_properties
# and the decline pass.
function(_kickos_mark_run_images)
  get_property(_runs DIRECTORY PROPERTY kickos_image_tests)
  foreach(_run IN LISTS _runs)
    string(REPLACE "|" ";" _run "${_run}")
    list(GET _run 0 _run_image)
    list(GET _run 1 _run_test)
    get_test_property("${_run_test}" LABELS _run_labels)
    get_test_property("${_run_test}" DISABLED _run_disabled)
    if(NOT "host" IN_LIST _run_labels AND NOT _run_disabled)
      set_property(GLOBAL APPEND PROPERTY KICKOS_RUN_IMAGES "${_run_image}")
    endif()
  endforeach()
endfunction()
cmake_language(DEFER CALL _kickos_mark_run_images)
