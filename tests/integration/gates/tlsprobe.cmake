# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The thread-pointer arms riding `tlsprobe`, plus the offline RX replay against the same
# image.

if(NOT TARGET tlsprobe)
  return()
endif()
# A chip that holds one task runs the probe without its peer.
if(KICKOS_CHIP_ONE_TASK)
  kickos_app_judge(tlsprobe tests/integration/check_qemu_tlsprobe.sh ARGS --alone)
else()
  kickos_app_judge(tlsprobe tests/integration/check_qemu_tlsprobe.sh)
endif()
if(TARGET tlsprobe_wide)
  kickos_app_judge(tlsprobe_wide tests/integration/check_qemu_tlsprobe.sh
                   ARGS --image=tlsprobe_wide)
endif()

kickos_add_qemu_test(TARGET tlsprobe
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")

# Its two programs' mains in the merged ELF, where every image sits at an address of its own.
if(KICKOS_IMAGE_SPLIT AND NOT KICKOS_CHIP_ONE_TASK)
  find_program(KICKOS_GDB NAMES gdb-multiarch)
  set(_tlsprobe_gdb gdb-multiarch)
  if(KICKOS_GDB)
    set(_tlsprobe_gdb "${KICKOS_GDB}")
  endif()
  kickos_add_qemu_test(NAME ${_tag}_gdb_main TARGET tlsprobe
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_gdb_space.sh"
    ARGS "${_tlsprobe_gdb}" "${CMAKE_NM}" "${PROJECT_SOURCE_DIR}")
endif()

# The RX emutls block is proved offline, rxv3 having no QEMU machine: every input the bump
# allocator uses is a link-time constant, so the allocation is replayed against the linked
# image. Only images in this tree are replayed.
if(KICKOS_ARCH STREQUAL "rxv3")
  add_test(
    NAME    rx_tls_fit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/rx_tls_fit.py"
            "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${CMAKE_BINARY_DIR}/user/apps")
  kickos_host_gate(rx_tls_fit TIMEOUT 60)
endif()
