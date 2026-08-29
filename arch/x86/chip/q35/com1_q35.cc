// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Every poll here is BOUNDED: the panic, fault, assert and pre-arm paths all reach this part,
// where a wedged UART must cost a dropped tail and not a hang.

#include <kickos/arch/portio.h>
#include <kickos/chip_com1.h>

#include <stdint.h>

namespace kickos::q35
{
    namespace
    {
        constexpr uint16_t com1_base = 0x3f8;

        constexpr uint16_t reg_data = 0;
        constexpr uint16_t reg_int_enable = 1;
        constexpr uint16_t reg_divisor_lo = 0;
        constexpr uint16_t reg_divisor_hi = 1;
        constexpr uint16_t reg_fifo_ctrl = 2;
        constexpr uint16_t reg_line_ctrl = 3;
        constexpr uint16_t reg_modem_ctrl = 4;
        constexpr uint16_t reg_line_status = 5;

        constexpr uint8_t line_ctrl_8n1 = 0x03;
        constexpr uint8_t line_ctrl_dlab = 0x80;
        // Only tx_empty says the part is actually idle: the shift register empty too.
        constexpr uint8_t line_status_thr_empty = 0x20;
        constexpr uint8_t line_status_tx_empty = 0x40;

        constexpr uint32_t poll_bound = 100000;

        void put(uint16_t reg, uint8_t value)
        {
            kickos::x86_64::outb(static_cast<uint16_t>(com1_base + reg), value);
        }

        uint8_t get(uint16_t reg)
        {
            return kickos::x86_64::inb(static_cast<uint16_t>(com1_base + reg));
        }
    }

    void com1_init(void)
    {
        put(reg_int_enable, 0x00);
        put(reg_line_ctrl, line_ctrl_dlab);
        // 1.8432 MHz reference over a divisor of 1: 115200 baud.
        put(reg_divisor_lo, 0x01);
        put(reg_divisor_hi, 0x00);
        put(reg_line_ctrl, line_ctrl_8n1);
        put(reg_fifo_ctrl, 0xc7);
        put(reg_modem_ctrl, 0x03);
    }

    void com1_putc(char c)
    {
        uint32_t spin = 0;
        while ((get(reg_line_status) & line_status_thr_empty) == 0 and spin < poll_bound)
        {
            spin++;
        }
        put(reg_data, static_cast<uint8_t>(c));
    }

    void com1_drain(void)
    {
        uint32_t spin = 0;
        while ((get(reg_line_status) & line_status_tx_empty) == 0 and spin < poll_bound)
        {
            spin++;
        }
    }

    void com1_puts(char const* s)
    {
        while (*s != '\0')
        {
            if (*s == '\n')
            {
                com1_putc('\r');
            }
            com1_putc(*s);
            ++s;
        }
    }

    void com1_hex64(uint64_t v)
    {
        char const* digits = "0123456789abcdef";
        com1_puts("0x");
        int shift = 60;
        while (shift >= 0)
        {
            com1_putc(digits[(v >> shift) & 0xf]);
            shift -= 4;
        }
    }

    void com1_dec(uint64_t v)
    {
        char buf[21];
        int n = 0;
        if (v == 0)
        {
            com1_putc('0');
            return;
        }
        while (v != 0)
        {
            buf[n] = static_cast<char>('0' + (v % 10));
            ++n;
            v /= 10;
        }
        while (n > 0)
        {
            --n;
            com1_putc(buf[n]);
        }
    }
}
