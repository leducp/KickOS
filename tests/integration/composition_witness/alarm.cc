// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The line taker (docs/design-m10-target.md, section 8): a user task holding the PL031's window
// and its alarm line, both granted by name. It arms the match interrupt two seconds ahead, waits
// on the line through a notification of its own, and prints the arrival. Its exit status ends the
// system: 0 once the interrupt arrived with the match pending and the clock at the second it was
// armed for, 1 otherwise.

#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
    // PL031 registers, in 32-bit words.
    constexpr uint32_t RTCDR = 0x00u / 4u;   // seconds
    constexpr uint32_t RTCMR = 0x04u / 4u;   // match
    constexpr uint32_t RTCIMSC = 0x10u / 4u; // interrupt mask set
    constexpr uint32_t RTCMIS = 0x18u / 4u;  // masked interrupt status
    constexpr uint32_t RTCICR = 0x1Cu / 4u;  // interrupt clear
    constexpr uint32_t WINDOW_BYTES = 0x20u;
    constexpr uint32_t AHEAD_S = 2u;
    constexpr uint32_t WAIT_US = 10000000u;
}

extern "C" void alarm_main(kos_self_t const* self)
{
    kos_window_t const rtc_window = kos_grant_mmio(self, "/dev/rtc");
    kos_line_t const line = kos_grant_irq(self, "alarm");
    auto const rtc = static_cast<uint32_t volatile*>(kos_window_addr(rtc_window));
    if (rtc == nullptr or kos_window_size(rtc_window) < WINDOW_BYTES or line.cap == KOS_CAP_NONE)
    {
        printf("alarm: no /dev/rtc window or alarm line\n");
        exit(1);
    }

    kos_cap_t notify = KOS_CAP_NONE;
    kos_cap_t badged = KOS_CAP_NONE;
    int rc = kos_notify_create(&notify);
    if (rc == 0)
    {
        rc = kos_notify_bind(notify);
    }
    if (rc == 0)
    {
        rc = kos_notify_badge(notify, 0u, &badged);
    }
    if (rc == 0)
    {
        rc = kos_irq_bind_notify(line.cap, badged);
    }
    if (rc != 0)
    {
        printf("alarm: the line's notification answered %d\n", rc);
        exit(1);
    }

    rtc[RTCICR] = 1u;
    uint32_t const armed = rtc[RTCDR] + AHEAD_S;
    rtc[RTCMR] = armed;
    rtc[RTCIMSC] = 1u;

    uint32_t bits = 0;
    rc = kos_notify_wait(notify, 1u, WAIT_US, &bits);
    uint32_t const pending = rtc[RTCMIS];
    uint32_t const now = rtc[RTCDR];
    rtc[RTCIMSC] = 0u;
    rtc[RTCICR] = 1u;
    if (rc != 0)
    {
        printf("alarm: no interrupt (%d)\n", rc);
        exit(1);
    }
    if ((pending & 1u) == 0u)
    {
        printf("alarm: woken with no match pending\n");
        exit(1);
    }
    if (now < armed)
    {
        printf("alarm: woken at %lu, before the %lu armed\n", static_cast<unsigned long>(now),
               static_cast<unsigned long>(armed));
        exit(1);
    }
    printf("alarm: the match interrupt arrived at %lu, armed for %lu\n",
           static_cast<unsigned long>(now), static_cast<unsigned long>(armed));
}
