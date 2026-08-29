# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trap-handler IPC fastpath opt-in.
#
# switch.S branches inside SVC_Handler and arch_armv6m.cc defines
# arch_ctx_set_syscall_result over the saved block, so KOS_SYS_CALL_REG completes in the
# trap and the exception return lands in the server.
set(KICKOS_ARCH_HAS_IPC_FASTPATH 1)
