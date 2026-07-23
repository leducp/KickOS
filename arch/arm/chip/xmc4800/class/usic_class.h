// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC class-driver leaf (M4 Rule 6, decision R-A). A FREESTANDING,
// STATELESS register-logic core: POD + free functions taking the channel base
// explicitly, no ctor/dtor, no mutable static state. It is built as its own
// kickos_class_xmc4800 static lib on a bare include path (repo include/ + this
// chip's register header dir ONLY -- never kernel/include), so the SAME object
// links unchanged into BOTH the kernel timebase (Phase A, deferred) and an
// unprivileged userspace USIC driver. KickOS links kernel + driver into one
// MPU-partitioned ELF, so this leaf is one shared read-only copy. That is why
// it must carry no writable state.
//
// Register offsets + bit fields come from the chip's existing usic.h; this leaf
// adds NO register definitions of its own.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_CLASS_USIC_CLASS_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_CLASS_USIC_CLASS_H

#include <stdint.h>

namespace kickos
{
namespace xmc
{
namespace classdrv
{
    // Transmit buffer ready to accept the next word (TCSR.TDV clear). Pure read
    // of TCSR -- side-effect-free, safe on any clocked USIC channel. Shared by the
    // kernel polled writer and the userspace UART driver's per-byte poll.
    bool usic_tx_ready(uintptr_t base);
}
}
}

#endif
