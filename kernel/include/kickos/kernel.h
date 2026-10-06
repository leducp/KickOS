// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_KERNEL_H
#define KICKOS_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/diag.h>
#include <kickos/thread.h>

namespace kickos
{
    // Kernel entry, after arch_init. Creates the idle + root threads and starts the
    // scheduler; the host argv is forwarded to the app entry (argc=0/argv=nullptr on MCU).
    int kmain(int argc, char** argv);

    // Console fan-out to every enabled backend (KICKOS_CONSOLE = chip|rtt|both|none).
    // The completed input bytes, n or fewer. A kernel caller may ignore it:
    // the console is a debug facility and a line lost to pressure is lost. The SYSCALL may
    // not, or a dropped middle chunk leaves a hole in a line whose later chunks were taken.
    int kconsole_write(char const* buf, size_t n);
    // A syscall writer keeps a half-sent AMP CRLF across short writes in its own TCB.
    // -KOS_EBUSY where the console is published and the caller's own stdout send would be
    // taken now: nothing was written. In the dark window (console_dark) it waits for the
    // reclaim or a publish and writes then, or answers -KOS_EAGAIN at once where `wait` is
    // false; -KOS_ECANCELED where the writer is cancelled.
    int kconsole_write_user(char const* buf, size_t n, bool wait);

