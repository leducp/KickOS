// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// QEMU `q35` (ICH9): the platform seam bodies.
//
// Firmware leaves the master i8259 based at vector 8, the DOUBLE FAULT vector, so a legacy
// line that asserted once would report as a double fault. legacy_pic_init remaps the pair and
// then masks it fully.

#include <kickos/arch/apic.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/arch/desc.h>
#include <kickos/arch/portio.h>
#include <kickos/arch/ring3.h>
#include <kickos/chip_com1.h>
#include <kickos/chip_q35.h>

#include <fatal_status.ld.h>

#include <stdint.h>

extern "C"
{
    // Nominal core clock (Hz). Measured at arch_init, this processor reporting none.
    uint32_t SystemCoreClock = 0;
}

namespace
{
    using namespace kickos::q35;
    using namespace kickos::x86_64;

    // The i8259 pair.
    constexpr uint16_t pic1_command = 0x20;
    constexpr uint16_t pic1_data = 0x21;
    constexpr uint16_t pic2_command = 0xa0;
    constexpr uint16_t pic2_data = 0xa1;
    // ICW1: initialise, expect ICW4. ICW4: 8086 mode.
    constexpr uint8_t icw1_init_icw4 = 0x11;
    constexpr uint8_t icw4_8086 = 0x01;
    // Clear of the 32 exception vectors and of the three the local APIC delivers.
    constexpr uint8_t pic1_vector_base = 0x20;
    constexpr uint8_t pic2_vector_base = 0x28;
    constexpr uint8_t pic_mask_all = 0xff;

    // The i8254, channel 0. Its gate is tied high here, so the counter runs with no port 0x61
    // involvement, and its output reaches only the controller masked above.
    constexpr uint16_t pit_channel0 = 0x40;
    constexpr uint16_t pit_command = 0x43;
    // Channel 0, latch the current count.
    constexpr uint8_t pit_latch_ch0 = 0x00;
    // Channel 0, access lo then hi, mode 2 (rate generator), binary.
    constexpr uint8_t pit_mode2_ch0 = 0x34;
    constexpr uint32_t pit_hz = 1193182;
    constexpr uint32_t pit_period = 0x10000;
    // A bound on the calibration spin, in poll iterations: a reference that never moves must
    // cost a refused frequency and not a hang.
    constexpr uint32_t pit_spin_bound = 20000000;

    // QEMU's `isa-debug-exit` device. The exit code it produces is (status << 1) | 1.
    constexpr uint16_t debug_exit_port = 0xf4;

    // The q35 ACPI PM1a control block. BOTH FIGURES ARE HARDCODED FOR THIS EMULATED CHIP: real
    // ACPI takes the S5 sleep type from the DSDT's \_S5 object and this register's address from
    // the FADT, and nothing here reads either.
    constexpr uint16_t pm1a_control = 0x604;
    constexpr uint16_t pm1a_soft_off = (0u << 10) | (1u << 13);

    uintptr_t g_ram_base = 0;
    size_t g_ram_size = 0;
    uintptr_t g_ram_next = 0;

    void outl(uint16_t port, uint32_t value)
    {
        __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }

    void outw(uint16_t port, uint16_t value)
    {
        __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }

    void legacy_pic_init(void)
    {
        outb(pic1_command, icw1_init_icw4);
        outb(pic2_command, icw1_init_icw4);
        outb(pic1_data, pic1_vector_base);
        outb(pic2_data, pic2_vector_base);
        // Cascade: the master's line 2 carries the slave, and the slave answers as identity 2.
        outb(pic1_data, 0x04);
        outb(pic2_data, 0x02);
        outb(pic1_data, icw4_8086);
        outb(pic2_data, icw4_8086);
        outb(pic1_data, pic_mask_all);
        outb(pic2_data, pic_mask_all);
    }

    uint32_t pit_count(void)
    {
        outb(pit_command, pit_latch_ch0);
        uint32_t const lo = inb(pit_channel0);
        uint32_t const hi = inb(pit_channel0);
        return lo | (hi << 8);
    }
}

