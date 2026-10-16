# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The repass witness: a later pass of one exception never rebuilds the frame the core stands on,
# and a switch booked back onto that frame still ends the lock span. Its kernel is built with
# KICKOS_WITNESS_REPASS in a scratch tree of this preset. Above one kernel core, and on x86_64
# alone: elsewhere neither fault turns it red, every exit ending the span for any booked switch and
# a rebuilt unprivileged victim's interrupt frame happening to resume cleanly.

if(NOT KICKOS_KERNEL_CORES GREATER 1 OR NOT KICKOS_ARCH STREQUAL "x86_64" OR NOT TARGET slaypeer)
  return()
endif()
kickos_qemu_machine("${KICKOS_BOARD}" _rp_env _rp_machine)
if(_rp_machine STREQUAL "")
  return()
endif()
set(_rp_bound 30)
add_test(NAME ${_tag}_repass
  COMMAND "${CMAKE_COMMAND}" -E env ${_rp_env} QEMU_MACHINE=${_rp_machine} QEMU_TIMEOUT=${_rp_bound}
          "${PROJECT_SOURCE_DIR}/tests/integration/check_repass.sh" "${PROJECT_SOURCE_DIR}"
          "${CMAKE_COMMAND}" "${_fs_preset_key}" "${KICKOS_ARCH}"
          "${PROJECT_BINARY_DIR}/repass-witness")
kickos_boot_timeout(${_tag}_repass "${PROJECT_SOURCE_DIR}/tests/integration/check_repass.sh"
  BOOTS 1 BOUND ${_rp_bound} WORK 300)
