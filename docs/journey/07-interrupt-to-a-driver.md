<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 7. An interrupt to a driver in user space

By the end of this chapter you will have followed one interrupt from the moment a device asks for
attention to the moment an unprivileged driver thread has handled it and unmasked the line.
You will see the vector table send every device line but the kernel's clock to one kernel entry,
the kernel's table of handlers with its two tiers, the line being claimed and attached to a
**notification** object, the kernel's first-level handler masking the line before it does anything else, the
driver thread waking in thread mode, and the line being re-armed. Last, you will watch the
interrupt controller retain a request that arrived while the line was masked and deliver it
when the driver unmasks the line.

The ideas live in the Book: [Interrupts and traps](../book/interrupts-and-traps.md) for the
vector table, the interrupt controller and masking, and
[A masked interrupt is latched, not lost](../book/a-masked-interrupt-is-latched-not-lost.md) for
the window between the kernel's mask and the driver's re-arm. The contract is
[Reference: Drivers & interrupts](../reference/architecture.md#drivers--interrupts). This chapter
uses the MPU grants of [06](06-shared-memory.md) and the syscall path of
[02](02-first-syscall.md).

## What runs on qemu, and what does not

The `qemu` machine runs **no userspace UART driver**. Its console
is **semihosting**: the kernel hands each byte to the debugger interface of the emulator, which
has no interrupt line at all (`arch_console_tx_backend` in `arch/common/semihost_console.cc`
answers line `-1`). There is no driver for the MPS2 UART under `system/driver/`.

What does run on qemu is the **self-test** (`user/apps/common/selftest/main.cc`), and one of its
arms, `t_irqdrv`, is a complete userspace driver with a pretend device:

- the **interrupt line** is real: NVIC line 12 (`KICKOS_IRQ_FREE_BASE + 1`; the base is the
  chip file's `free_from: 11` in `platform/mps2/chip.yaml`, generated into `chip_limits.h`), with
  its real vector, its real enable and pending bits, and the kernel's real entry path;
- the **device** is a 16-byte block of RAM, granted to the driver as its data region together
  with the two semaphores the driver posts;
- the **raise** is done by `main`, which writes a value into the block and then calls
  `kos_irq_raise`. That sets the line's pending bit in the NVIC, exactly where a device's
  interrupt signal would have set it.

From the pending bit onwards, every stop is the path a real device interrupt takes. Where a
real device does something different (Stop 11, servicing the registers), the stop shows the
`f411disco` UART driver, which is the silicon twin's real one, and says it was not run here.

The arm, trimmed:

```
    void irq_driver(void* mmio) // caps: line(WAIT)@0, note@1
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        kos_window win = {};
        if (kos_window_get(0, &win) == 0)
        {
            mmio = reinterpret_cast<void*>(win.base);
        }
        volatile int* const dev = static_cast<volatile int*>(mmio);
        kos_sem_post(irqdrv_sem(mmio, IRQDRV_READY)); // bound to the object + about to park
        for (int i = 0; i < 3; i++)
        {
            kos::notify_wait(NOTE_ALL);
            dev[1 + i] = dev[0];
            irq.ack();
            kos_sem_post(irqdrv_sem(mmio, IRQDRV_DONE));
        }
    }
```

`main` claims line 12 (`TAP_ASK_LINE`), creates a notification and attaches the line to it
(`notify_for_line`), arms the line, spawns `irqdrv` with a wait-only copy of the line
capability, the notification, and a memory window over a 24-byte block, then three times writes
`0x100 + i` into word 0 and raises the line. The driver reads the block's address from its
window and copies word 0 into word `1 + i` each time. The block holds the 16-byte device and, after it, the driver's
"ready" and "done" semaphores. On a board where a thread's window is mapped at an
address of the kernel's choosing, a pointer `main` passes would not reach the block, which is why
the driver asks `kos_window_get`. A word in the block that both threads reach is one semaphore for
both.

```
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/selftest/selftest -gdb tcp::1234 -S &
gdb-multiarch build/qemu/user/apps/common/selftest/selftest \
    -ex 'target remote :1234'
```

The self-test runs about a hundred arms before this one. To get there quickly, every recipe below
starts with `break kickos::thread_self_grant if size == 24`, `continue`, `delete`: that is
`main` granting itself the 24-byte block at the top of `t_irqdrv` (it is
[06](06-shared-memory.md) Stop 10). The stops below follow one another in a single run, as the
recipes are written.

### Stop 1: the vector
**Where:** `arch/arm/chip/mps2/startup.S`, `g_isr_vector`; `arch/arm/common/arch_arm_common.cc`,
`kickos_arm_default_irq`

On Cortex-M the **vector table** is an array of addresses. Entry 0 is the initial stack pointer,
entries 1 to 15 are the core's own exceptions, and entry `16 + n` is the handler for NVIC
line `n`. On the MPS2 machines every one of the 48 device lines but one gets the same handler:

```
    .rept KICKOS_LAYOUT_LINE_DUALTIMER_IRQ
    .word kickos_arm_default_irq
    .endr
    .word kickos_mps2_clock_isr      /* dual timer 1 (clock wrap) */
    .rept KICKOS_MAX_IRQ - KICKOS_LAYOUT_LINE_DUALTIMER_IRQ - 1
    .word kickos_arm_default_irq
    .endr
```

The one exception is line 10, the dual timer the kernel's clock counts on: its wrap interrupt goes
to the chip's own handler, `kickos_mps2_clock_isr` in `arch/arm/chip/mps2/chip_mps2.cc`. The
kernel owns that line, and a claim of it is refused. That is why the lines free for an app start
at 11.

`kickos_arm_default_irq` finds out which line it is serving from the `IPSR` register, which holds
the number of the exception being handled, and subtracts 16:

```
void kickos_arm_default_irq(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    int line = static_cast<int>(ipsr & 0x1FFu) - 16;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
}
```

The f411's `arch/arm/chip/stm32f411/startup.S` builds its 86-line table the same way, with its
TIM2 line going to `kickos_tim2_clock_isr`. A chip that needs a dedicated handler for one line
edits its own `startup.S`; the kernel does not care which vector it came through, only which
line.

**Try it:** right after `target remote`, before any `continue`. The table starts at address 0
(`VTOR`, at `0xE000ED08`, reads 0), so line 12's entry is at `(16 + 12) * 4 = 0x70`, and line
10's at `0x68`:

```
(gdb) x/wx 0x70
0x70 <g_isr_vector+112>:	0x00000871
(gdb) info symbol *(unsigned *)0x70
kickos_arm_default_irq + 1 in section .text
(gdb) info symbol *(unsigned *)0x68
kickos_mps2_clock_isr + 1 in section .text
```

The `+ 1` is the Thumb bit: on Cortex-M every code address in the table has bit 0 set.

**Why, and the contract:** [Book: Interrupts and traps](../book/interrupts-and-traps.md) (the
vector table), [Reference: Porting](../reference/porting.md) (the `arch_irq_*` seam).

### Stop 2: one table, two tiers
**Where:** `kernel/irq/irq.cc`, `kickos_isr_irq`, `irq_init`, `irq_default_handler`;
`kernel/include/kickos/irq.h`, `IrqEntry`

`kickos_isr_irq` is the kernel's single IRQ entry for device lines. On a one-core kernel it is a
table lookup and a call:

```
    ::kickos::Kernel& k = ::kickos::kernel();
    // No null check: every slot is a valid callback (the null-object default).
    ::kickos::ktrace_irq_enter(static_cast<uint16_t>(irq));
    k.irq_table[irq].handler(k.irq_table[irq].arg);
    ::kickos::ktrace_irq_exit(static_cast<uint16_t>(irq));
```

Each row of `irq_table` is a handler and an argument, and there is no null check because no row is
ever empty. At boot `irq_init` fills every row with `irq_default_handler`, whose argument is the
line number: it masks the line and counts it in `irq_spurious_count`. A line that fires with no
driver is silenced, not left to fire forever.

A row is then taken in one of two ways, the **two tiers**:

- **Tier 2, a direct kernel handler.** `irq_attach(line, handler, arg)` puts a privileged kernel
  function in the row, and it runs right there in handler mode. Only the kernel uses this. On the
  f411 the kernel's buffered console attaches the USART2 line this way
  (`console_buffer_init` in `kernel/init/console_tx.cc`, whose handler calls `console_tx_isr`) to
  drain its transmit ring. On qemu nothing attaches a line: the semihosting console has none. The
  kernel's clock line does not go through the table at all: its vector is its own (Stop 1).
- **Tier 1, IRQ-as-event.** `irq_claim` puts `irq_event_isr` in the row, with a pointer to a
  small **binding** object as its argument. `irq_event_isr` does almost nothing itself; it turns
  the interrupt into an event that wakes a thread. This is the tier of every userspace driver, and
  the rest of this chapter.

**Try it:** stop after the claim (Stop 3 shows how), then compare row 12 with an unclaimed row:

```
(gdb) p/x kickos::detail::g_instance.per_instance[0].irq_table[12]
$5 = {handler = 0xb0e5, arg = 0x20005c64}
(gdb) p/x kickos::detail::g_instance.per_instance[0].irq_table[13]
$6 = {handler = 0xb085, arg = 0xd}
(gdb) info symbol 0xb0e5
kickos::(anonymous namespace)::irq_event_isr(void*) + 1 in section .text
(gdb) info symbol 0xb085
kickos::(anonymous namespace)::irq_default_handler(void*) + 1 in section .text
```

**Why, and the contract:** [Reference: Drivers & interrupts](../reference/architecture.md#drivers--interrupts),
[Reference: Invariants](../reference/invariants.md), `irq-table-no-null-slots` and
`one-driver-per-line`.

### Stop 3: claiming the line
**Where:** `kernel/syscall/syscall.cc`, `syscall_body` (the `KOS_SYS_IRQ_CLAIM` case);
`kernel/irq/irq.cc`, `irq_claim`, `irq_line_admit`

`kos_irq_claim(line, flags, &cap)` is how a task takes a line. The syscall checks one authority,
`AUTH_IRQ`, because the caller names a bare line number and could name any. After that, the line
is a **capability** (`CAP_IRQ`) and every later use is checked by possession and rights, never by
authority again. `irq_claim` refuses a line out of range (`-KOS_EINVAL`), a line the kernel
drives itself (`-KOS_EPERM`, line 10 here), a task at its budget of bindings (`-KOS_EAGAIN`) and a
line someone already holds (`-KOS_EBUSY`), then seats a binding:

```
        b->notify = nullptr;
        b->next_signaller = NOTIFY_SIGNALLER_NONE;
        b->badge = 0;
        b->line = line;
        // The first wait over the line's bit arms it: a claim leaves it masked, so there is no
        // window in which the line is armed and unowned.
        b->needs_rearm = true;
        b->armed_once = false;
        b->trigger = IRQ_EDGE;
```

The claimer gets a capability with
`CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER`, and only then is the row of Stop 2 pointed at the binding,
with `line_bind(line, irq_event_isr, b)`. The line stays **masked**: the claim does not enable it.

In `t_irqdrv`, `main` claims, because `main` holds `AUTH_IRQ` and the driver does not. It then
gives the driver a copy of the capability with `KOS_CAP_WAIT` only. The driver can wait on the line
and acknowledge it, but cannot raise it or pass it on. A real system gives a driver its lines in
one of two ways. A driver task the composition declares gets them from the kernel at its create
(`driver_lines` in `kernel/syscall/syscall_thread.cc`), claimed into its entry thread and already
attached to its notification. A task that hosts drivers itself claims them with `kos_irq_claim`
and delegates them (`system/init/compose/driver_task.cc`).

**Try it:** `break kickos::irq_claim if $r1 == 12`, `continue`, `finish`, then look at the
binding. (The condition reads the line from `r1`: at that breakpoint gdb cannot name the
parameter yet.)

```
Breakpoint 2, kickos::irq_claim (c=0x20004288 <...>, line=line@entry=12, flags=flags@entry=0, out_cap=...) at .../kernel/irq/irq.cc:783
(gdb) finish
Value returned is $4 = 0
(gdb) p *(kickos::IrqBinding*)kickos::detail::g_instance.per_instance[0].irq_table[12].arg
$7 = {notify = 0x0 <g_isr_vector>, line = 12, next_signaller = 0 '\000', badge = 0 '\000', needs_rearm = true, trigger = 0 '\000', armed_once = false, hold_mark = 0}
```

`flags = 0` is `KOS_IRQ_EDGE`. `notify = 0x0`: the line signals nothing yet. (gdb names any
null pointer after the vector table at address 0.) `c` is `main`, at priority 2.

**Why, and the contract:** [Reference: Invariants](../reference/invariants.md),
`irq-line-is-a-capability` and `no-driver-on-a-kernel-vector`,
[Book: Authority comes from possession, or from identity](../book/authority-from-possession-or-from-identity.md).

### Stop 4: the notification, and attaching the line to it
**Where:** `kernel/notify/notify.cc`, `notify_create`; `kernel/notify/watch.cc`, `watch_irq`;
`kernel/irq/irq.cc`, `irq_watch_arm` and `bind_chain`; `kernel/include/kickos/notify.h`,
`Notification`

A **notification** is the object a driver waits on. Its working fields are four:

```
    struct Notification
    {
        // Raised bits nothing has consumed. Survives the bound thread's death, for the next
        // server.
        uint32_t pending = 0;
        // The one waiter. A second bind is refused while this is set to another thread.
        Thread* bound = nullptr;
        // What the current wait may be woken by. Set at wait entry and cleared at its exit,
        // under IrqLock. A bit raised outside it stays pending and wakes nobody.
        uint32_t accept = 0;
        // Head of the signaller chain, a biased binding pool index, or NOTIFY_SIGNALLER_NONE.
        uint8_t signallers = NOTIFY_SIGNALLER_NONE;
        // ...
    };
```

`pending` is a word of event bits. Each capability to the object can carry a **badge**, one bit
number. Whoever signals through that capability sets that bit. One driver can therefore wait on
several sources at once (two lines, and a "doorbell" a service thread rings) and tell them apart
by the bits it gets back.

`notify_for_line` in the self-test makes the line one of those sources with
`kos_watch(KOS_WATCH_IRQ, KOS_IRQ_FIRED, line, note, 0, nullptr)`. In the kernel, `watch_irq`
checks that the line capability carries `KOS_CAP_WAIT` and that the line raises no notification
yet (a second watch is `-KOS_EBUSY`), resolves the notification capability, which must carry
`KOS_CAP_SIGNAL`, and takes its badge bit (an unbadged capability means bit 0). `irq_watch_arm`
then runs `bind_chain`: it takes a reference on the object so it cannot be freed while the
binding points at it, stores the bit and a raw pointer to the object in the binding, and links
the binding into the object's chain of **signallers**. The copy matters: in interrupt context the
kernel may not look up capabilities or walk tables, so everything the first-level handler needs
is already sitting in the binding.

**Try it:** continue from Stop 3. `delete`, `break kickos::irq_watch_arm`, `continue`, `bt 4`,
`finish`, and print the binding again:

```
Breakpoint 3, kickos::irq_watch_arm (b=b@entry=0x20005c64 <...>, n=0x200059a8 <...>, notify_obj=589838, bit=0 '\000') at .../kernel/irq/irq.cc:910
#1  0x0000bed0 in kickos::(anonymous namespace)::watch_irq (c=0x20004288 <...>, events=17032, source=14614542, notify_cap=14614537) at .../kernel/notify/watch.cc:107
#2  kickos::watch_call (c=0x20004288 <...>, spec=spec@entry=33554433, source=source@entry=14614542, notify_cap=notify_cap@entry=14614537, arg=arg@entry=0, out_watch=out_watch@entry=0x0 <g_isr_vector>) at .../kernel/notify/watch.cc:178
#3  0x00006e9e in (anonymous namespace)::syscall_body (nr=<optimized out>, a0=33554433, ...) at .../kernel/syscall/syscall.cc:659
(gdb) finish
Value returned is $8 = 0
(gdb) p *(kickos::IrqBinding*)kickos::detail::g_instance.per_instance[0].irq_table[12].arg
$9 = {notify = 0x200059a8 <kickos::(anonymous namespace)::system_pools+20072>, line = 12, next_signaller = 0 '\000', badge = 0 '\000', needs_rearm = true, trigger = 0 '\000', armed_once = false, hold_mark = 482}
```

`spec` 33554433 is `0x02000001`: the kind `KOS_WATCH_IRQ` (2) in its top byte and the event
`KOS_IRQ_FIRED` (1). Frame #1's `events` is not that 1: in this optimised code gdb reads a
register already reused.

**Why, and the contract:** [Reference: Invariants](../reference/invariants.md),
`notification-is-the-delivery-object`; the `kos_notify_*` and `kos_watch` comments in
`user/include/kickos/sys.h`.

### Stop 5: arming the line
**Where:** `kernel/irq/irq.cc`, `irq_ack` and `rearm_locked`; `kernel/irq/irq_route.cc`,
`irq_line_op`; `arch/arm/common/arch_arm_common.cc`, `arch_irq_clear_pending` and
`arch_irq_unmask`

A claimed line is masked until someone **arms** it. Arming happens in `rearm_locked`, reached from
two places: the driver's next wait on the notification (Stop 6), or `kos_irq_ack`. In `t_irqdrv`,
`main` arms it with `kos_irq_ack` before spawning the driver, so a raise that comes early is not
lost. `rearm_locked`:

```
            b->needs_rearm = false;
            if (not b->armed_once or b->trigger == IRQ_LEVEL)
            {
                irq_line_op(b->line, LineOp::CLEAR, held);
            }
            b->armed_once = true;
            irq_line_op(b->line, LineOp::UNMASK, held);
```

The **first** arm also clears the controller's pending bit: anything latched before the line had
an owner belongs to nobody and must not wake the new driver. `irq_line_op` is the one place in the
kernel that may touch a line's mask (on a multicore kernel it forwards the touch to the core the
line belongs to); on this one-core build it calls the arch seam directly.

On the NVIC, unmasking means setting the line's bit in an **ISER** (interrupt set-enable)
register. Just before that, `arch_irq_unmask` writes the line's priority:

```
    kickos_arm_irq_line_prio(l);
    // ...
    __asm volatile("dsb" ::: "memory");
    reg32(NVIC_ISER0 + (l >> 5) * 4) = 1u << (l & 31);
```

The priority matters. The kernel's lock on Cortex-M (an `IrqLock`) does not disable interrupts
completely. It raises `BASEPRI` to `0x20`, which blocks every exception whose priority number is
`0x20` or higher. Device lines get `PRIO_DEVICE = 0x30` (`arch/arm/armv7m/regs.h`), so while the
kernel holds its lock no device interrupt can cut in. The NVIC resets every priority to `0x00`,
the most urgent, which `BASEPRI` cannot block; hence the write before every enable.

**Try it:** `break arch_irq_clear_pending if line == 12`, `break arch_irq_unmask if line == 12`,
`continue` twice with `bt 4`, then `finish` and read the line's priority byte and `ISER0`:

```
Breakpoint 4, arch_irq_clear_pending (line=12) at .../arch/arm/common/arch_arm_common.cc:234
#1  0x0000b120 in kickos::(anonymous namespace)::rearm_locked (b=0x20005c64 <...>, held=...) at .../kernel/irq/irq.cc:201
#2  0x0000b5ae in kickos::(anonymous namespace)::rearm_locked (held=..., b=<optimized out>) at .../kernel/irq/irq.cc:198
#3  kickos::irq_ack (c=0x20004288 <...>, cap_handle=cap_handle@entry=14614542) at .../kernel/irq/irq.cc:1014
Breakpoint 5, arch_irq_unmask (line=12) at .../arch/arm/common/arch_arm_common.cc:216
(gdb) info registers basepri
basepri        0x20                32
(gdb) finish
(gdb) x/bx 0xE000E40C
0xe000e40c:	0x30
(gdb) x/wx 0xE000E100
0xe000e100:	0x00001400
```

Bit 12 of `ISER0` (`0x1000`) is line 12, enabled. Bit 10 (`0x400`) is the kernel's clock line,
enabled since boot.

**Why, and the contract:** [Reference: Invariants](../reference/invariants.md),
`device-irq-in-maskable-band`, `irqlock-nesting-safe` and `isr-mask-then-wake-wait-rearm`,
[Book: Interrupts and traps](../book/interrupts-and-traps.md) (masking and `BASEPRI`).

### Stop 6: the driver binds and parks
**Where:** `kernel/notify/notify.cc`, `notify_bind` and `notify_wait`

The driver thread joins `main`'s task, with its memory window over the block
([06](06-shared-memory.md) Stop 5). It holds the notification capability and the wait-only line
capability, delegated at spawn ([08](08-spawn-and-caps.md)). It first **binds** itself to the
notification with `note.bind()`: an object has at most one bound thread, and the bind is what
lets that thread wait on it. Then it calls `kos::notify_wait(NOTE_ALL)`, which is
`kos_notify_wait` with an accept mask of every bit and no deadline (`KOS_NEVER`). The wait names
no capability: it waits on the object its caller is bound to.

`notify_wait` does three things under the kernel lock. It re-arms every signaller on the chain
whose badge is in the mask and that owes a re-arm (none does yet; `main`'s ack paid it). It looks
at `pending`: if a wanted bit is already set, it takes it and returns at once, without sleeping.
Otherwise it answers a poll (`KOS_POLL`) `-KOS_EAGAIN` and a deadline already passed
`-KOS_ETIMEDOUT`, and for any other limit records the accept mask and **parks** the thread:

```
            irq_signallers_rearm(n, mask, lock);
            if ((n->pending & mask) != 0)
            {
                return notify_wait_bits(take_pending(n, mask));
            }
            if (limit.poll())
            {
                return -KOS_EAGAIN;
            }
            if (ktime_limit_passed(limit))
            {
                return -KOS_ETIMEDOUT;
            }
            n->accept = mask;
            // ...
            epoch = park_queueless(ask)(c, WAIT_NOTIFY, n, lock);
            ktime_limit_arm(c, limit, lock);
            // ...
            sched::reschedule(nullptr, lock);
```

"Queueless" because there is no wait queue to join: the object has exactly one possible waiter,
the bound thread, so the object itself is where it waits. Parking and switching away are the
blocking substrate of [03](03-sleep.md).

**Try it:** `break kickos::notify_wait`, `continue`, `bt 2`, `p c->name`:

```
Breakpoint 6, kickos::notify_wait (c=0x20004400 <...>, mask=4294967295, deadline=18446744073709551615) at .../kernel/notify/notify.cc:454
#1  0x00000e0e in svc_trampoline () at .../arch/arm/armv7m/switch.S:690
(gdb) p c->name
$10 = "irqdrv\000\000\000\000\000\000\000\000\000"
```

The deadline is all ones, `KOS_LIMIT_NONE`: no deadline. The dispatch leaves for `notify_wait`
with a tail call, so gdb shows the trampoline below it.

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
the `kos_notify_wait` comment in `user/include/kickos/sys.h`.

### Stop 7: the device raises the line
**Where:** `kernel/irq/irq.cc`, `irq_raise`; `kernel/include/kickos/irq_route.h`, `irq_inject`;
`arch/arm/common/arch_arm_common.cc`, `arch_irq_inject`

This is the stop that stands in for the hardware. `main` writes `0x101` into word 0 of the block,
then `kos_irq_raise(irq)`. The kernel checks `CAP_SIGNAL` on the capability and calls
`arch_irq_inject`, which writes the line's bit into **ISPR** (interrupt set-pending):

```
    // ISPR pends independently of ISER, so a raise on a masked line latches: write it
    // unconditionally.
    reg32(NVIC_ISPR0 + (l >> 5) * 4) = 1u << (l & 31);
```

A real device sets the very same bit, through a wire into the NVIC, when its interrupt output
goes active. From here on the two cases are one.

The interrupt does not fire at once. `irq_raise` runs inside the kernel lock, with `BASEPRI` at
`0x20`, and line 12 is at priority `0x30`. The NVIC holds it **pending** until the lock is dropped
at the end of `irq_raise`, and the exception is taken right there.

**Try it:** `break arch_irq_inject if irq == 12`, `continue`, `info registers basepri`, `finish`,
then read `ISPR0` and `ISER0`:

```
Breakpoint 7, arch_irq_inject (irq=12) at .../arch/arm/common/arch_arm_common.cc:246
(gdb) info registers basepri
basepri        0x20                32
(gdb) finish
Value returned is $11 = true
(gdb) x/wx 0xE000E200
0xe000e200:	0x00001000
(gdb) x/wx 0xE000E100
0xe000e100:	0x00001400
```

Pending and enabled, and held off by `BASEPRI`.

**Why, and the contract:** the `kos_irq_raise` comment in `user/include/kickos/sys.h`,
[Reference: Invariants](../reference/invariants.md), `deferred-switch-lowest-band`.

### Stop 8: the kernel's IRQ entry masks the line
**Where:** `arch/arm/common/arch_arm_common.cc`, `kickos_arm_default_irq`; `kernel/irq/irq.cc`,
`kickos_isr_irq` and `irq_event_isr`

The lock drops, the core takes exception 28 (16 + 12), pushes the interrupted registers on the
current stack, and runs `kickos_arm_default_irq` (Stop 1), which calls `kickos_isr_irq(12)`. The
row of Stop 2 sends it to `irq_event_isr` with the binding:

```
            IrqBinding* b = static_cast<IrqBinding*>(arg);
            // ...
            irq_line_op_local(b->line, LineOp::MASK);
            if (b->notify == nullptr)
            {
                return;
            }
            (void)notify_raise(b->notify, 1u << b->badge);
```

The first thing it does is **mask** the line (clear its ISER bit). The driver will service the
device later, in thread context, and until it has, the device may still be asserting its signal.
Without the mask the line would fire again the moment this handler returns, over and over, and the
driver would never get to run. The second thing is to raise the binding's badge bit in the
notification. That is all: no device register is touched in handler mode, and no capability is
looked up.

**Try it:** `break kickos_isr_irq if irq == 12` (the kernel's clock line never comes here, but
another arm's line could), `continue`, `bt 3`, then the active bit (**IABR**, `0xE000E300`) and
the exception number in `xPSR`:

```
Breakpoint 8, kickos_isr_irq (irq=12) at .../kernel/irq/irq.cc:1123
#0  kickos_isr_irq (irq=12) at .../kernel/irq/irq.cc:1123
#1  <signal handler called>
#2  0x0000b61e in kickos::irq_raise (c=0x20004288 <...>, cap_handle=cap_handle@entry=14614542) at .../kernel/irq/irq.cc:1055
(gdb) x/wx 0xE000E300
0xe000e300:	0x00001000
(gdb) p/x $xpsr
$12 = 0x2100001c
```

Frame #2 is the closing brace of `irq_raise`: the `IrqLock` destructor lowering `BASEPRI` is the
instruction that let the interrupt in. The low byte of `xPSR`, `0x1c`, is 28.

**Why, and the contract:** [Book: A masked interrupt is latched, not lost](../book/a-masked-interrupt-is-latched-not-lost.md)
("The window nobody opened on purpose"), [Reference: Invariants](../reference/invariants.md),
`isr-mask-then-wake-wait-rearm` and `line-masking-owned-by-handler`.

### Stop 9: the raise wakes the driver
**Where:** `kernel/notify/notify.cc`, `notify_raise` and `raise_take`; `kernel/sched/sched.cc`,
`sched::wake`

`notify_raise` takes the kernel lock (it nests safely inside the handler) and calls `raise_take`:

```
            *changed = (n->pending & bit) == 0u;
            n->pending = n->pending | bit;
            Thread* const t = n->bound;
            if (t == nullptr)
            {
                return nullptr; // latched for whatever server binds next
            }
            if ((n->accept & bit) == 0u)
            {
                return nullptr; // no wait open on this bit: it stays pending
            }
            if (t->wait_notify() == n)
            {
                // Normal delivery preserves wait_result; only an early wake changes it.
                t->clear_wait_edge();
                return t;
            }
```

The bit is **always** recorded in `pending` first. Whether anybody wakes is a second question. If
no thread is bound, or the bound thread is busy and not waiting on this bit, the bit simply stays
set and the next wait takes it at once (Stop 6's early return). So a raise is never lost between a
wake and the next wait. Here `irqdrv` is parked waiting on every bit, so `notify_raise` hands it
to `sched::wake`. The driver has priority 15 and `main` 2 (a larger number is more urgent), so
the scheduler picks the driver and pends a switch.

**Try it:** `break kickos::notify_raise`, `continue`, then the object and its waiter:

```
Breakpoint 9, kickos::notify_raise (n=0x200059a8 <...>, bit=1) at .../kernel/notify/notify.cc:187
(gdb) p/x *n
$13 = {pending = 0x0, bound = 0x20004400, accept = 0xffffffff, signallers = 0x6, lend_mark = 0x0, hold_mark = 0x1e3}
(gdb) p n->bound->name
$14 = "irqdrv\000\000\000\000\000\000\000\000\000"
(gdb) p n->bound->state
$15 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) x/wx 0xE000E100
0xe000e100:	0x00000400
```

Line 12's bit in `ISER0` is already clear, only the clock line's is left: Stop 8's mask came
first. Do not use `finish` here: it would run on past
the end of the exception.

**Why, and the contract:** [Reference: Invariants](../reference/invariants.md),
`notification-is-the-delivery-object`.

### Stop 10: the driver runs, and owes a re-arm
**Where:** `arch/arm/armv7m/switch.S`, `PendSV_Handler`; `kernel/notify/notify.cc`,
`notify_wait` and `take_pending`; `kernel/irq/irq.cc`, `irq_signallers_owe_rearm`

When the IRQ handler returns, the pended switch runs at once: PendSV is the lowest-priority
exception, and it tail-chains behind the IRQ without returning to `main` in between. It swaps to
`irqdrv`'s stack and commits `irqdrv`'s MPU regions ([06](06-shared-memory.md) Stop 9). The
driver resumes inside `notify_wait`, after its park. It takes the pending bits it accepted:

```
        uint32_t take_pending(Notification* n, uint32_t mask)
        {
            uint32_t const taken = n->pending & mask;
            n->pending = n->pending & ~taken;
            irq_signallers_owe_rearm(n, taken);
            return taken;
        }
```

and marks each line that signalled one of those bits as **owing a re-arm**. Note where that flag
is set: in thread context, when the driver consumes the event, and never in the handler. A flag
set in the handler would let a quick `ack` re-enable the line before the driver had looked at the
device at all.

The line is still masked when the driver returns to user mode. It stays masked while the driver
works. That is the window the Book chapter is about.

**Try it:** `break PendSV_Handler`, `continue`, then `delete`,
`break kickos::irq_signallers_owe_rearm`, `continue`, `bt 3`:

```
Breakpoint 10, PendSV_Handler () at .../arch/arm/armv7m/switch.S:163
Breakpoint 11, kickos::irq_signallers_owe_rearm (n=n@entry=0x200059a8 <...>, taken=taken@entry=1) at .../kernel/irq/irq.cc:926
#1  0x0000bce8 in kickos::(anonymous namespace)::take_pending (n=0x200059a8 <...>, mask=4294967295) at .../kernel/notify/notify.cc:57
#2  kickos::notify_wait (c=<optimized out>, mask=4294967295, deadline=<optimized out>) at .../kernel/notify/notify.cc:522
(gdb) p/x *n
$16 = {pending = 0x0, bound = 0x20004400, accept = 0x0, signallers = 0x6, lend_mark = 0x0, hold_mark = 0x1e3}
```

The bit the raise set is gone from `pending` (the driver took it), and `accept` is 0: the wait is
over.

**Why, and the contract:** [Book: Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Book: A masked interrupt is latched, not lost](../book/a-masked-interrupt-is-latched-not-lost.md).

### Stop 11: servicing the device
**Where (qemu):** `user/apps/common/selftest/main.cc`, `irq_driver`.
**Where (f411):** `user/include/kickos/sys/uart_service.h`, `irq_loop`;
`user/src/uart_service_dev.cc`, `irq_pass`; `system/driver/stm32f411/uart_stm32f4.cc`,
`kos_uart_read`

On qemu the "device" is word 0 of the block, and servicing it is one line of user code,
`dev[1 + i] = dev[0]`, through the memory window the spawn granted. The driver runs unprivileged
and touches only memory its own MPU rows cover.

**Try it:** `break kickos::irq_ack if ((kickos::Thread*)$r0)->name[0] == 'i'` catches the driver's
acknowledgement, which comes right after the copy. (The condition reads the first argument
straight from `r0`, because at that breakpoint gdb cannot yet see the parameter by name.) Then
read the block:

```
Breakpoint 12, kickos::irq_ack (c=0x20004400 <...>, cap_handle=cap_handle@entry=1) at .../kernel/irq/irq.cc:994
(gdb) x/4wx '(anonymous namespace)::g_mmio'
0x200245a0:	0x00000101	0x00000101	0x00000000	0x00000000
```

Word 0 is what `main` wrote, word 1 is the driver's copy.

On the f411 the same place is held by a real UART driver, `f4uartirq`
(`system/driver/stm32f411/f4uartirq/f4uartirq.cc`, a few lines over the shared service). Its
interrupt thread is `irq_loop`:

```
    while (true)
    {
        // The FIRST wait is also what arms the line: a claim leaves it masked so no window
        // exists in which it is armed and unowned. One wait covers the lines and the
        // doorbell.
        waited = kos::notify_wait(KOS_UART_ACCEPT);
        if (waited != 0)
        {
            break; // bound to nothing, or cancelled: this thread has no work
        }
        // ...
        irq_pass(&dev, sh);
```

`irq_pass` moves received bytes from the device into a ring and queued bytes from a ring into the
device. The device access is plain loads and stores: `kos_uart_read` reads the USART status
register `SR`, then the data register `DR`, at `u->base + ru::SR_OFFSET` and
`u->base + ru::DR_OFFSET`. `u->base` is the USART2 register block, which the driver holds as a
**device window** ([06](06-shared-memory.md) Stop 11): an MPU row of exactly the block's size, with
device memory attributes and execute-never. No syscall stands between the driver and the device
registers. Reading `DR` also clears the device's receive flag, which drops its interrupt signal;
that is why the line must stay masked until this pass is done.

The f411 also shows the one moment the two tiers meet. Until a userspace console driver takes over,
the kernel itself drives USART2 through tier 2 (Stop 2). The driver's bring-up publishes the
console endpoint first, which makes the kernel detach its own handler (`console_tx_deinit`), and
only then claims the line, since a claim is refused while any handler other than the default holds
it.

<!-- unverified: the f411 half of this stop runs on silicon only; qemu has no UART driver. -->

**Why, and the contract:** [Book: Peripheral isolation and the hardware ceiling](../book/peripheral-isolation-and-the-hardware-ceiling.md),
[Book: The fast path is the capability](../book/the-fast-path-is-the-capability-gpio-direct-mmio.md),
[Reference: The kernel console](../reference/console.md) ("Capability handover").

### Stop 12: re-arming
**Where:** `kernel/irq/irq.cc`, `irq_ack` and `rearm_locked`; `kernel/notify/notify.cc`,
`notify_wait` (its `irq_signallers_rearm` call)

The `t_irqdrv` driver re-arms explicitly with `irq.ack()`, which is `kos_irq_ack`. That reaches
`rearm_locked` (Stop 5) again, and this time the line has been armed once and is edge-triggered,
so it does **not** clear the pending bit; it only unmasks. Clearing would throw away a raise that
arrived while the driver worked (Stop 13).

The `f411` UART loop never calls `kos_irq_ack`. Its next `kos::notify_wait` re-arms the line on the
way in, through `irq_signallers_rearm`, because Stop 10 left `needs_rearm` set. A driver that only
ever loops "wait, service" cannot forget to re-arm. `kos_irq_ack` is an early re-arm, for a
driver that wants the line open again before its next wait. Calling both is harmless: the second
finds `needs_rearm` false and does nothing.

A **level-triggered** line (claimed with `KOS_IRQ_LEVEL`, as the f411 UART is) clears the pending
bit on every re-arm. A level source keeps its signal up until the device is serviced, so if it is
still up after the clear, it simply pends again; and a stale pending bit from before the service
would wake the driver for nothing.

**Try it:** continue from Stop 11: `delete`, `break arch_irq_clear_pending if line == 12`,
`break arch_irq_unmask if line == 12`, `continue`, `bt 3`, `x/wx 0xE000E100`, `finish`,
`x/wx 0xE000E100`:

```
Breakpoint 14, arch_irq_unmask (line=12) at .../arch/arm/common/arch_arm_common.cc:216
#1  0x0000b5ae in kickos::(anonymous namespace)::rearm_locked (held=..., b=<optimized out>) at .../kernel/irq/irq.cc:198
#2  kickos::irq_ack (c=0x20004400 <...>, cap_handle=cap_handle@entry=1) at .../kernel/irq/irq.cc:1014
(gdb) x/wx 0xE000E100
0xe000e100:	0x00000400
(gdb) finish
(gdb) x/wx 0xE000E100
0xe000e100:	0x00001400
```

`arch_irq_clear_pending` was not hit: this is the second arm of an edge line. `irq_ack` is called
by `c=0x20004400`, the driver, through its own line capability in slot 1, not by `main`
(`0x20004288`) as in Stop 5.

**Why, and the contract:** [Reference: Invariants](../reference/invariants.md),
`isr-mask-then-wake-wait-rearm`; the `kos_irq_ack` and `kos_irq_discard` comments in
`user/include/kickos/sys.h`.

### Stop 13: a raise while masked is latched, not lost
**Where:** `user/apps/common/selftest/main.cc`, `t_irq_mask` and `mask_driver`;
`arch/arm/common/arch_arm_common.cc`, `arch_irq_inject` and `arch_irq_unmask`

The arm right after `t_irqdrv`, `t_irq_mask`, holds the masked window open on purpose. Its driver
services one raise on line 11 and then stops before its ack, so the line stays masked. `main` raises
the line twice in that window, then lets the driver ack.

Nothing in the kernel stores those two raises. The NVIC does: a pending bit in ISPR is kept
whether or not the line is enabled, and the moment `arch_irq_unmask` sets the enable bit, the
pending interrupt is taken. Two raises in the window leave one pending bit, so they arrive as one
interrupt. That is the contract: a raise during the window is **latched** and delivered once,
however many there were. The driver then has to drain everything the device has, not one item per
interrupt.

**Try it:** from the `t_irqdrv` start, `delete`, `break arch_irq_inject if irq == 11`. At each of
the three hits, `finish`, then read ISPR0 and ISER0. Then `delete`,
`break kickos_isr_irq if irq == 11`, `continue`, `bt 4`:

```
first raise (line armed, held off by the lock):
0xe000e200:	0x00000800
0xe000e100:	0x00000c00
second raise (line masked by the ISR, driver has not acked):
0xe000e200:	0x00000800
0xe000e100:	0x00000400
third raise:
0xe000e200:	0x00000800
0xe000e100:	0x00000400

Breakpoint 3, kickos_isr_irq (irq=11) at .../kernel/irq/irq.cc:1123
#1  <signal handler called>
#2  0x0000b59a in kickos::irq_ack (c=0x20004400 <...>, cap_handle=cap_handle@entry=1) at .../kernel/irq/irq.cc:1016
#3  0x00007288 in (anonymous namespace)::syscall_body (nr=<optimized out>, a0=1, ...) at .../kernel/syscall/syscall.cc:1211
```

The latched interrupt is taken inside the driver's own `kos_irq_ack`, as its lock drops. The
self-test then checks that the driver ran exactly twice: once for the first raise, once for the
two coalesced ones.

**Why, and the contract:** [Book: A masked interrupt is latched, not lost](../book/a-masked-interrupt-is-latched-not-lost.md)
("Latch is the answer", "How many events is a burst? Exactly one").

## What changes elsewhere

- **On the f411** the stops are the same; Stop 11 is a real device, and the console line moves
  from tier 2 to tier 1 at the console handover ([Reference: The kernel console](../reference/console.md)).
- **Above one kernel core** ([11. Multicore](11-multicore.md)): each claimed line is routed to the
  core it was claimed on (`arch_irq_route`), the claimer and every waiter must be pinned to that
  core, and `irq_line_op` forwards a mask or unmask to the line's own core
  (`kernel/irq/irq_route.cc`). The dispatch table becomes a published record, read under an epoch
  so a line can be released while another core is still in its handler.
- **On RISC-V and Xtensa** ([14](14-riscv.md), [17](17-xtensa.md)) the first level is not a
  per-line vector. The arch demuxes the physical source and calls the same
  `kickos_isr_irq(line)`; masking stays the handler tier's job. **On RX** ([16](16-rx.md)) every
  vector a line can claim has its own entry, which calls `kickos_isr_irq` with that vector, and
  the group vectors, the kernel's, demux their sources the same way.
- **With an MMU** ([10. The MMU](10-the-mmu.md)): the device window is a page mapping with device
  attributes instead of an MPU row. Nothing else in this chapter changes.

## Where to next

- [8. Spawn and capabilities](08-spawn-and-caps.md): how the driver received its wait-only line and its
  notification.
- [9. Fault and kill](09-fault-and-kill.md): what happens to a line when its driver dies (the
  binding is released and the line masked, while the notification's pending bits stay for the next
  server).
- [5. Call and reply](05-call-and-reply.md): a server can accept notification bits in the same
  `kos_reply_recv` it serves requests with.
- [Reference: Drivers & interrupts](../reference/architecture.md#drivers--interrupts) and
  [Reference: Invariants](../reference/invariants.md), the `irq-*` and `isr-*` entries.

## Exercises

1. Delete the `kos_irq_ack(irq)` line `main` runs before spawning the driver in `t_irqdrv`.
   Predict whether the first raise is still delivered, and who arms the line. Check with
   `break arch_irq_unmask` and `bt 3`.
2. Claim `t_irq_mask`'s line with `KOS_IRQ_LEVEL` instead of `KOS_IRQ_EDGE` (the claim is in
   `irq_claim_await`, `user/apps/common/selftest/selftest_common.cc`). Predict whether the two
   raises in the masked window are still delivered after the ack. Check with
   `break arch_irq_clear_pending if line == 11` and the ISPR read of Stop 13.
3. In Stop 9, `accept` was `0xffffffff`. Predict what happens if the driver waits with an accept
   mask of `2` while the line's badge is bit 0. Where does the raise go, and when is it seen?
   Check with `p/x *n` at `kickos::notify_raise`.
