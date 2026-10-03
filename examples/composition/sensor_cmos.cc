// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The sensor task for QEMU q35, which owns a PORT device. Its measurement is the CMOS clock's
// seconds register, read as the clock keeps it (BCD by default), which is enough for an app that
// only asks whether the value moves. The composition grants the clock's port range; the task's
// I/O permission bitmap opens the data port alone. The index port's top bit masks NMIs, so the
// index is written through kos_port_reg_write, a byte-wide write that withholds that bit.

#include <kickos/sys.h>

#include "sample.h"

#include <stdint.h>

namespace
{
    constexpr uintptr_t INDEX = 0x0;      // offset of the index port within the range
    constexpr uint16_t DATA_OFFSET = 0x1; // offset of the data port within the range
    constexpr uint32_t REG_SECONDS = 0x00;
    constexpr uint32_t REG_STATUS_A = 0x0A;
    constexpr uint8_t STATUS_A_UPDATING = 0x80;

    uint8_t inb(uint16_t port)
    {
        uint8_t v;
        __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
        return v;
    }

    bool read_register(uintptr_t base, uint32_t reg, uint8_t* out)
    {
        if (kos_port_reg_write(base, INDEX, reg) != 0)
        {
            return false;
        }
        *out = inb(static_cast<uint16_t>(base + DATA_OFFSET));
        return true;
    }
}

extern "C" void sensor_main(kos_self_t const* self)
{
    // A port lookup answers the same handle as a memory one; its address is the first port.
    kos_window_t const rtc_ports = kos_grant_ports(self, "/dev/cmos_rtc");
    kos_window_t const history_window = kos_grant_mem(self, "/shm/history");
    uintptr_t const base = reinterpret_cast<uintptr_t>(kos_window_addr(rtc_ports));
    auto const hist = static_cast<history*>(kos_window_addr(history_window));
    kos_cap_t const served = kos_grant_endpoint(self, "/svc/sensor");
    if (kos_window_size(rtc_ports) <= DATA_OFFSET or hist == nullptr
        or kos_window_size(history_window) < sizeof(history))
    {
        return;
    }

    struct kos_reply_recv_opts opts;
    kos_cap_t reply = KOS_CAP_NONE;
    struct sample s = {};
    while (true)
    {
        kos_reply_recv_opts_init(&opts, served, 0u, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(reply, &s, kos_call_lens_pack(sizeof(s), 0u), &opts);
        reply = KOS_CAP_NONE;
        if (n < 0)
        {
            continue;
        }
        reply = opts.info.reply_cap;

        // A read during the clock's once-a-second update can tear, so wait for it to finish.
        uint8_t status = STATUS_A_UPDATING;
        uint8_t seconds = 0;
        bool ok = true;
        for (int i = 0; i < 1000 and ok and (status & STATUS_A_UPDATING) != 0; i++)
        {
            ok = read_register(base, REG_STATUS_A, &status);
        }
        if (ok and (status & STATUS_A_UPDATING) == 0 and read_register(base, REG_SECONDS, &seconds))
        {
            s.value = seconds;
            history_append(hist, s.value);
        }
        else
        {
            kos_handle_close(reply);
            reply = KOS_CAP_NONE;
        }
    }
}
