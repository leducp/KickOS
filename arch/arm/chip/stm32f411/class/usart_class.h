// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 USART class-driver leaf (M4 Rule 6). A freestanding, stateless register-logic
// core: a free function taking the USART instance base explicitly, with no ctor/dtor and no
// mutable static state. Built as the kickos_class_stm32f411 static lib on a bare include
// path: repo include/ plus this chip's register dir, excluding kernel/include.
//
// The register map it reads is the chip's shared regs/usart.h.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_CLASS_USART_CLASS_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_CLASS_USART_CLASS_H

#include <stdint.h>

namespace kickos::stm32f411::driver
{
    // True when the instance at `base` can accept another byte into DR. Pure read of SR,
    // so side-effect-free: unlike DR, reading SR clears nothing on its own, and the
    // SR-then-DR pair is what the error latches need.
    bool usart_tx_ready(uintptr_t base);
}

#endif
