# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The own-image AMP partition's demo, one composition per node under
# systems/<board>/<variant>/, every node's build admitting them all together and linking its
# own node's. Every node preset builds both images; which one a node runs is decided where the
# partition is assembled (tools/amp/build-partition.sh), by node order.
#
# Sources stay .c: a C task entry over the plain C API is part of what this demonstrates.
if(KICKOS_AMP_NODES LESS 2)
  message(FATAL_ERROR
    "KickOS: KICKOS_AMP_NODES is ${KICKOS_AMP_NODES}, so this own-image partition names no "
    "peer at all. There is nothing to merge into an artefact and no crossing to exercise; "
    "docs/design-multicore.md N6c makes a partition of two the smallest useful one. State "
    "KICKOS_AMP_NODES=2 or more in boards/<board>/configs/<variant>/defconfig.")
endif()

set(_ampping_systems "${CMAKE_CURRENT_SOURCE_DIR}/systems/${KICKOS_BOARD}/${KICKOS_CONFIG_VARIANT}")
set(_ampping_nodes "")
math(EXPR _amp_last_node "${KICKOS_AMP_NODES} - 1")
foreach(_amp_node RANGE 0 ${_amp_last_node})
  list(APPEND _ampping_nodes "${_ampping_systems}/node${_amp_node}.yaml")
endforeach()
# The partition is admitted against this build's manifest, which the root writes once every
# directory is read.
set_property(GLOBAL PROPERTY KICKOS_AMPPING_NODES "${_ampping_nodes}")
function(_ampping_compose)
  get_property(_nodes GLOBAL PROPERTY KICKOS_AMPPING_NODES)
  kickos_compose(ampping_system PARTITION ${_nodes})
endfunction()
cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL _ampping_compose)

# A board whose partition states a gate witnesses it from systems/<board>/<variant>/gate.c.
set(_ampping_gate gate_none.c)
if(EXISTS "${_ampping_systems}/gate.c")
  set(_ampping_gate "${_ampping_systems}/gate.c")
endif()
add_executable(ampping_n0 main_n0.c crossing.c "${_ampping_gate}")
add_executable(ampserve main_serve.c crossing.c "${_ampping_gate}")
foreach(_ampping ampping_n0 ampserve)
  target_include_directories(${_ampping} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${_ampping} PRIVATE KickOS::kickos ampping_system)
  set_target_properties(${_ampping} PROPERTIES KICKOS_APP_SYSTEM ampping_system)
  kickos_emit_image(${_ampping})
endforeach()

# The peer the SELFTEST talks to: it echoes the caller's own bytes, where ampserve's reply
# transforms the payload. Its node of the partition the selftest's node 0 runs in, every other
# node an ampecho too.
set(_ampecho_systems "${PROJECT_SOURCE_DIR}/user/apps/common/selftest/systems/${KICKOS_BOARD}/${KICKOS_CONFIG_VARIANT}")
set(_ampecho_partition "")
foreach(_amp_node RANGE 0 ${_amp_last_node})
  if(_amp_node EQUAL 0 AND NOT KICKOS_AMP_NODE_ID EQUAL 0)
    list(APPEND _ampecho_partition "${_ampecho_systems}/node0.yaml")
  else()
    list(APPEND _ampecho_partition "${_ampecho_systems}/echo${_amp_node}.yaml")
  endif()
