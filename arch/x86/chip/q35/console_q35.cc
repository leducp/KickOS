// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The buffered console TX half of the q35 seam. KEEP IT IN A TRANSLATION UNIT OF ITS OWN: it
// calls into the kernel, and the x86_64 bring-up images link this chip's archive with no
// kernel at all, so arch_console_write must sit in a member they do not extract.

#include <kickos/arch/arch.h>
#include <kickos/chip_com1.h>
#include <kickos/console_tx.h>

#include <stddef.h>
#include <stdint.h>

namespace
{
    using namespace kickos::q35;

    // com1_init masks the part's own interrupt and legacy_pic_init masks the i8259 pair, so
    // no TX event can fire: irq_line is -1 and the producer drains.
    int q35_tx_slot_free(void) { return com1_slot_free(); }
    void q35_tx_push(uint8_t b) { com1_push(b); }
    void q35_tx_irq_enable(void) {}
    void q35_tx_irq_disable(void) {}

    char console_tx_buf[KICKOS_CONSOLE_TX_SIZE];
    console_tx_backend const q35_console_backend = {
        q35_tx_slot_free, q35_tx_push, q35_tx_irq_enable, q35_tx_irq_disable};
}

extern "C"
{

int arch_console_write(char const* buf, size_t n)
{
    return console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = KICKOS_CONSOLE_TX_SIZE;
    *irq_line = -1; // no TX event can fire here; the producer drains
    return &q35_console_backend;
}

}
