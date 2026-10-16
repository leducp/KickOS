# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The uaccess witness: a ring-0 access to a user page faults, on every arch whose hardware keeps
# ring 0 out of user pages. Its kernel is built in a scratch tree of this preset with
# KICKOS_WITNESS_UACCESS on; this build carries no such kernel. An arch joins with its name here,
# the posture its banner prints, and a judge in check_uaccess.sh.

if(NOT KICKOS_HAVE_ASPACE)
  return()
endif()

kickos_qemu_machine("${KICKOS_BOARD}" _ua_env _ua_machine)
if(_ua_machine STREQUAL "")
  return()
endif()

set(_ua_posture "")
if(KICKOS_ARCH STREQUAL "armv8a")
  # The cores QEMU models with no PAN (ARMv8.0); every later one has it.
  set(_ua_v80 cortex-a35 cortex-a53 cortex-a57 cortex-a72)
  kickos_qemu_cpu(_ua_cpu)
  if(_ua_cpu STREQUAL "")
    message(FATAL_ERROR "KickOS: board ${KICKOS_BOARD} names its emulated core neither in its "
      "emulator line's -cpu nor in its chip's -mcpu, so the uaccess gate cannot tell which guard "
      "it runs under")
  endif()
  if(_ua_cpu IN_LIST _ua_v80)
    set(_ua_posture ttbr0)
  else()
    set(_ua_posture pan)
  endif()
elseif(KICKOS_ARCH STREQUAL "rv64imac")
  set(_ua_posture "sum clear")
elseif(KICKOS_ARCH STREQUAL "x86_64")
  # The processor model every x86_64 emulation runs offers both (cmake/kickos.cmake).
  if(NOT KICKOS_X86_64_QEMU_CPU MATCHES ",\\+smep(,|$)"
     OR NOT KICKOS_X86_64_QEMU_CPU MATCHES ",\\+smap(,|$)")
    message(FATAL_ERROR "KickOS: KICKOS_X86_64_QEMU_CPU offers no SMEP or no SMAP, so the "
      "uaccess gate has no guard to witness")
  endif()
  set(_ua_posture "smep smap")
endif()
if(_ua_posture STREQUAL "")
  return()
endif()

# Pinned in the command too: a job-wide QEMU_TIMEOUT would be charged once per boot.
set(_ua_bound 60)
add_test(NAME ${_tag}_uaccess
  COMMAND "${CMAKE_COMMAND}" -E env ${_ua_env} QEMU_MACHINE=${_ua_machine} QEMU_TIMEOUT=${_ua_bound}
          "${PROJECT_SOURCE_DIR}/tests/integration/check_uaccess.sh" "${PROJECT_SOURCE_DIR}"
          "${CMAKE_COMMAND}" "${_fs_preset_key}" "${KICKOS_ARCH}" "${_ua_posture}"
          "${PROJECT_BINARY_DIR}/uaccess-witness")
set(_ua_boots 5)
if(KICKOS_ARCH STREQUAL "x86_64")
  # Seven arms and the control.
  set(_ua_boots 8)
endif()
kickos_boot_timeout(${_tag}_uaccess "${PROJECT_SOURCE_DIR}/tests/integration/check_uaccess.sh"
  BOOTS ${_ua_boots} BOUND ${_ua_bound} WORK 300)
