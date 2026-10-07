# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The exit statuses the kernel gives a task or a thread that picked none. ABI, not configuration:
# no Kconfig symbol, no defconfig states one. Declared in the build, whose gates expect them, and
# emitted to C through the generated <kickos/sys/exit_status.h>, which <kickos/sys/abi.h> includes.

# A thread killed by a CPU fault, 128 + SIGSEGV: its task's status, and the process status when it
# was the last thread live. A clean kos_exit(139) aliases it.
set(KOS_EXIT_FAULT 139)
# A cancelled thread, 128 + SIGINT, the kernel ending it: also the status of a task whose members
# were all cancelled.
set(KOS_EXIT_CANCELLED 130)
kickos_emit_geometry(kickos/sys/exit_status.h KOS_EXIT_FAULT KOS_EXIT_CANCELLED)
