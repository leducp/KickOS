// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/trap.h>

extern "C" kickos::x86_64::trap_frame* kickos_x86_64_isr(kickos::x86_64::trap_frame* frame)
{
    (void)frame;
    return nullptr;
}
