// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The buffered console TX half of the q35 seam.

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

    console_tx_backend const q35_console_backend = {
        q35_tx_slot_free, q35_tx_push, q35_tx_irq_enable, q35_tx_irq_disable,
        &kickos_console_tx_storage};
}

extern "C"
{

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = -1;
    return &q35_console_backend;
}

}
