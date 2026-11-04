# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Built only where the board compiles TLS: elsewhere the thread_local is a link error naming
# __aeabi_read_tp, which is the intended diagnostic but not a fleet build failure.
add_executable(tlsprobe main.cc)
# Without exceptions: a thread_local destructor that prints would otherwise link the exception
# runtime.
set_source_files_properties(main.cc PROPERTIES COMPILE_OPTIONS -fno-exceptions)
add_executable(tlsprobe_peer peer.cc)
target_link_libraries(tlsprobe_peer PRIVATE kickos)
kickos_app_system(tlsprobe COMPOSITION system.yaml)
kickos_emit_image(tlsprobe)
# Six task images, where the ROM boots esptool's image format, whose segment count is bounded.
# A single-node build only: an AMP node's slice seats fewer threads than its six tasks.
if(DEFINED KICKOS_CHIP_ESPTOOL_IMAGE AND NOT KICKOS_AMP_NODE)
  add_executable(tlsprobe_wide main.cc)
  kickos_app_system(tlsprobe_wide COMPOSITION system_wide.yaml SYSTEM tlsprobe_wide_system)
  kickos_emit_image(tlsprobe_wide)
endif()