    // Debug console (in-kernel, write-only, unbuffered). Routes via kconsole_write.
    void kputs(char const* s);
    void kprintf(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

#if KICKOS_BENCH
    // kprintf that OFFERS A REFUSED LINE AGAIN rather than losing it. The console takes a whole
    // line or none, so a burst longer than the ring outruns the wire and every line past the
    // fill is dropped; this one waits for the drain between attempts. THREAD CONTEXT ONLY, and
    // never under a lock: it spins with interrupts open so the drain ISR can run. It gives up,
    // and loses the line as kprintf would, once a whole attempt passes with nothing leaving
    // the ring.
    void kprintf_paced(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

    // A thread-fault record: the kprintf_fault lines a thread prints before its krecord_end.
    void kprintf_fault(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
    void krecord_end(void);
    // A thread slain inside its record never reaches krecord_end; its open record is dropped.
    void krecord_abandon(void);

    // Whether a live thread outside `t` holds the console's registers. Caller holds IrqLock.
    bool console_window_held_outside(Task const* t);
    // Whether [base, base+size) overlaps the console's registers while the console is published
    // for a task other than `t`. Caller holds IrqLock.
    bool console_window_withheld(uintptr_t base, size_t size, Task const* t);

    // Unrecoverable error: report and halt the system.
    void kpanic(char const* msg) __attribute__((noreturn));

#if KICKOS_DIAG_TERSE
    // The terse assert terminal. file and line arrive SEPARATELY: the file string is then one
    // literal per translation unit that every assert in it shares, and the line never becomes a
    // string at all.
    void kpanic_at(char const* file, unsigned line) __attribute__((noreturn));
#endif

    // Kernel diagnostic LED: the board's single status LED. init() at boot; set()/toggle() drive
    // it. No-op on boards with no known LED.
    void kdiag_led_init(void);
    void kdiag_led_set(bool on);
    void kdiag_led_toggle(void);

    // Build a thread. `stack_base`/`stack_size` and the TCB storage are supplied by the caller.
    // Leaves it INACTIVE: the caller publishes it with sched::add, under the same lock that
    // allocated it. A publish deferred past that lock leaves a child only a slay discards.
    void thread_create(Thread* t, void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size, ThreadAttr const& attr);

#if KICKOS_MEMORY_ENFORCED and KICKOS_HAVE_MPU and not KICKOS_HAVE_ASPACE
    struct Domain;

    // The region set thread_create would seat for a thread of `attr` on this stack in domain
    // `dom`, assembled into the kernel's one spawn staging set for the caller to judge and hand
    // thread_create through ThreadAttr::regions. Caller holds IrqLock until thread_create.
    MpuSet const* thread_regions_stage(Domain const* dom, ThreadAttr const& attr,
                                       void* stack_base, size_t stack_size);

    // For idle and root, which no spawn judges: resolves `attr.task` where it is null, stages
    // the set and seats it in `attr.regions`, and panics where this MPU would decide one of its
    // overlaps otherwise than the kernel. No spawn may stage between this and thread_create.
    void thread_regions_boot(ThreadAttr& attr, void* stack_base, size_t stack_size);
#endif

    // True iff NO live thread holds a DEV region overlapping [base, base+size). The admission
    // test behind the one-holder-per-window rule, and the console reclaim's precondition. A
    // dying thread holds its windows until its exit releases them. Callers pass a non-wrapping
    // window.
    bool dev_window_free(uintptr_t base, size_t size);
    // True iff a live thread that is not a member of `t` holds a DEV region overlapping
    // [base, base+size).
    bool dev_window_held_outside(uintptr_t base, size_t size, Task const* t);

    // Whether no held task domain and no live thread but `except` holds a data region over
    // [base, base+size) whose memory type differs from `attr`'s: one block cacheable for one
    // thread and not for another is incoherent. Caller holds IrqLock.
    bool memory_type_free(uintptr_t base, size_t size, uint32_t attr, Thread const* except);

#if KICKOS_MEMORY_ENFORCED and not KICKOS_HAVE_ASPACE
    // Whether a cacheable stack over [base, base + size) meets no region of another memory
    // type: memory_type_free's, or a memory window of the spawn's own list, which no thread
    // holds yet. Caller holds IrqLock.
    bool stack_type_free(uintptr_t base, size_t size, kos_window const* list, uint16_t n);
#endif

#if not KICKOS_HAVE_ASPACE
    // A self-grant of [base, base + size) with `attr` seated in `c`'s own region set, live
    // before the return: 0, or the refusal. Caller holds IrqLock.
    int thread_self_grant(Thread* c, uintptr_t base, size_t size, uint32_t attr);
#endif

#if KICKOS_ARCH_ARENA_DCACHE and not KICKOS_HAVE_ASPACE
    // The sync a new unprivileged thread's memory windows, its task's data and its stack owe
    // the kernel's cacheable view of their blocks (grant_sync). Caller holds IrqLock.
    void thread_region_sync(Thread const* t);
#endif

#if KICKOS_ARCH_HAS_PORTS
    // Whether ports [base, base+count) lie inside one aperture the chip states
    // (arch_port_apertures), and whether no live thread holds any of them.
    bool port_aperture_ok(uintptr_t base, size_t count);
    bool port_window_free(uintptr_t base, size_t count);
#endif

    // The region attributes a memory window's kos_window_flags ask for.
    uint32_t window_memory_attr(uint8_t flags);

    // Break `t` out of whatever it is parked on and hand it `result`, without granting it
    // whatever it was waiting for. Every WaitKind is covered, so a park is never a place a
    // thread cannot be reached. Caller holds IrqLock and `t` must be BLOCKED.
    void thread_abort_park(Thread* t, intptr_t result);

    // Raise `t`'s cancellation to `kind` and nothing else: no park is broken and nothing is
    // woken, so the caller owes a reason `t` cannot be parked. Answers whether the record moved.
    // The narrow form is what keeps thread_abort_park's wait-kind unwind off the switch
    // path's callgraph, where every reachable assert is a panic taken inside the fault record
    // being written.
    bool thread_cancel_escalate(Thread* t, uint8_t kind);

    // Raise `t`'s cancellation to `kind` and, if it is parked, break that park so it becomes
    // runnable. CANCEL_KILL leaves the thread its cleanup window and ends it at its next syscall
    // entry; CANCEL_SLAY claims its next resume in switch_to and it executes no further
    // unprivileged instruction. MONOTONIC in the enum's VALUES: a kind at or below the one
    // already recorded is discarded, so a later kill can never demote a slay.
    //
    // Idempotent, and a no-op on a thread that is already dead or dying. Caller holds IrqLock.
    void thread_cancel_kind(Thread* t, uint8_t kind);
    // thread_cancel_kind(t, CANCEL_KILL): kos_thread_kill's cooperative form.
    void thread_cancel(Thread* t);
}

// Enter the panic / fault dead-end. Call FIRST, before any dump is printed. Three
// premise-free steps, in order:
//   1. mask IRQs on this core (arch_irq_save, never restored, since we do not return),
//      so the timer/scheduler/other threads stop while the dump prints and the
//      terminal blinks (kpanic runs in THREAD context; the fault path is already
//      masked, where this is a harmless re-mask);
//   2. force the console onto the synchronous polled writer for all subsequent
//      output, whether or not this arch ever armed the buffered ring;
//   3. drain bytes already queued in the ring so the dump prints in order.
// Idempotent: safe to call again.
extern "C" void kpanic_enter(void);

// Terminal for the panic / fault dead-end, shared by kpanic and the arch fault handlers. The
// fallback blinks the diag LED forever; the host and QEMU targets override it to exit with a
// fault status. extern "C": overridden across TUs. Under KICKOS_SHUTDOWN_TO_BOOTLOADER the
// fallback hands the chip to its bootloader instead. An ARM MPU/hard fault terminates HERE,
// never through kickos_terminate.
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// The chokepoint every ORDERED terminal path goes through: the KOS_SYS_SHUTDOWN syscall,
// KOS_SYS_EXIT issued by ROOT, last-thread-out and the software fault reporter. Drains the
// buffered console, then ends the system via arch_shutdown. Under
// KICKOS_SHUTDOWN_TO_BOOTLOADER it tries arch_reboot before halting; a chip with no bootloader
// entry declines with -KOS_ENOSYS and falls through to the halt.
extern "C" void kickos_terminate(int status) __attribute__((noreturn));

#if KICKOS_DIAG_TERSE
#define KICKOS_ASSERT(cond)                               \
    do                                                    \
    {                                                     \
        if (not(cond))                                    \
        {                                                 \
            ::kickos::kpanic_at(__FILE_NAME__, __LINE__); \
        }                                                 \
    } while (0)
#else
#define KICKOS_ASSERT(cond)                     \
    do                                          \
    {                                           \
        if (not(cond))                          \
        {                                       \
            ::kickos::kpanic("assert: " #cond); \
        }                                       \
    } while (0)
#endif

// A control-flow point that must never be reached: halt LOUDLY with a diagnostic, never spin
// silently. Takes a diag.h catalogue entry, which carries its own "unreachable: " in the prose
// column. It marks a state the code believes cannot occur, where kpanic marks one it refuses to
// continue from; keeping the two spellings apart is what makes the first kind greppable.
#define KICKOS_UNREACHABLE(msg) ::kickos::kpanic(msg)

#endif
