# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The two-image AMP partition's own demo: two roots, one per node, each its own image with its
# own init provider, service list and app, talking over the shared window.
#
# BOTH TARGETS ARE BUILT BY BOTH NODE PRESETS, an application being an executable and not a
# per-preset selection. Which one belongs to which node is decided where the partition is
# assembled (tools/amp/merge-partition.sh), by node order.
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
kickos_add_application(ampping_n1
  SOURCES main_n1.c
  BOARD   ${KICKOS_BOARD}
  CAPABILITIES_INBOUND_REPLY 1)

# The peer the SELFTEST talks to: it echoes the caller's own bytes. Separate from ampping_n1,
# whose reply transforms the payload to show a service doing work.
kickos_add_application(ampecho_n1
  SOURCES main_echo.c
  BOARD   ${KICKOS_BOARD}
  CAPABILITIES_INBOUND_REPLY 1)

# One peer app per peer node, in node order. tools/amp/build-partition.sh and
# merge-partition.sh both take a peer target per node, so the node count is not what stops
# here: these two lists are.
set(_amp_peer_apps ampping_n1)
set(_amp_peer_echoes ampecho_n1)
list(LENGTH _amp_peer_apps _amp_peer_app_count)
math(EXPR _amp_peers "${KICKOS_AMP_NODES} - 1")

if(KICKOS_AMP_NODES LESS 2)
  message(FATAL_ERROR
    "KickOS: KICKOS_AMP_NODES is ${KICKOS_AMP_NODES}, so this own-image partition names no "
    "peer at all. There is nothing to merge into an artefact and no crossing to exercise; "
    "docs/design-multicore.md N6c makes a partition of two the smallest useful one. State "
    "KICKOS_AMP_NODES=2 or more in boards/<board>/configs/<variant>/defconfig.")
endif()

if(NOT _amp_peers EQUAL _amp_peer_app_count)
  message(FATAL_ERROR
    "KickOS: this AMP partition holds ${KICKOS_AMP_NODES} nodes, which is ${_amp_peers} "
    "peer(s), and user/apps/common/ampping supplies ${_amp_peer_app_count}: "
    "'${_amp_peer_apps}' and '${_amp_peer_echoes}'. Two things are missing, and neither is "
    "the partition tooling: tools/amp/build-partition.sh and merge-partition.sh already take "
    "one peer target per node in node order, and every gate but one reads counters rather "
    "than a pair. What is owed is a demo app per peer node here, added to both lists above, "
    "and an N-way form of tests/static/check_amp_two_elf.sh, which compares node 0 against "
    "exactly ONE peer build directory. Until both exist this width would build green with no "
    "artefact and no AMP gate registered at all.")
endif()

# N6b: deployment is a MERGE and not a second flash. Node 0 owns it, being the node that
# releases the others at boot, so this target exists only in a node 0 build and one step
# produces the whole partition:
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
  # needing a peer that is running have one. They are compiled on every posture and decide at
  # runtime, so this changes the ANSWER they get and not which of them exist.
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

    if(KICKOS_BUILD_TESTS)
      kickos_add_qemu_test(NAME amp_peer_arms TARGET ampping_n0 BOARD ${KICKOS_BOARD}
        SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_amp_peer_arms.sh"
        ARGS "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${_amp_st_artefact}"
        TIMEOUT 900)
      set_tests_properties(amp_peer_arms PROPERTIES RUN_SERIAL TRUE)
    endif()
  endif()

  if(KICKOS_BUILD_TESTS)
    # The gate boots the ARTEFACT, and building it is the first thing the script does.
    kickos_add_qemu_test(NAME amp_partition TARGET ampping_n0 BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_amp_partition.sh"
      ARGS "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${_amp_artefact}" "${KICKOS_AMP_NODES}"
      TIMEOUT 900)
    # The two images agree on the partition or they are two partitions, and nothing a single
    # link sees can check it, so this reads both ELFs. It takes node 0 and ONE peer build
    # directory, so a wider partition needs an N-way form of it.
    add_test(NAME amp_two_elf
      COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_amp_two_elf.sh"
              "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${CMAKE_READELF}"
              "$<TARGET_FILE:ampping_n0>" "${CMAKE_BINARY_DIR}/amp-peers/node1")
    set_tests_properties(amp_two_elf PROPERTIES TIMEOUT 900 LABELS host DEPENDS amp_partition)
    # RUN_SERIAL is correctness here. Each of these gates begins by running `cmake --build` on
    # an artefact target, and two concurrent ninja invocations on ONE build directory race on
    # the intermediates they share, leaving kernel/libkickos_kernel.a truncated mid-archive.
    # Under `ctest -j` it is a draw, and the tell is a gate failing in about a tenth of a
    # second, far too fast to have built or booted anything.
    set_tests_properties(amp_partition amp_two_elf PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()
