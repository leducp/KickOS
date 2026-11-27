# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The kernel-block depth witness. Its kernel is built in a scratch tree of this preset with
# KICKOS_WITNESS_KSTACK, whose panic prints the block's high-water line; this build carries no such
# kernel. It boots stackdepth0 (panic and nothing else) and stackdepth1 (the grant-carrying spawn,
# then panic): the block records a maximum, so the difference between the two readings is what the
# spawn arm reached.

if(NOT KICKOS_KERNEL_STACKS OR KICKOS_HAVE_ASPACE)
  return()
endif()
kickos_qemu_machine("${KICKOS_BOARD}" _sd_env _sd_machine)
if(_sd_machine STREQUAL "")
  return()
endif()

# The floor is a DECLARATION, not a measurement: it is the slack the block must keep over the
# paths these two images drive.
set(_sd_floor 256)

# Pinned in the command too: a job-wide QEMU_TIMEOUT would be charged once per boot.
set(_sd_bound 30)
add_test(NAME ${_tag}_stackdepth
  COMMAND "${CMAKE_COMMAND}" -E env ${_sd_env} QEMU_MACHINE=${_sd_machine} QEMU_TIMEOUT=${_sd_bound}
          "${PROJECT_SOURCE_DIR}/tests/integration/check_stackdepth.sh" "${PROJECT_SOURCE_DIR}"
          "${CMAKE_COMMAND}" "${_fs_preset_key}" "${PROJECT_BINARY_DIR}/stackdepth-witness"
          ${_sd_floor})
kickos_boot_timeout(${_tag}_stackdepth "${PROJECT_SOURCE_DIR}/tests/integration/check_stackdepth.sh"
  BOOTS 2 BOUND ${_sd_bound} WORK 300)
