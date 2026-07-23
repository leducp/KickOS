// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M I/O-port class-driver leaf (M4 Rule 6, decision R-A). A FREESTANDING,
// STATELESS register-logic core: free functions taking the PODR block base
// explicitly, no ctor/dtor, no mutable static state. Built as its own
// kickos_class_rx72m static lib on a bare include path (repo include/ + this
// chip's register dir ONLY -- never kernel/include), so the SAME object links
// unchanged into BOTH the kernel (Phase A timebase, deferred) and an unprivileged
// userspace GPIO driver. One shared read-only copy, so no writable state.
//
// The RX chip layer keeps its SFR map inline (no shared chip header), so this leaf
// owns the one layout fact it needs -- the PODR block is one byte per port,
// contiguous from PORT0 (UM r01uh0804ej0120 sec.22.3.2). It maps N identical ports
// onto one base per rule 4 (parameterize by base + port index).

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_CLASS_PORT_CLASS_H
#define KICKOS_ARCH_RX_CHIP_RX72M_CLASS_PORT_CLASS_H

#include <stdint.h>

namespace kickos
{
namespace rx
{
namespace classdrv
{
    // Read a port's output data latch (PODR). podr_base is the PODR block base
    // (PORT0.PODR); port is the port index (PORT8 -> 8). Pure read -- reading PODR
    // has no side effect. Shared by the kernel diag-LED path and a userspace GPIO
    // driver that reads back what it drove.
    uint8_t port_odr_read(uintptr_t podr_base, unsigned port);
}
}
}

#endif
