# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ampcon on one kernel, or as the qemu-arm64 amp3 partition: node 0 relays node 1's standard
# output onto its console, and node 2 drives a UART of its own. Every node's build admits the three
# compositions together and links its own node's.
add_executable(ampcon main.cc)
if(NOT KICKOS_AMP_NODE)
  kickos_app_system(ampcon COMPOSITION system.yaml)
  kickos_emit_image(ampcon)
  return()
endif()

set(_ampcon_systems "${CMAKE_CURRENT_SOURCE_DIR}/systems/${KICKOS_BOARD}/${KICKOS_CONFIG_VARIANT}")
set(_ampcon_nodes "")
math(EXPR _ampcon_last "${KICKOS_AMP_NODES} - 1")
foreach(_node RANGE 0 ${_ampcon_last})
  list(APPEND _ampcon_nodes "${_ampcon_systems}/node${_node}.yaml")
endforeach()
add_executable(ampcon_n0 main_n0.c)
foreach(_image ampcon ampcon_n0)
  kickos_app_system(${_image} PARTITION ${_ampcon_nodes} SYSTEM ampcon_system)
  kickos_emit_image(${_image})
endforeach()
if(NOT KICKOS_AMP_NODE_ID EQUAL 0)
  return()
endif()

# Node 0 assembles the partition, node 1 and node 2 each running the copy of ampcon a peer tree of
# its own builds.
set(_ampcon_peers "")
foreach(_node RANGE 1 ${_ampcon_last})
  list(APPEND _ampcon_peers ampcon)
endforeach()
set(_ampcon_artefact "${CMAKE_BINARY_DIR}/kickos-partition-ampcon.elf")
add_custom_command(OUTPUT "${_ampcon_artefact}"
  COMMAND "${PROJECT_SOURCE_DIR}/tools/amp/build-partition.sh"
          "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}" "${PROJECT_SOURCE_DIR}"
          "${CMAKE_BINARY_DIR}/amp-peers-con"
          "${CMAKE_OBJCOPY}" "${CMAKE_LINKER}" "${CMAKE_READELF}" "${CMAKE_OBJDUMP}"
          "${CMAKE_TOOLCHAIN_FILE}" "${CMAKE_BUILD_TYPE}"
          "${_ampcon_artefact}" "${KICKOS_BOARD}" "${KICKOS_CONFIG_VARIANT}"
          "$<TARGET_FILE:ampcon_n0>" ${_ampcon_peers}
  DEPENDS ampcon_n0
  COMMENT "assembling the ampcon partition")
add_custom_target(amp_partition_ampcon DEPENDS "${_ampcon_artefact}")
set_target_properties(ampcon_n0 PROPERTIES KICKOS_AMP_ARTEFACT "${_ampcon_artefact}")
