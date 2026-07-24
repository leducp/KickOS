// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The universal-default console bring-up provider: no userspace console driver, so
// the default init keeps the kernel console. Selected by KICKOS_CONSOLE_BRINGUP on
// every board that ships no driver provider. start = NULL is the "keep the kernel
// console" signal, not a failure.

#include <kickos/sys/bringup.h>

extern "C"
{
    struct kos_console_bringup const kickos_board_console = { nullptr, 0 };
}