extern "C"
{

// --- The reference timebase apic_init measures against ----------------------
uint32_t kickos_x86_ref_hz(void)
{
    return pit_hz;
}

uint32_t kickos_x86_ref_spin(uint32_t want)
{
    outb(pit_command, pit_mode2_ch0);
    // A count of zero programs the full period.
    outb(pit_channel0, 0);
    outb(pit_channel0, 0);

    uint32_t last = pit_count();
    uint32_t seen = 0;
    uint32_t spin = 0;
    while (seen < want and spin < pit_spin_bound)
    {
        uint32_t const now = pit_count();
        // Counts DOWN and reloads, so the step is the wrapped difference. Valid only while
        // the poll is faster than one full period.
        seen += (last - now) & (pit_period - 1);
        last = now;
        spin++;
    }
    return seen;
}

// --- One-time bring-up ------------------------------------------------------
void arch_init(void)
{
    com1_init();
    // A zero arena is a failed handover: ring3_init and aspace_init are each handed it as a
    // range to grant, and every arm above them reports ok against an empty one.
    if (g_ram_size == 0)
    {
        com1_puts("\nx86_64 q35: the handover published no usable conventional memory\n");
        arch_shutdown(KICKOS_FATAL_STATUS);
    }
    // BEFORE the interrupt table goes live and before anything sets the interrupt flag: a
    // controller left as firmware had it delivers into vectors this port has just claimed.
    legacy_pic_init();
    desc_init();
    // AFTER desc_init, which loads a null gs selector and would zero the base this seats, and
    // BEFORE any interrupt source is armed: it programs the fast syscall entry's flag mask.
    ring3_init(arch_ram_base(), arch_ram_size());
    // AFTER ring3_init, which restores the write protection its own table edits lift, and
    // BEFORE any interrupt source is armed: adopting the live regime writes one entry into a
    // table the firmware owns.
    aspace_init(arch_ram_base(), arch_ram_size());
    apic_init();
    // Clamped: the measured figure is 64 bits wide and this global is 32, so a part above
    // 4.295 GHz would wrap and report a clock tens of times fast.
    uint64_t const tsc_hz = apic_tsc_hz();
    SystemCoreClock = 0xffffffffu;
    if (tsc_hz < 0xffffffffull)
    {
        SystemCoreClock = static_cast<uint32_t>(tsc_hz);
    }
}

// --- Console ----------------------------------------------------------------
// Raw bytes: no newline translation here, unlike com1_puts.
void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        com1_putc(buf[i]);
    }
}

void arch_console_flush_sync(void)
{
    com1_drain();
}

// --- RAM bounds -------------------------------------------------------------
// The FIRMWARE's answer: this image has no KickOS linker script and so no __kickos_ram_start.
uintptr_t arch_ram_base(void)
{
    return g_ram_base;
}

size_t arch_ram_size(void)
{
    return g_ram_size;
}

void* arch_ram_alloc(size_t size)
{
    if (size == 0 or g_ram_size == 0)
    {
        return nullptr;
    }
    size_t const rsz = arch_ram_region_size(size);
    size_t const ralign = arch_ram_region_align(size);
    arch_irq_state_t const state = arch_irq_save();
    void* out = nullptr;
    // Natural (absolute) alignment: the base must be aligned to the region size.
    uintptr_t const aligned = (g_ram_next + (ralign - 1)) & ~static_cast<uintptr_t>(ralign - 1);
    size_t const off = static_cast<size_t>(aligned - g_ram_base);
    if (aligned >= g_ram_next and off <= g_ram_size and rsz <= g_ram_size - off)
    {
        g_ram_next = aligned + rsz;
        out = reinterpret_cast<void*>(aligned);
    }
    arch_irq_restore(state);
    return out;
}

// Rule 7 (arch.h). Only the local APIC window, and only on the xAPIC path: in x2APIC mode the
// block is MSRs, and every other device here sits in the port-I/O space, which an MMIO grant
// cannot name.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    uintptr_t const lapic = apic_mmio_base();
    if (lapic == 0 or max == 0)
    {
        return 0;
    }
    out[0].base = lapic;
    out[0].size = 0x1000u;
    return 1;
}

// --- Termination ------------------------------------------------------------
// tests/lib/gate.sh READS THE CONSOLE LINE below: `isa-debug-exit` reports (status << 1) | 1
// into an 8-bit process exit code, so a status of 128 or more loses its top bit.
//
// Keep it clear of the `=== ... ===` shape: tests/static/check_panic_banners.sh reads every
// literal of that shape as a fault reporter's dump marker and holds it to tests/lib/panic.ere.
void arch_shutdown(int status)
{
    __asm__ volatile("cli" ::: "memory");
    com1_puts("\nKICKOS-EXIT status ");
    com1_dec(static_cast<uint64_t>(static_cast<unsigned int>(status) & 0xffu));
    com1_puts("\n");
    com1_drain();
    outl(debug_exit_port, static_cast<uint32_t>(status));
    // Reached only when the machine carries no exit device. Soft-off carries no status, the
    // console line above being the only report left.
    outw(pm1a_control, pm1a_soft_off);
    // Reached only where neither channel exists, and it is a WEDGE: interrupts are masked, so
    // nothing can wake this and a harness sees a timeout.
    while (true)
    {
        __asm__ volatile("hlt");
    }
}

// The shared panic/fault dead-end (kernel.h).
void kfault_terminate(void)
{
    arch_shutdown(KICKOS_FATAL_STATUS);
}

}

namespace kickos::q35
{
    void ram_publish(uintptr_t base, size_t size)
    {
        g_ram_base = base;
        g_ram_size = size;
        g_ram_next = base;
    }
}
