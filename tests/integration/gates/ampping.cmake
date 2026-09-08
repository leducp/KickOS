# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding the own-image AMP partition. Two boot the merged artefact, one reads link
# geometry out of every node's ELF. The artefacts themselves are BUILD targets assembled in
# the app file, which records each path on ampping_n0 so this side states no path of its own.

if(NOT TARGET ampping_n0)
  return()
endif()

# N6b: deployment is a MERGE and not a second flash, owned by node 0, so the artefact targets
# and the peer build directories these gates read exist in a node 0 build alone.
if(NOT KICKOS_AMP_NODE_ID EQUAL 0)
  return()
endif()

get_target_property(_amp_artefact ampping_n0 KICKOS_AMP_ARTEFACT)
# Unset unless the selftest artefact was assembled, which needs the selftest target and the
# selftest posture. A property that is not set reads as false here.
get_target_property(_amp_st_artefact ampping_n0 KICKOS_AMP_ARTEFACT_SELFTEST)

# Two of the three gates boot the artefact and need an emulator; amp_elf_agree reads link
# geometry and needs none. kickos_add_qemu_test registers nothing at all for a board with no
# emulator, so the absence is announced here rather than left silent.
kickos_qemu_machine("${KICKOS_BOARD}" _amp_qemu_env _amp_qemu_machine)
set(_amp_emulated TRUE)
if(_amp_qemu_machine STREQUAL "")
  set(_amp_emulated FALSE)
  message(STATUS
    "KickOS: AMP partition on ${KICKOS_BOARD}: amp_elf_agree registers, the two gates that "
    "BOOT the artefact do not (no emulator for this board; witness it by flashing the merged "
    "artefact and capturing the console)")
endif()

# The selftest artefact puts node 0 on the selftest and node 1 on a peer that echoes, so the
# arms needing a running peer have one. They are compiled on every posture and decide at
# runtime, so this changes the ANSWER they get and not which of them exist.
if(_amp_st_artefact AND _amp_emulated)
  kickos_add_qemu_test(NAME amp_peer_arms TARGET ampping_n0
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_amp_peer_arms.sh"
    ARGS "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${_amp_st_artefact}"
    TIMEOUT 900)
  set_tests_properties(amp_peer_arms PROPERTIES RUN_SERIAL TRUE)
endif()

# Registers everywhere, which is why the emulator predicate is not wrapped around it: it boots
# nothing. It reads every node's ELF, taking the peer ROOT and the width rather than one peer
# directory, so nothing in it is shaped by how many nodes there are.
add_test(NAME amp_elf_agree
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_amp_elf_agree.sh"
          "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${CMAKE_READELF}"
          "$<TARGET_FILE:ampping_n0>" "${CMAKE_BINARY_DIR}/amp-peers"
          "${KICKOS_AMP_NODES}" ampserve)
set_tests_properties(amp_elf_agree PROPERTIES TIMEOUT 900 LABELS host)

if(_amp_emulated)
  # The gate boots the ARTEFACT, and building it is the first thing the script does.
  kickos_add_qemu_test(NAME amp_partition TARGET ampping_n0
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_amp_partition.sh"
    ARGS "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${_amp_artefact}" "${KICKOS_AMP_NODES}"
    TIMEOUT 900)
  set_tests_properties(amp_elf_agree PROPERTIES DEPENDS amp_partition)
endif()
# RUN_SERIAL is CORRECTNESS here. Each of these gates begins by running `cmake --build` on an
# artefact target, and two concurrent ninja invocations on one build directory race on the
# intermediates they share, leaving kernel/libkickos_kernel.a truncated mid-archive. The tell
# is a gate failing in a tenth of a second, too fast to have built or booted.
set_tests_properties(amp_elf_agree PROPERTIES RUN_SERIAL TRUE)
if(_amp_emulated)
  set_tests_properties(amp_partition PROPERTIES RUN_SERIAL TRUE)
endif()
