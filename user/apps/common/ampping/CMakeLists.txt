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
if(NOT KICKOS_AMP_NODE OR NOT KICKOS_AMP_OWN_IMAGE)
  return()
endif()

kickos_add_application(ampping_n0
  SOURCES main_n0.c
  BOARD   ${KICKOS_BOARD})

# One CONCURRENTLY parked far caller: the peer's call is held on this node's table while the
# service thread answers it.
kickos_add_application(ampserve
  SOURCES main_serve.c
  BOARD   ${KICKOS_BOARD}
  CAPABILITIES_INBOUND_REPLY 1)

# The peer the SELFTEST talks to: it echoes the caller's own bytes, where ampserve's reply
# transforms the payload.
kickos_add_application(ampecho
  SOURCES main_echo.c
  BOARD   ${KICKOS_BOARD}
  CAPABILITIES_INBOUND_REPLY 1)

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

# --- Which of these gates this board can actually run --------------------------------------
# Two of the three boot the artefact and need an emulator; amp_elf_agree reads link geometry
# and needs none. The absence is announced rather than silent, and the predicate is needed at
# all because kickos_add_qemu_test refuses a board it does not know, by name.
set(_amp_emulated FALSE)
if(KICKOS_BOARD STREQUAL "qemu-arm64")
  set(_amp_emulated TRUE)
else()
  message(STATUS
    "KickOS: AMP partition on ${KICKOS_BOARD}: amp_elf_agree registers, the two gates that "
    "BOOT the artefact do not (no emulator for this board; witness it by flashing the merged "
    "artefact and capturing the console)")
endif()

# N6b: deployment is a MERGE and not a second flash, owned by node 0, so this target exists
# only in a node 0 build and one step produces the whole partition:
#
#   cmake --build build/qemu-arm64-amp2-n0 --target amp_partition
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

    if(KICKOS_BUILD_TESTS AND _amp_emulated)
      kickos_add_qemu_test(NAME amp_peer_arms TARGET ampping_n0 BOARD ${KICKOS_BOARD}
        SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_amp_peer_arms.sh"
        ARGS "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${_amp_st_artefact}"
        TIMEOUT 900)
      set_tests_properties(amp_peer_arms PROPERTIES RUN_SERIAL TRUE)
    endif()
  endif()

  if(KICKOS_BUILD_TESTS)
    # Registers everywhere, which is why the emulator predicate is not wrapped around the
    # block: it boots nothing. It reads every node's ELF, taking the peer ROOT and the width
    # rather than one peer directory, so nothing in it is shaped by how many nodes there are.
    add_test(NAME amp_elf_agree
      COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_amp_elf_agree.sh"
              "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${CMAKE_READELF}"
              "$<TARGET_FILE:ampping_n0>" "${CMAKE_BINARY_DIR}/amp-peers"
              "${KICKOS_AMP_NODES}" ampserve)
    set_tests_properties(amp_elf_agree PROPERTIES TIMEOUT 900 LABELS host)

    if(_amp_emulated)
      # The gate boots the ARTEFACT, and building it is the first thing the script does.
      kickos_add_qemu_test(NAME amp_partition TARGET ampping_n0 BOARD ${KICKOS_BOARD}
        SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_amp_partition.sh"
        ARGS "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${_amp_artefact}" "${KICKOS_AMP_NODES}"
        TIMEOUT 900)
      set_tests_properties(amp_elf_agree PROPERTIES DEPENDS amp_partition)
    endif()
    # RUN_SERIAL is CORRECTNESS here. Each of these gates begins by running `cmake --build` on
    # an artefact target, and two concurrent ninja invocations on one build directory race on
    # the intermediates they share, leaving kernel/libkickos_kernel.a truncated mid-archive.
    # The tell is a gate failing in a tenth of a second, too fast to have built or booted.
    set_tests_properties(amp_elf_agree PROPERTIES RUN_SERIAL TRUE)
    if(_amp_emulated)
      set_tests_properties(amp_partition PROPERTIES RUN_SERIAL TRUE)
    endif()
  endif()
endif()
