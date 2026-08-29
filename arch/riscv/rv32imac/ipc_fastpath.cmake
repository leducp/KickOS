# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trap-handler IPC fastpath opt-in.
#
# switch.S takes the seam inside .Lecall and defines arch_syscall_reg; arch_rv32imac.cc
# defines arch_ctx_set_syscall_result, so KOS_SYS_CALL_REG completes in the trap and the
# mret lands in the server.
set(KICKOS_ARCH_HAS_IPC_FASTPATH 1)
