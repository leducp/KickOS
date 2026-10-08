// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console seams of a semihosting chip. SYS_WRITEC hands each byte to the host inside the
// call: no channel to wait on, no completion to interrupt on, nothing ever in flight, so
// arch_console_flush_sync is left to its no-op fallback.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>

#include <stdint.h>

#include "semihost.h"

namespace
{
    void semihost_putc(char c)
    {
        kickos::semihost::call(kickos::semihost::SYS_WRITEC, &c);
    }

    int semihost_tx_slot_free(void) { return 1; }
    void semihost_tx_push(uint8_t b) { semihost_putc(static_cast<char>(b)); }
    void semihost_tx_irq_enable(void) {}
    void semihost_tx_irq_disable(void) {}

    char console_tx_buf[KICKOS_CONSOLE_TX_SIZE];
    console_tx_backend const semihost_console_backend = {
        semihost_tx_slot_free, semihost_tx_push, semihost_tx_irq_enable, semihost_tx_irq_disable};
}

extern "C"
{

int arch_console_write(char const* buf, size_t n)
{
    return console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
}

bool arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        semihost_putc(buf[i]);
    }
    return true;
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = KICKOS_CONSOLE_TX_SIZE;
    *irq_line = -1;
    return &semihost_console_backend;
}

}
