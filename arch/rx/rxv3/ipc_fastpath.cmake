# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trap-handler IPC fastpath opt-in (mirrors the mpu.cmake opt-in). The top CMakeLists
# includes this in its own scope, so a plain set (no PARENT_SCOPE) is what it reads.
#
# switch.S branches inside kickos_rx_syscall_trap, builds the switch frame the leaf's
# args point into, and arch_rxv3.cc defines arch_ctx_set_syscall_result over that frame's
# R1 slot, so KOS_SYS_CALL_REG completes in the trap and the RTE lands in the server.
set(KICKOS_ARCH_HAS_IPC_FASTPATH 1)
