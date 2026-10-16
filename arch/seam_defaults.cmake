# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The seam fallbacks whose body is one plain definition. Each row is written at configure time
# to ${KICKOS_SEAM_DEFAULTS_DIR}/<name>_default.cc, a translation unit of its own under the
# lone-TU rule (arch/CMakeLists.txt).
set(KICKOS_SEAM_DEFAULTS_DIR "${CMAKE_CURRENT_BINARY_DIR}/seam_defaults")

function(kickos_seam_default name definition)
  file(CONFIGURE OUTPUT "${KICKOS_SEAM_DEFAULTS_DIR}/${name}_default.cc" CONTENT
"// Generated from arch/seam_defaults.cmake.

#include <kickos/arch/arch.h>
#include <kickos/sys/errno.h>

extern \"C\"
{
${definition}
}
" @ONLY)
endfunction()

# --- Every arch -------------------------------------------------------------

# A chip whose FIFO outlives the core truncates its last line at shutdown unless it defines its
# own, even if it never retunes.
kickos_seam_default(arch_console_flush_sync [[
void arch_console_flush_sync(void)
{
}]])

# A chip body must be idempotent straight-line register stores: kpanic_enter can reach it from
# a partial nested-fault state.
kickos_seam_default(arch_console_reclaim [[
void arch_console_reclaim(void)
{
}]])

# With no window the reclaim runs at the driver task's end, before its slain threads have
# exited: correct only while none of them can hold the console's registers.
kickos_seam_default(arch_console_reclaim_window [[
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = 0;
    *size = 0;
}]])

kickos_seam_default(arch_console_retune [[
void arch_console_retune(void)
{
}]])

kickos_seam_default(arch_console_tx_backend [[
struct console_tx_backend;

struct console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    (void)irq_line;
    return nullptr;
}]])

kickos_seam_default(arch_cpu_clock_set [[
uint64_t arch_cpu_clock_set(uint32_t target)
{
    (void)target;
    return 0;
}]])

kickos_seam_default(arch_diag_led_init [[
void arch_diag_led_init(void)
{
}]])

kickos_seam_default(arch_diag_led_set [[
void arch_diag_led_set(int on)
{
    (void)on;
}]])

kickos_seam_default(arch_irq_line_kernel_owned [[
bool arch_irq_line_kernel_owned(int line)
{
    (void)line;
    return false;
}]])

kickos_seam_default(arch_irq_route [[
void arch_irq_route(int line, uint32_t core)
{
    (void)line;
    (void)core;
}]])

kickos_seam_default(arch_fault_is_user_thread [[
bool arch_fault_is_user_thread(void*)
{
    return false;
}]])

kickos_seam_default(arch_fault_redirect_to_exit [[
void arch_fault_redirect_to_exit(void*)
{
}]])

# Never the core clock: a wrong branch clock garbles the wire, and 0 sends the driver to its
# explicit fallback.
kickos_seam_default(arch_periph_clock_hz [[
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    (void)base;
    return 0;
}]])

# Not 0: a driver whose block really is gated must fail loud instead of reading registers that
# BusFault.
kickos_seam_default(arch_periph_enable [[
int arch_periph_enable(uintptr_t base)
{
    (void)base;
    return -KOS_ENOSYS;
}]])

kickos_seam_default(arch_periph_disable [[
int arch_periph_disable(uintptr_t base)
{
    (void)base;
    return -KOS_ENOSYS;
}]])

kickos_seam_default(arch_reboot [[
int arch_reboot(uint32_t outcome)
{
    (void)outcome;
    return -KOS_ENOSYS;
}]])

# --- ARM --------------------------------------------------------------------

kickos_seam_default(arch_bitband_present [[
int arch_bitband_present(void)
{
    return 0;
}]])

# A QEMU semihosting-clock chip (mps2, nrf51) must spin instead: QEMU 10 and older freeze
# SYS_CLOCK while the core sits in WFI, so a timed sleep with every thread idle never wakes.
kickos_seam_default(arch_idle_wait [[
void arch_idle_wait(void)
{
    __asm volatile("wfi");
}]])

kickos_seam_default(arch_shutdown [[
void arch_shutdown(int status)
{
    (void)status;
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}]])

kickos_seam_default(arch_fault_chip_addr [[
bool arch_fault_chip_addr(uintptr_t*)
{
    return false;
}]])

kickos_seam_default(arch_fault_report_extra [[
void arch_fault_report_extra(void)
{
}]])

# --- RISC-V rv32imac --------------------------------------------------------

# Runs at the head of the external-doorbell trap, before the line's ISR: a chip with a
# level-triggered source de-asserts it here.
kickos_seam_default(arch_rv_ext_eoi [[
void arch_rv_ext_eoi(void)
{
}]])

# A core that traps on `csrw mcounteren` (ESP32-C6 HP core) answers 0.
kickos_seam_default(arch_rv_has_mcounteren [[
int arch_rv_has_mcounteren(void)
{
    return 1;
}]])

# A chip with a level-triggered device line defines its own, or a mask-until-ack storms.
kickos_seam_default(arch_rv_hw_mask [[
void arch_rv_hw_mask(int line)
{
    (void)line;
}]])

# Called inside the arch critical section.
kickos_seam_default(arch_rv_hw_unmask [[
void arch_rv_hw_unmask(int line)
{
    (void)line;
}]])

kickos_seam_default(kickos_rv_ext_dispatch_dev [[
void kickos_rv_ext_dispatch_dev(void)
{
}]])

# --- RXv3 -------------------------------------------------------------------

# A chip that routes a device off the dedicated INTB slots defines its own, calling
# kickos_isr_irq once per asserted source.
kickos_seam_default(kickos_rx_dev_dispatch [[
void kickos_rx_dev_dispatch(void)
{
}]])

kickos_seam_default(kickos_rx_group_arm [[
void kickos_rx_group_arm(int line, int on)
{
    (void)line;
    (void)on;
}]])

# --- sim --------------------------------------------------------------------

# In the sim archive only: a chip that forgets SystemCoreClock must fail the link.
kickos_seam_default(system_core_clock [[
uint32_t SystemCoreClock = 0;]])
