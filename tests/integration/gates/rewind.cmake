# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The rewind witness: a thread switched out inside the user compare-and-swap's window resumes at
# its load. Its kernel is built with KICKOS_WITNESS_REWIND in a scratch tree of this preset, on
# the arches whose switcher rewinds; rxv3 has no emulator, so its capture is judged by the same
# script from silicon.

if(NOT KICKOS_ARCH STREQUAL "armv6m")
  return()
endif()
kickos_qemu_machine("${KICKOS_BOARD}" _rw_env _rw_machine)
if(_rw_machine STREQUAL "")
  return()
endif()
set(_rw_bound 30)
add_test(NAME ${_tag}_rewind
  COMMAND "${CMAKE_COMMAND}" -E env ${_rw_env} QEMU_MACHINE=${_rw_machine} QEMU_TIMEOUT=${_rw_bound}
          "${PROJECT_SOURCE_DIR}/tests/integration/check_rewind.sh" "${PROJECT_SOURCE_DIR}"
          "${CMAKE_COMMAND}" "${_fs_preset_key}" "${PROJECT_BINARY_DIR}/rewind-witness")
kickos_boot_timeout(${_tag}_rewind "${PROJECT_SOURCE_DIR}/tests/integration/check_rewind.sh"
  BOOTS 1 BOUND ${_rw_bound} WORK 300)
