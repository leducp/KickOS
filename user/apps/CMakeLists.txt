# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_add_diagnostic_apps(<dir>...)
#   A DIAGNOSTIC app depends on the test-only syscall surface kept out of the production ABI,
#   and/or deliberately faults, so it is built ONLY under KICKOS_ENABLE_SELFTEST. The posture
#   is named HERE, where the tree decides what applies, and not inside the app: that is what
#   lets an app file be its three lines and never ask whether it applies at all.
#
#   Defined in this file rather than in common/ because the per-board parents below need it
#   too, and a command defined in a subdirectory that happens to be added first is a coupling
#   nothing states.
macro(kickos_add_diagnostic_apps)
  if(KICKOS_ENABLE_SELFTEST)
    foreach(_kos_diag_dir ${ARGN})
      add_subdirectory(${_kos_diag_dir})
    endforeach()
  endif()
endmacro()

# kickos_app_system(<app> COMPOSITION <file> [SYSTEM <system>])
#   Links <app> against KickOS::kernel and the system composed from <file>, relative to the
#   app's sources. Images naming one SYSTEM share it. The root CMakeLists.txt composes every
#   system once the manifest is written, which is after this tree.
function(kickos_app_system app)
  cmake_parse_arguments(KAS "" "SYSTEM;COMPOSITION" "" ${ARGN})
  if(KAS_UNPARSED_ARGUMENTS OR NOT KAS_COMPOSITION)
    message(FATAL_ERROR "kickos_app_system(${app}): takes COMPOSITION <file> [SYSTEM <system>], not "
      "'${ARGN}'")
  endif()
  set(_system ${app}_system)
  if(KAS_SYSTEM)
    set(_system ${KAS_SYSTEM})
  endif()
  get_filename_component(_composition "${KAS_COMPOSITION}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  if(NOT EXISTS "${_composition}")
    message(FATAL_ERROR "kickos_app_system(${app}): no composition at ${_composition}")
  endif()
  get_property(_known GLOBAL PROPERTY KICKOS_APP_SYSTEMS)
  if(_system IN_LIST _known)
    get_property(_was GLOBAL PROPERTY KICKOS_APP_SYSTEM_${_system})
    if(NOT _was STREQUAL _composition)
      message(FATAL_ERROR "kickos_app_system(${app}): the system ${_system} is ${_was} already, "
        "not ${_composition}")
    endif()
  else()
    set_property(GLOBAL APPEND PROPERTY KICKOS_APP_SYSTEMS ${_system})
    set_property(GLOBAL PROPERTY KICKOS_APP_SYSTEM_${_system} "${_composition}")
  endif()
  target_link_libraries(${app} PRIVATE KickOS::kernel ${_system})
endfunction()

# kickos_app_judge(<image> <judge>)
#   Names the gate script, relative to the tree, that judges a silicon capture of <image> as
#   tools/bench/bench.sh's JUDGE. Configure writes one `<image>|<judge>` row per call to
#   kickos-app-manifest.txt, which `LIST_APPS=1 tools/bench/bench.sh <board>` prints.
function(kickos_app_judge image judge)
  if(NOT TARGET ${image})
    message(FATAL_ERROR "kickos_app_judge(${image}): no such image")
  endif()
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${judge}")
    message(FATAL_ERROR "kickos_app_judge(${image}): no gate script at ${judge}")
  endif()
  set_property(GLOBAL APPEND PROPERTY KICKOS_APP_JUDGES "${image}|${judge}")
endfunction()

# This tree's posture for its own apps, which an out-of-tree app does not get. gcc 15 defaults
# to gnu23, which accepts bool, static_assert, alignas and nullptr, so an unpinned C app stops
# witnessing the C contract it exists for.
add_compile_options(${KICKOS_WARN_FLAGS})
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Board-agnostic and multi-board apps.
add_subdirectory(common)

# Board-specific apps: the directory IS the board gate. Boards without a
# per-board app subdir (sim, qemu, microbit, qemu-riscv, picopi, blackpill,
# due, ...) simply skip; add_subdirectory on a missing dir is a hard error.
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_BOARD}")
  add_subdirectory("${KICKOS_BOARD}")
endif()

get_property(_kos_app_judges GLOBAL PROPERTY KICKOS_APP_JUDGES)
set(_kos_app_rows "")
foreach(_kos_row IN LISTS _kos_app_judges)
  string(APPEND _kos_app_rows "${_kos_row}\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/kickos-app-manifest.txt" CONTENT "${_kos_app_rows}" @ONLY)
