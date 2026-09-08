# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The own-image AMP partition's demo: one root per node, each its own image with its own init
# provider, service list and app, talking over the shared window.
#
# Every target here is built by EVERY node preset; which one belongs to which node is decided
# where the partition is assembled (tools/amp/merge-partition.sh), by node order. Neither
# serving source names a node index, each deriving the port it binds from the partition's list
# and its own KOS_AMP_SELF_NODE, so one serving app covers every peer at any width.
#
# Sources stay .c: the consumer posture is a plain add_executable linking `kickos` with a pure
# C main, and a C++ file would quietly stop demonstrating it.
kickos_add_app_target(ampping_n0 main_n0.c)
target_link_libraries(ampping_n0 PRIVATE kickos)
kickos_emit_image(ampping_n0)

kickos_add_app_target(ampserve main_serve.c)
target_link_libraries(ampserve PRIVATE kickos)
kickos_emit_image(ampserve)
# One CONCURRENTLY parked far caller: the peer's call is held on this node's table while the
# service thread answers it.
kickos_declare_app_capabilities(ampserve ${KICKOS_CAP_APP_PEAK_DEFAULT} 0 1)

# The peer the SELFTEST talks to: it echoes the caller's own bytes, where ampserve's reply
# transforms the payload.
kickos_add_app_target(ampecho main_echo.c)
target_link_libraries(ampecho PRIVATE kickos)
kickos_emit_image(ampecho)
kickos_declare_app_capabilities(ampecho ${KICKOS_CAP_APP_PEAK_DEFAULT} 0 1)

# --- What this demo serves, at whatever width the partition states -------------------------
if(KICKOS_AMP_NODES LESS 2)
  message(FATAL_ERROR
    "KickOS: KICKOS_AMP_NODES is ${KICKOS_AMP_NODES}, so this own-image partition names no "
    "peer at all. There is nothing to merge into an artefact and no crossing to exercise; "
    "docs/design-multicore.md N6c makes a partition of two the smallest useful one. State "
    "KICKOS_AMP_NODES=2 or more in boards/<board>/configs/<variant>/defconfig.")
endif()

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

  # The second artefact: node 0 is the selftest and node 1 a peer that echoes, so the arms
  # needing a running peer have one. They are compiled on every posture and decide at runtime,
  # so this changes the ANSWER they get and not which of them exist.
  if(TARGET selftest AND KICKOS_ENABLE_SELFTEST)
    set(_amp_st_artefact "${CMAKE_BINARY_DIR}/kickos-partition-selftest.elf")
    add_custom_command(OUTPUT "${_amp_st_artefact}"
      COMMAND "${PROJECT_SOURCE_DIR}/tools/amp/build-partition.sh"
              "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}" "${PROJECT_SOURCE_DIR}"
              "${CMAKE_BINARY_DIR}/amp-peers-st"
              "${CMAKE_OBJCOPY}" "${CMAKE_LINKER}" "${CMAKE_READELF}" "${CMAKE_OBJDUMP}"
              "${CMAKE_TOOLCHAIN_FILE}" "${CMAKE_BUILD_TYPE}"
              "${_amp_st_artefact}" "${KICKOS_BOARD}" "${KICKOS_CONFIG_VARIANT}"
              "$<TARGET_FILE:selftest>" ${_amp_peer_echoes}
      DEPENDS selftest
      COMMENT "assembling the AMP partition with the selftest as node 0")
    add_custom_target(amp_partition_selftest DEPENDS "${_amp_st_artefact}")
    set_target_properties(ampping_n0 PROPERTIES
      KICKOS_AMP_ARTEFACT_SELFTEST "${_amp_st_artefact}")
  endif()
endif()