endforeach()
add_executable(ampecho main_echo.c crossing.c)
target_include_directories(ampecho PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
kickos_app_system(ampecho PARTITION ${_ampecho_partition})
kickos_emit_image(ampecho)
# The task that never receives, the program ampecho_spare of the echo compositions' second task.
# A node whose composition names no spare still builds it.
add_executable(ampecho_spare main_spare.c)
target_link_libraries(ampecho_spare PRIVATE kickos)
kickos_program_link(ampecho_spare)

# One entry per peer node in node order, which is the order build-partition.sh assigns them.
# The same target repeats: what makes two peers different images is the node index their build
# directory is configured at, never the source.
set(_amp_peer_apps "")
set(_amp_peer_echoes "")
math(EXPR _amp_last_peer "${KICKOS_AMP_NODES} - 1")
foreach(_amp_peer RANGE 1 ${_amp_last_peer})
  list(APPEND _amp_peer_apps ampserve)
  list(APPEND _amp_peer_echoes ampecho)
endforeach()

# N6b: deployment is a MERGE and not a second flash, owned by node 0, so this target exists
# only in a node 0 build and one step produces the whole partition:
#
#   cmake --build build/qemu-arm64-amp2-n0 --target amp_partition
#
# Each artefact path is recorded on ampping_n0: the gate fragment that boots it reads the
# property rather than restating the path.
if(KICKOS_AMP_NODE_ID EQUAL 0)
  set(_amp_artefact "${CMAKE_BINARY_DIR}/kickos-partition.elf")
  add_custom_command(OUTPUT "${_amp_artefact}"
    COMMAND "${PROJECT_SOURCE_DIR}/tools/amp/build-partition.sh"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/amp-peers"
            "${CMAKE_OBJCOPY}" "${CMAKE_LINKER}" "${CMAKE_READELF}" "${CMAKE_OBJDUMP}"
            "${CMAKE_TOOLCHAIN_FILE}" "${CMAKE_BUILD_TYPE}"
            "${_amp_artefact}" "${KICKOS_BOARD}" "${KICKOS_CONFIG_VARIANT}"
            "$<TARGET_FILE:ampping_n0>" ${_amp_peer_apps}
    DEPENDS ampping_n0
    COMMENT "assembling the AMP partition into one artefact")
  add_custom_target(amp_partition DEPENDS "${_amp_artefact}")
  set_target_properties(ampping_n0 PROPERTIES KICKOS_AMP_ARTEFACT "${_amp_artefact}")
  # The judge of the partition's silicon capture, which no emulator runs on these boards.
  if(KICKOS_BOARD STREQUAL "esp32c6-wroom")
    string(REPLACE "," ";" _amp_slices "${KICKOS_AMP_SLICE_LIST}")
    list(GET _amp_slices 0 _amp_slice0)
    list(GET _amp_slices 1 _amp_slice1)
    math(EXPR _amp_n0 "${KICKOS_AMP_PARTITION_BASE}" OUTPUT_FORMAT HEXADECIMAL)
    math(EXPR _amp_n1 "${KICKOS_AMP_PARTITION_BASE} + ${_amp_slice0}" OUTPUT_FORMAT HEXADECIMAL)
    math(EXPR _amp_n1_end "${_amp_n1} + ${_amp_slice1}" OUTPUT_FORMAT HEXADECIMAL)
    kickos_app_judge(ampping_n0 tests/integration/check_c6_amp_capture.sh
                     ARGS ${_amp_n0} ${_amp_n1} ${_amp_n1_end})
  elseif(KICKOS_BOARD STREQUAL "pizero2350")
    kickos_app_judge(ampping_n0 tests/integration/check_pizero_amp_gate.sh)
  endif()

  # The flashers take an image by its base name.
  set(_amp_base "${CMAKE_BINARY_DIR}/kickos-partition")
  add_custom_command(OUTPUT "${_amp_base}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_amp_artefact}" "${_amp_base}"
    DEPENDS "${_amp_artefact}"
    VERBATIM)
  add_custom_target(amp_partition_base DEPENDS "${_amp_base}")
  add_dependencies(amp_partition amp_partition_base)

  # The merged ELF carries no symbol: node 0's own names its flash text. Where the ROM boots
  # esptool's format alone, a partition with no ROM image is not built.
  if(DEFINED KICKOS_CHIP_ESPTOOL_IMAGE)
    find_program(KICKOS_ESPTOOL NAMES esptool esptool.py)
    find_program(KICKOS_PYTHON3 NAMES python3 REQUIRED)
  endif()
  function(_ampping_esp_image elf node0 nodes tag)
    string(REGEX REPLACE "\\.elf$" ".app.bin" _flash "${elf}")
    if(KICKOS_ESPTOOL)
      add_custom_command(OUTPUT "${_flash}"
        COMMAND ${KICKOS_PYTHON3} -I ${KICKOS_ESP_IMAGE_TOOL} --symbols "${node0}"
                ${KICKOS_ESPTOOL} ${KICKOS_CHIP} ${CMAKE_NM} "${elf}" "${_flash}"
                ${KICKOS_CHIP_ESPTOOL_IMAGE}
        COMMAND sh "${PROJECT_SOURCE_DIR}/tests/integration/check_esp_segments.sh" --partition
                "${CMAKE_READELF}" "${CMAKE_NM}" "${elf}" "${_flash}" "${nodes}"
        DEPENDS "${elf}" "${KICKOS_ESP_IMAGE_TOOL}"
                "${PROJECT_SOURCE_DIR}/tests/integration/check_esp_segments.sh"
        COMMENT "packing every AMP node's LOAD segments into one ROM image"
        VERBATIM)
      add_custom_target(${tag} DEPENDS "${_flash}")
    else()
      list(JOIN KICKOS_CHIP_ESPTOOL_IMAGE " " _esp_options)
      get_filename_component(_name "${_flash}" NAME)
      add_custom_target(${tag}
        COMMAND ${CMAKE_COMMAND} -E echo "KickOS: esptool not found, so ${_name}, the only"
                "image the ${KICKOS_CHIP} ROM boots of this partition, cannot be built. Activate"
                "the esp-idf env, or run: python3 tools/esp-image.py --symbols ${node0} esptool"
                "${KICKOS_CHIP} nm ${elf} ${_flash} ${_esp_options}"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM)
    endif()
  endfunction()
  if(DEFINED KICKOS_CHIP_ESPTOOL_IMAGE)
    _ampping_esp_image("${_amp_artefact}" "$<TARGET_FILE:ampping_n0>"
                       "${CMAKE_BINARY_DIR}/amp-peers/nodes" esp_amp_partition_image)
    add_dependencies(amp_partition esp_amp_partition_image)
  endif()

  # The second artefact: node 0 is the selftest image on the kernel console that registers the
  # arms needing a running peer, and node 1 a peer that echoes, so those arms have one. They are
  # compiled on every posture and decide at runtime, so this changes the ANSWER they get and not
  # which of them exist. <build>/kickos-partition-selftest.node0 names that image.
  set(_amp_st_node0 "")
  get_property(_amp_st_images GLOBAL PROPERTY KICKOS_SELFTEST_IMAGES)
  foreach(_img IN LISTS _amp_st_images)
    get_target_property(_img_console ${_img} KICKOS_SELFTEST_CONSOLE)
    get_target_property(_img_names ${_img} KICKOS_SELFTEST_ARM_NAMES)
    if(_img_console STREQUAL "kernel" AND "amp_far_call" IN_LIST _img_names)
      set(_amp_st_node0 ${_img})
    endif()
  endforeach()
  if(TARGET selftest AND NOT _amp_st_node0)
    message(FATAL_ERROR "ampping: no selftest image on the kernel console registers amp_far_call, "
      "so the selftest partition would run no arm that needs its peer")
  endif()
  if(_amp_st_node0)
    file(WRITE "${CMAKE_BINARY_DIR}/kickos-partition-selftest.node0" "${_amp_st_node0}\n")
    set(_amp_st_artefact "${CMAKE_BINARY_DIR}/kickos-partition-selftest.elf")
    add_custom_command(OUTPUT "${_amp_st_artefact}"
      COMMAND "${PROJECT_SOURCE_DIR}/tools/amp/build-partition.sh"
              "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}" "${PROJECT_SOURCE_DIR}"
              "${CMAKE_BINARY_DIR}/amp-peers-st"
              "${CMAKE_OBJCOPY}" "${CMAKE_LINKER}" "${CMAKE_READELF}" "${CMAKE_OBJDUMP}"
              "${CMAKE_TOOLCHAIN_FILE}" "${CMAKE_BUILD_TYPE}"
              "${_amp_st_artefact}" "${KICKOS_BOARD}" "${KICKOS_CONFIG_VARIANT}"
              "$<TARGET_FILE:${_amp_st_node0}>" ${_amp_peer_echoes}
      DEPENDS ${_amp_st_node0}
      COMMENT "assembling the AMP partition with ${_amp_st_node0} as node 0")
    add_custom_target(amp_partition_selftest DEPENDS "${_amp_st_artefact}")
    set(_amp_st_base "${CMAKE_BINARY_DIR}/kickos-partition-selftest")
    add_custom_command(OUTPUT "${_amp_st_base}"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_amp_st_artefact}" "${_amp_st_base}"
      DEPENDS "${_amp_st_artefact}"
      VERBATIM)
    add_custom_target(amp_partition_selftest_base DEPENDS "${_amp_st_base}")
    add_dependencies(amp_partition_selftest amp_partition_selftest_base)
    if(DEFINED KICKOS_CHIP_ESPTOOL_IMAGE)
      _ampping_esp_image("${_amp_st_artefact}" "$<TARGET_FILE:${_amp_st_node0}>"
                         "${CMAKE_BINARY_DIR}/amp-peers-st/nodes" esp_amp_partition_selftest_image)
      add_dependencies(amp_partition_selftest esp_amp_partition_selftest_image)
    endif()
    set_target_properties(ampping_n0 PROPERTIES
      KICKOS_AMP_ARTEFACT_SELFTEST "${_amp_st_artefact}")
  endif()
endif()
