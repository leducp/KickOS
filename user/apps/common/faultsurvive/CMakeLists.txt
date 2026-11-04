# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# faultsurvive_image(<target> <mode> [<definition>...])
#   One image of the family: main.cc as <target>, and its faulter peer.cc as <target>_peer, both
#   built for fault mode <mode>.
function(faultsurvive_image target mode)
  add_executable(${target} main.cc)
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
  target_compile_definitions(${target} PRIVATE KICKOS_FS_MODE=${mode} ${ARGN}
                             "KICKOS_FS_PEER=\"${target}_peer\"")
  add_executable(${target}_peer peer.cc)
  target_include_directories(${target}_peer PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
  target_link_libraries(${target}_peer PRIVATE kickos)
  target_compile_definitions(${target}_peer PRIVATE KICKOS_FS_MODE=${mode} ${ARGN})
  set(FS_APP ${target})
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/system.yaml.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${target}.yaml" @ONLY)
  kickos_app_system(${target} COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/${target}.yaml")
  kickos_emit_image(${target})
endfunction()

faultsurvive_image(faultsurvive 0)

if(NOT KICKOS_ARCH STREQUAL "sim")
  faultsurvive_image(faultsurvive_ovf 1)
endif()
# The published console of the sim is simcon's, which the composition names.
if(KICKOS_ARCH STREQUAL "sim")
  add_executable(faultsurvive_published main.cc)
  target_include_directories(faultsurvive_published PRIVATE
                             "${PROJECT_SOURCE_DIR}/user/apps/common/include")
  target_compile_definitions(faultsurvive_published PRIVATE KICKOS_FS_MODE=0
                             "KICKOS_FS_PEER=\"faultsurvive_published_peer\"")
  add_executable(faultsurvive_published_peer peer.cc)
  target_include_directories(faultsurvive_published_peer PRIVATE
                             "${PROJECT_SOURCE_DIR}/user/apps/common/include")
  target_link_libraries(faultsurvive_published_peer PRIVATE kickos)
  target_compile_definitions(faultsurvive_published_peer PRIVATE KICKOS_FS_MODE=0)
  set(FS_APP faultsurvive_published)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/system_published.yaml.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/faultsurvive_published.yaml" @ONLY)
  kickos_app_system(faultsurvive_published
                    COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/faultsurvive_published.yaml")
  kickos_emit_image(faultsurvive_published)
endif()

if(NOT (KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "x86_64"))
  faultsurvive_image(faultsurvive_off 2)
endif()

if(KICKOS_ARCH STREQUAL "rv32imac" OR KICKOS_ARCH STREQUAL "rxv3")
  faultsurvive_image(faultsurvive_kwrite 3)
  # Mode 5 drops SP two bytes: in bounds, deep inside the stack, and misaligned, so only the
  # prologue's alignment leg can refuse it.
  faultsurvive_image(faultsurvive_misalign 5)
endif()

if(KICKOS_ARCH STREQUAL "rv32imac")
  faultsurvive_image(faultsurvive_lowedge 4)
endif()

if(KICKOS_ARCH STREQUAL "rv32imac")
  foreach(_fs_tgt faultsurvive_kwrite faultsurvive_lowedge)
    if(TARGET ${_fs_tgt}_peer)
      target_include_directories(${_fs_tgt}_peer PRIVATE
        "${PROJECT_SOURCE_DIR}/arch/riscv/rv32imac/include")
    endif()
  endforeach()
endif()

if(KICKOS_HAVE_STACK_LIMIT AND KICKOS_MEMORY_ENFORCED)
  faultsurvive_image(faultsurvive_ovfmpu 7)
endif()
if(KICKOS_ARCH STREQUAL "armv7m" AND KICKOS_QEMU_MPS2 AND KICKOS_MEMORY_ENFORCED)
  faultsurvive_image(faultsurvive_unread 6 KICKOS_FS_NOWHERE=0xF0000000u)
endif()
