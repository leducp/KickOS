# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_add_diagnostic_apps(<dir>...)
#   A DIAGNOSTIC app is a test image that deliberately faults or provokes a refusal, so it is
#   built ONLY under KICKOS_ENABLE_SELFTEST. The posture
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
# kickos_app_system(<app> PARTITION <node0> <node1>... [SYSTEM <system>])
#   Links <app> against KickOS::kernel and the system composed from <file>, or from this build's
#   node of the partition the node compositions state, each relative to the app's sources.
#   Images naming one SYSTEM share it. The root CMakeLists.txt composes every system once the
#   manifest is written, which is after this tree.
function(kickos_app_system app)
  cmake_parse_arguments(KAS "" "SYSTEM;COMPOSITION" "PARTITION" ${ARGN})
  if(KAS_UNPARSED_ARGUMENTS OR (NOT KAS_COMPOSITION AND NOT KAS_PARTITION)
     OR (KAS_COMPOSITION AND KAS_PARTITION))
    message(FATAL_ERROR "kickos_app_system(${app}): takes COMPOSITION <file> or PARTITION <node0> "
      "<node1>..., then [SYSTEM <system>], not '${ARGN}'")
  endif()
  set(_system ${app}_system)
  if(KAS_SYSTEM)
    set(_system ${KAS_SYSTEM})
  endif()
  set(_composition "")
  foreach(_named IN LISTS KAS_COMPOSITION KAS_PARTITION)
    get_filename_component(_one "${_named}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_one}")
      message(FATAL_ERROR "kickos_app_system(${app}): no composition at ${_one}")
    endif()
    list(APPEND _composition "${_one}")
  endforeach()
  if(KAS_PARTITION)
    list(PREPEND _composition PARTITION)
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
  set_target_properties(${app} PROPERTIES KICKOS_APP_SYSTEM ${_system})
endfunction()

# kickos_app_judge(<image> <judge> [ARGS <arg>...])
#   Names the gate script, relative to the tree, that judges a silicon capture of <image> as
#   tools/bench/bench.sh's JUDGE, and the arguments it takes after `<build> <tree> cmake`. The
#   judge and its arguments are the third and fourth fields of the image's row in
#   kickos-images.txt, which `LIST_IMAGES=1 tools/bench/bench.sh <board>` prints.
function(kickos_app_judge image judge)
  cmake_parse_arguments(KAJ "" "" "ARGS" ${ARGN})
  if(KAJ_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "kickos_app_judge(${image}): unexpected ${KAJ_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT TARGET ${image})
    message(FATAL_ERROR "kickos_app_judge(${image}): no such image")
  endif()
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${judge}")
    message(FATAL_ERROR "kickos_app_judge(${image}): no gate script at ${judge}")
  endif()
  get_target_property(_judge ${image} KICKOS_IMAGE_JUDGE)
  if(_judge)
    message(FATAL_ERROR "kickos_app_judge(${image}): already ${_judge}")
  endif()
  set_target_properties(${image} PROPERTIES KICKOS_IMAGE_JUDGE "${judge}"
                                            KICKOS_IMAGE_JUDGE_ARGS "${KAJ_ARGS}")
endfunction()

# kickos_human_judged(<image> <what a person reads>)
#   States that <image>'s only verdict is something a person reads and no capture carries, an
#   LED say. The image listing reads `human` with <what> as its argument, which the bench reports
#   as owed.
function(kickos_human_judged image what)
  if(NOT TARGET ${image})
    message(FATAL_ERROR "kickos_human_judged(${image}): no such image")
  endif()
  get_target_property(_judge ${image} KICKOS_IMAGE_JUDGE)
  if(_judge)
    message(FATAL_ERROR "kickos_human_judged(${image}): ${_judge} already judges it")
  endif()
  set_target_properties(${image} PROPERTIES KICKOS_IMAGE_JUDGE human
                                            KICKOS_IMAGE_JUDGE_ARGS "${what}")
endfunction()

# This tree's posture for its own apps, which an out-of-tree app does not get. gcc 15 defaults
# to gnu23, which accepts bool, static_assert, alignas and nullptr, so an unpinned C app stops
# witnessing the C contract it exists for.
add_compile_options(${KICKOS_WARN_FLAGS})
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# <kickos/sys.h> poisons the raw kernel console write in every app TU it compiles. This refuses
# it in the arms and the sources no configuration compiles.
file(GLOB_RECURSE _kos_app_sources "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cc" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
foreach(_kos_src IN LISTS _kos_app_sources)
  file(STRINGS "${_kos_src}" _kos_raw
       REGEX "(^|[^A-Za-z0-9_])(kos_kconsole_write *\\(|KOS_SYS_KCONSOLE_WRITE)")
  if(_kos_raw)
    message(FATAL_ERROR "KickOS: ${_kos_src} writes the kernel console raw: '${_kos_raw}'. Write "
      "through kos_print or stdio, or state what of the write is measured: "
      "KICKOS_KCONSOLE_MEASURED(DROP|ANSWER|WAIT, buf, len) (<kickos/sys.h>).")
  endif()
endforeach()

# Board-agnostic and multi-board apps.
add_subdirectory(common)

# Board-specific apps: the directory IS the board gate. Boards without a
# per-board app subdir (sim, qemu, microbit, qemu-riscv, picopi, blackpill,
# due, ...) simply skip; add_subdirectory on a missing dir is a hard error.
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_BOARD}")
  add_subdirectory("${KICKOS_BOARD}")
endif()
