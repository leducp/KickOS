# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trap-handler IPC fastpath opt-in (mirrors the mpu.cmake opt-in). The top CMakeLists
# includes this in its own scope, so a plain set (no PARENT_SCOPE) is what it reads.
#
# switch.S takes the seam inside .Lecall and arch_rv32imac.cc defines arch_syscall_reg
# with arch_ctx_set_syscall_result, so KOS_SYS_CALL_REG completes in the trap and the
# mret lands in the server.
set(KICKOS_ARCH_HAS_IPC_FASTPATH 1)
