# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Built only where the board compiles TLS: elsewhere the thread_local is a link error naming
# __aeabi_read_tp, which is the intended diagnostic but not a fleet build failure.
kickos_add_app_target(tlsprobe main.cc)
target_link_libraries(tlsprobe PRIVATE kickos)
kickos_emit_image(tlsprobe)
