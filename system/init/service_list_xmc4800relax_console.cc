// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// xmc4800-relax CONSOLE-ONLY service-list provider: one entry, KOS_SVC_CONSOLE ->
// xmcuart_console_start (USIC0 CH0 handover). The combined list that also brings the
// USIC0 CH1 SSC bus up is service_list_xmc4800relax.cc; the two are alternatives,
// never both linked (EXACTLY ONE kickos_board_services per image).
//
// Both bring-ups are pure syscall and touch no register from the calling thread, so both
// run from an unprivileged root holding AUTH_MEMORY + AUTH_CONSOLE. The combined list
// stays refused at enforcement until the xmcssc-as-a-service posture is witnessed on
// silicon, which is why this list is the board default
// (docs/design-unprivileged-root.md section 9).
//
// Selected automatically for xmc4800-relax + enforcement (root CMakeLists.txt), which is
// also where the combined list is refused. Per-instance config travels as DATA
// (kos_service_cfg), never as literals in a driver TU.

#include <kickos/sys/service.h>

#include <kickos/driver/xmcuart.h>

extern "C"
{
    // USIC0 CH0 @ 0x4003_0000, 0x200 B (RM Table 18-21): the 0x200-aligned pow2
    // window that encodes on PMSA as one exact-cover descriptor, leaving the sibling
    // channel U0C1 (base + 0x200) and the SCU / IOCR peripherals outside it. prio 12
    // must be >= every stdout client's priority (D9: no PI on the console rendezvous),
    // and matches the combined list so the two are behaviourally identical here.
    static struct kos_service_cfg const xmcuart_cfg = {
        /*name=*/"xmcuart", /*mmio_base=*/0x40030000u, /*mmio_window=*/0x200u,
        /*hz=*/0, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*cs_policy=*/KOS_SVC_CS_NONE, /*cs_index=*/0, /*rsv=*/{ 0, 0 }
    };

    static struct kos_service_bringup const xmc4800relax_console_services[] = {
        { xmcuart_console_start, &xmcuart_cfg },
    };
    struct kos_service_list const kickos_board_services = { xmc4800relax_console_services, 1 };
}
