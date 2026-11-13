// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The privilege window witness. The gate's debugger stops this thread inside a kernel window
// and pends a switch there, so its context is saved wherever it then rests; where that is the
// thread's own stack, the debugger rewrites the stacked PC to pw_payload, as a task-mate could.
// The payload reports the privilege it resumed with. Nothing may resume privileged at a PC
// that memory chose.
//
// KICKOS_PRIVWINDOW_MODE
//   0  the syscall return, between its move back onto the thread's stack and its privilege
//      drop. main loops a slow syscall with the debugger armed.
//   1  the fault exit: main faults and the debugger stops at the first instruction the
//      contained fault's return lands on.
//   2  the register IPC trap entered with PendSV already pended, which sends it to the slow
//      path from the fastpath's own refusals. The call must answer as the generic dispatch.
//   3  mode 0 with nothing rewritten: the context saved demoted in the syscall return must
//      resume there unprivileged and hand back the syscall's own result.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/abi.h>

#if !defined(__arm__)
#error "privwindow reads ARM system registers; it must not be built for another ISA"
#endif
#ifndef KICKOS_PRIVWINDOW_MODE
#error "KICKOS_PRIVWINDOW_MODE must be set by this app's CMakeLists"
#endif

// The debugger's break condition reads it, so no kernel path reached before the arm counts.
extern "C"
{
    __attribute__((used)) volatile uint32_t g_pw_armed = 0;
}

extern "C" __attribute__((noreturn, used)) void pw_report(uint32_t ipsr, uint32_t control)
{
    if (ipsr != 0u or (control & 1u) == 0u)
    {
        kos::print("[privwindow] payload: privileged\n");
    }
    else
    {
        kos::print("[privwindow] payload: unprivileged\n");
    }
    kos_exit(0);
}

extern "C" void pw_payload(void);
extern "C" int32_t arch_syscall_reg(uint32_t* io);
__asm__(".section .text.pw_payload,\"ax\",%progbits\n"
        ".global pw_payload\n"
        ".type pw_payload, %function\n"
        ".thumb_func\n"
        "pw_payload:\n"
        "    mrs   r0, ipsr\n"
        "    mrs   r1, control\n"
        "    ldr   r2, =pw_report\n"
        "    bx    r2\n"
        "    .ltorg\n"
        ".size pw_payload, . - pw_payload\n"
        ".previous\n");

int main(int, char**)
{
    // Named so the linker keeps it: the debugger takes its address.
    void (*volatile const payload)(void) = &pw_payload;
    (void)payload;
    kos::print("[privwindow] armed\n");
#if KICKOS_PRIVWINDOW_MODE == 2
    g_pw_armed = 2u;
#elif KICKOS_PRIVWINDOW_MODE != 3
    g_pw_armed = 1u;
#endif
#if KICKOS_PRIVWINDOW_MODE == 0
    for (uint32_t i = 0; i < 100000u; i++)
    {
        (void)kos_clock_now();
    }
    kos::print("[privwindow] ERROR: the debugger never caught the syscall return\n");
    return 1;
#elif KICKOS_PRIVWINDOW_MODE == 2
    // A capability the leaf refuses too, so the dispatch's answer is the only right one.
    uint32_t io[KOS_CALL_REG_WORDS + 3] = {};
    io[0] = KOS_SYS_CALL_REG;
    io[1] = KOS_CAP_NONE;
    int32_t const rc = arch_syscall_reg(io);
    g_pw_armed = 0u;
    if (rc != KOS_CALL_REG_FALLBACK)
    {
        kos::print("[privwindow] ERROR: the pended register call did not answer as the dispatch\n");
        return 1;
    }
    kos::print("[privwindow] call_reg: answered by the dispatch\n");
    return 0;
#elif KICKOS_PRIVWINDOW_MODE == 3
    uint64_t const before = kos_clock_now();
    g_pw_armed = 1u;
    uint64_t const at = kos_clock_now();
    g_pw_armed = 0u;
    uint64_t const after = kos_clock_now();
    uint32_t control = 0;
    __asm volatile("mrs %0, control" : "=r"(control));
    if ((control & 1u) == 0u)
    {
        kos::print("[privwindow] ERROR: the demoted resume left the thread privileged\n");
        return 1;
    }
    if (at < before or at > after)
    {
        kos::print("[privwindow] ERROR: the demoted resume lost the syscall's result\n");
        return 1;
    }
    kos::print("[privwindow] resumed: unprivileged, with the syscall's result\n");
    return 0;
#else
    __asm volatile("udf #0");
    kos::print("[privwindow] ERROR: the fault returned to its own instruction\n");
    return 1;
#endif
}
