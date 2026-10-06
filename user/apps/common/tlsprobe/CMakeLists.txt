# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Built only where the board compiles TLS: elsewhere the thread_local is a link error naming
# __aeabi_read_tp, which is the intended diagnostic but not a fleet build failure.
add_executable(tlsprobe main.cc)
target_link_libraries(tlsprobe PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(tlsprobe)
