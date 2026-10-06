// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F LPTMR0 driver as the task's entry, over the window and line its composition grants: it
// clocks the timer through kos_periph_enable, counts the 1 kHz LPO to a 250 ms compare for ten
// ticks on the line, then has a thread holding no window read the counter. The SYSMPU sees no
// device and the AIPS bridge opens a whole slot to every unprivileged thread, so that read
// succeeds: the slot is the gate, not the window.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>
#include <stdlib.h>

namespace
{
    // K64 RM 42.3.
    constexpr uintptr_t CSR_OFFSET = 0x00u;
    constexpr uintptr_t PSR_OFFSET = 0x04u;
    constexpr uintptr_t CMR_OFFSET = 0x08u;
    constexpr uintptr_t CNR_OFFSET = 0x0Cu;
    constexpr uint32_t WINDOW_BYTES = 0x10u;

    constexpr uint32_t CSR_TEN = 1u << 0;
    constexpr uint32_t CSR_TIE = 1u << 6;
    constexpr uint32_t CSR_TCF = 1u << 7; // w1c
    // PCS=01 selects the 1 kHz LPO (RM 3.8.4.1) and PBYP clocks the counter with it directly.
    constexpr uint32_t PSR_PCS_LPO = 1u << 0;
    constexpr uint32_t PSR_PBYP = 1u << 2;
    constexpr uint32_t COMPARE = 249u; // 250 LPO counts per tick

    constexpr int TICKS = 10;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    [[noreturn]] void give_up(char const* what, int rc)
    {
        char m[80];
        ksnprintf(m, sizeof(m), "[k64drv] ERROR: %s rc %d\n", what, rc);
        kos::print(m);
        exit(1);
    }

    // Holds no window: the base arrives as a value, never dereferenced as memory.
    void slot_reader(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        // RM 42.4.5: a write latches the counter into the register the next read returns.
        r32(win + CNR_OFFSET) = 0u;
        uint32_t const cnr = r32(win + CNR_OFFSET);
        char s[96];
        ksnprintf(s, sizeof(s), "[k64drv] a thread holding no window read CNR=%u through the open slot\n",
                  static_cast<unsigned>(cnr));
        kos::print(s);
    }
}

extern "C" void k64drv_main(kos_self_t const* self)
{
    kos_window_t const window = kos_grant_mmio(self, "/dev/lptmr0");
    kos_line_t const line = kos_grant_irq(self, "irq");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < WINDOW_BYTES or line.cap == KOS_CAP_NONE)
    {
        give_up("no /dev/lptmr0 window or irq line", -KOS_EBADF);
    }

    // Ungates the clock and opens the slot: until then any access to it is a bus error.
    int rc = kos_periph_enable(win);
    if (rc != 0)
    {
        give_up("periph_enable(LPTMR0)", rc);
    }

    kos_cap_t note = KOS_CAP_NONE;
    rc = kos_notify_create(&note);
    if (rc == 0)
    {
        rc = kos_notify_bind(note);
    }
    if (rc == 0)
    {
        rc = kos_irq_bind_notify(line.cap, note);
    }
    if (rc != 0)
    {
        give_up("the line's notification", rc);
    }

    // RM 42.4.1: CSR is written with the timer off before PSR and CMR, and TIE goes in last.
    r32(win + CSR_OFFSET) = 0u;
    r32(win + PSR_OFFSET) = PSR_PCS_LPO | PSR_PBYP;
    r32(win + CMR_OFFSET) = COMPARE;
    r32(win + CSR_OFFSET) = CSR_TIE | CSR_TEN;
    kos::print("[k64drv] LPTMR0 counting the 1 kHz LPO, compare every 250 ms\n");

    for (int tick = 1; tick <= TICKS; tick++)
    {
        (void)kos_notify_wait(note, 1u, KOS_TIMEOUT_NONE, nullptr);
        // TCF is a level: cleared before the next wait rearms the edge-claimed line, with the
        // other CSR bits written as they stand.
        r32(win + CSR_OFFSET) = CSR_TCF | CSR_TIE | CSR_TEN;
        kos::kernel_diag_led_toggle();
        char s[48];
        ksnprintf(s, sizeof(s), "[k64drv] tick %d\n", tick);
        kos::print(s);
    }

    auto const reader = kos::thread::create(slot_reader, reinterpret_cast<void*>(win), "k64read", 10);
    if (not reader.valid())
    {
        give_up("spawn of the windowless reader", reader.error());
    }
    (void)reader.join();
    r32(win + CSR_OFFSET) = 0u;
    kos::print("[k64drv] done\n");
}
