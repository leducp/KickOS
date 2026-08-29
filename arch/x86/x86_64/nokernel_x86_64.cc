// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{

bool kickos_fault_kill_thread(void* frame)
{
    (void)frame;
    return false;
}

bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes)
{
    (void)frame;
    (void)bytes;
    return false;
}

uintptr_t kickos_fault_stack_top(void)
{
    return 0;
}

void kickos_fault_record(char const* status_name, uint64_t status,
                         uintptr_t pc, uintptr_t addr, int addr_valid)
{
    (void)status_name;
    (void)status;
    (void)pc;
    (void)addr;
    (void)addr_valid;
}

[[noreturn]] void kickos_thread_fault_exit(void)
{
    while (true)
    {
        __asm__ volatile("cli\n\thlt");
    }
}

[[noreturn]] void kickos_user_thread_return(void)
{
    while (true)
    {
        __asm__ volatile("cli\n\thlt");
    }
}

uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    (void)nr;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

arch_phys_addr_t kickos_frame_alloc(void)
{
    return 0;
}

void kickos_frame_free(arch_phys_addr_t frame)
{
    (void)frame;
}

}
