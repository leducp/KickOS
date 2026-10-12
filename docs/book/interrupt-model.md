<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The interrupt model: the kernel takes it, a driver serves it

> Chapter 0.2 showed how the CPU stops for an interrupt and enters a handler. This
> chapter asks the question a microkernel adds: if drivers are unprivileged tasks,
> who runs when their device interrupts? It lays out the general answers, then the
> one KickOS chose: two tiers of handler behind one dispatch seam, with the console
> as the one device the kernel keeps. It ends with a map of the sub-chapters that
> develop each piece. The exact contracts live in `../reference/architecture.md`
> ("Drivers & interrupts"), `../reference/console.md` and `../reference/porting.md`.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.2 (section 2.1.6, what
> the lowest level of the system does when an interrupt arrives) and ch.5 (section
> 5.3, the I/O software layers: interrupt handlers and device drivers).*

## The problem: the CPU only knows privileged handlers

In a monolithic kernel, a driver is kernel code. When its device interrupts, the
CPU enters the vector table, the vector names the driver's handler, and the handler
reads the device registers right there, privileged, in interrupt context. Device and
code meet in one step.

A microkernel breaks that step in two. Chapter 0.4, *[Monolithic vs
microkernel](monolithic-vs-microkernel.md)*, moved drivers out of the kernel: a
driver is an ordinary unprivileged task that holds its device's register window and
nothing else. The hardware did not move with it. On every core KickOS runs on, an
interrupt enters a handler at the privileged level, through a table the kernel
owns. An unprivileged task cannot be that handler. So the kernel must take every
interrupt first, and then get it to the task that can serve it.

The f411 example shows both sides. While the app's `printf("hello\n")` runs, two
lines can fire on the STM32F411:

- **TIM2, line 28.** The kernel's time base: its counter wraps about every 51
  seconds, and the wrap must be counted (Chapter 0.2). Nothing outside the kernel
  needs TIM2, and nothing outside it may touch it.
- **USART2, line 38.** The serial port. Its transmit-empty event says "give me the
  next byte", and its receive event says "a byte arrived". Both belong to whoever
  serves the UART, which in a microkernel should be a driver task.

The interrupt model is the set of rules that decides, for each line, who runs when it
fires, in which context, and with what authority.

## How one could deliver an interrupt

Operating systems answer this in a few recurring ways.

**Run the whole driver in the handler.** The classic monolithic answer. It has the
lowest latency, and every line of driver code joins the trusted computing base. A bug
in a sensor driver is then a bug in the kernel.

**Split the handler, keep both halves in the kernel.** Linux calls them the top half
and the bottom half: a short handler acknowledges the device, and the slow work runs
later in a softirq, a workqueue or a kernel thread (`request_threaded_irq`). The
interrupt is held off for less time, but both halves remain privileged.

**Turn the interrupt into an event for a user thread.** The kernel's handler does
almost nothing: it masks the line, signals an object, and returns. A driver thread
waiting on that object wakes, serves the device through its own register window, and
re-enables the line. MINIX 3 delivers interrupts to user drivers as notification
messages. seL4 binds an interrupt handler capability to a notification object, and
the driver acknowledges through the capability. QNX offers both forms side by side:
`InterruptAttach` runs a handler, `InterruptAttachEvent` delivers an event to a
thread.

The event form has two costs. Each interrupt now pays a context switch before the
device is served. And the line must stay masked from the kernel's handler until the
driver has served the device, because a device that still asserts its signal would
otherwise interrupt again the moment the handler returns, and the driver would never
run. That masked window has consequences of its own (Chapter 3.8).

## What KickOS chose: two tiers

KickOS uses the event form for every driver, and keeps a direct handler for the
kernel's own few devices. The kernel has one dispatch table, `irq_table`, with one
row per line, and each row is filled in one of two ways
(`kernel/include/kickos/irq.h`):

- **Tier 1, IRQ-as-event.** A task claims the line with `kos_irq_claim`. The row then
  points at `irq_event_isr`, the kernel's generic first-level handler, which masks the
  line and raises a bit in a **notification**, the object the driver thread waits on.
  The driver serves the device in thread mode, unprivileged, and its next
  `kos_notify_wait` re-arms the line. The driver never runs in handler mode.
- **Tier 2, direct kernel handler.** `irq_attach` puts a privileged kernel function in
  the row, and it runs right there in interrupt context. The handler is always
  kernel code: userspace has no way to put a function of its own in a row.

Why keep the second tier at all? Because a few devices must work when no driver
exists: during bring-up, before the first task runs, and in a panic, when the system
is dying and no thread may be trusted to run again. The kernel's handlers for those
devices are already part of the trusted computing base, since the kernel calls that
code anyway; running them directly costs no isolation. Everything else belongs to
tier 1.

A row is never empty. At boot `irq_init` fills every row with a default handler that
masks the line and counts it as spurious, so a line that fires with no owner is
silenced instead of firing forever. A line has one owner at a time, and a line the
kernel drives itself cannot be claimed at all: the claim is refused with `-KOS_EPERM`.

Some kernel lines never reach the table. A chip may vector a line straight to a
private kernel handler, and the core's own timer and trap exceptions have vectors of
their own. The table is for device lines that a driver might own.

### The f411 example: three paths

On the STM32F411 the two example lines take different paths, and a third path, the
deadline timer, never uses a device line at all.

| Source | Vector | Who runs | Context |
|---|---|---|---|
| SysTick, the core's timer | slot 15, `SysTick_Handler` | `kickos_isr_timer`, the kernel's deadline wake | handler mode, kernel |
| TIM2, line 28 | slot 44, `kickos_tim2_clock_isr` | the kernel's clock: count the wrap | handler mode, kernel |
| USART2, line 38 | slot 54, `kickos_arm_default_irq` | row 38 of `irq_table`: tier 2 or tier 1 | handler mode, then a driver thread |

TIM2 is the kernel's by declaration: `platform/stm32f411/chip.yaml` gives the device
`owner: kernel`, and `arch/arm/chip/stm32f411/startup.S` places
`kickos_tim2_clock_isr` at line 28's slot instead of the generic entry. A task that
tries `kos_irq_claim(28, ...)` is refused. Chapter 2.1, *[The time
base](the-time-base-a-monotonic-clock-you-can-trust.md)*, explains why the clock is
the one thing a kernel cannot hand out.

USART2 enters the generic path, and what its row holds depends on who owns the UART.
At boot the kernel drives it for its console (below), so row 38 is a tier-2 handler
that feeds the transmitter. When a system starts the userspace UART driver
`f4uartirq`, the kernel detaches its handler, and the driver's bring-up claims
line 38: the row becomes tier 1, and the same line now wakes a driver thread. The
line changes tier; the vector does not.

## The life of a tier-1 interrupt

Seen from far enough away, a tier-1 interrupt goes through five steps. The Journey
follows each one in the code.

1. **Claim.** A bring-up task holding the `AUTH_IRQ` authority claims the line and
   receives a capability for it. From then on, every use of the line is checked by
   possession of that capability, and the bring-up task delegates it to the driver
   (Chapter 8.0).
2. **Bind.** The line capability is bound to a notification. A notification is a word
   of event bits with one waiting thread, so one driver can wait on several lines and
   a software "doorbell" at once and tell them apart by bit.
3. **Mask and signal.** The device raises the line. The kernel's handler masks it,
   sets the bit, and marks the driver ready. No device register is touched in handler
   mode, and no capability is looked up: everything the handler needs was copied into
   the binding at bind time.
4. **Serve.** The scheduler runs the driver when its priority says so. The driver reads
   and writes the device registers with plain loads and stores, through the register
   window its task was granted. On the f411, `f4uartirq` reads USART2's status register
   at `0x40004400` and its data register at `0x40004404` directly, with no syscall in
   between.
5. **Re-arm.** The driver's next wait unmasks the line. A raise that arrived while the
   line was masked was latched by the interrupt controller and is delivered now.

Two consequences follow. First, a driver obeys the same priorities as every other
thread. A low-priority device costs a high-priority control loop only the few
instructions of step 3, never the time it takes to serve the device. Second, the
kernel's own critical sections hold device lines off. On Cortex-M the kernel lock
raises `BASEPRI` to `0x20`, and device lines sit at priority `0x30`
(`arch/arm/armv7m/regs.h`), so no device handler can cut into a kernel update.

## The dispatch seam: one entry, a first level per architecture

The kernel above the arch layer never names an interrupt controller. It sees one
entry point, `kickos_isr_irq(line)`, which looks up the row and calls it, and one set
of line operations in `arch/include/kickos/arch/arch.h`: `arch_irq_mask`,
`arch_irq_unmask`, `arch_irq_clear_pending`, `arch_irq_inject`. Everything between
the pin on the chip and that call is the arch layer's job: find out which source
fired, and call `kickos_isr_irq` once per source.

How hard that is depends on the controller:

| Family | How the first level finds the line | Where |
|---|---|---|
| ARM Cortex-M (NVIC) | one vector per line; the generic entry reads the exception number from `IPSR` and subtracts 16 | `arch/arm/common/arch_arm_common.cc`, `kickos_arm_default_irq` |
| ARM64 (GIC) | read the acknowledge register for the interrupt ID, dispatch it, write it back to end the interrupt | `arch/arm64/common/arch_arm64_gicv2.cc`, `arch_arm64_gicv3.cc` |
| RISC-V | one trap entry; the cause register says "external interrupt", then the chip reads the device's own status word, as the ESP32-C6 does for UART0 | `arch/riscv/chip/esp32c6/chip_esp32c6.cc`, `kickos_rv_ext_dispatch_dev` |
| Renesas RX | grouped interrupts share a vector; the chip snapshots the group status and dispatches one line per set bit | `arch/rx/chip/rx72m/chip_rx72m.cc`, `kickos_rx_dev_dispatch` |
| Xtensa LX6 | a CPU interrupt can carry several sources; the chip reads which ones asserted | `arch/xtensa/chip/esp32/chip_esp32.cc`, via `kickos_lx6_dispatch_dev` |

The Cortex-M row is the easy case: the hardware already did the demultiplexing, and
the vector number is the line. On the others, one hardware interrupt can stand for
several devices, or several events of one device, and the chip code must read the
right status register before it knows what to call. The rest stays fixed on every
family: the line number the kernel sees is a logical one, masking belongs to the
kernel's handler and never to the demux, and a raise on a masked line is latched,
whatever the controller.

That last rule is part of the seam, not a property every controller has for free. The
NVIC and the GIC latch a pending bit in hardware. A chip whose lines are raised from
software emulates the latch in its arch code. Chapter 3.8 explains why dropping
instead would break every driver's `wait`.

## The console: the one device the kernel keeps

The kernel needs to print: the boot banner, a fault report, a panic. It needs to do
so before any driver exists and while the system is dying, which is exactly when a
userspace driver cannot be relied on. So KickOS keeps a minimal, write-only debug
console in the kernel, the standard microkernel exception (seL4 has
`seL4_DebugPutChar` for the same reason). It is not a UART driver: it never reads,
and a real console is a userspace driver that takes the device as a capability.

The f411 example's `printf("hello\n")` ends here when no console driver is
published: newlib calls `_write`, KickOS's stdout path issues the
`kos_kconsole_write` syscall, and the kernel console sends the bytes to USART2.

**Why a ring.** The simplest console waits on the transmit-empty flag before each
byte. At 115200 baud a byte takes about 87 microseconds on the wire, so a 40-byte
line holds the CPU for about 3.5 ms. KickOS instead copies each line into a ring and
returns; the bytes leave later, one transmit-empty interrupt at a time. On the f411,
that interrupt is line 38, and the handler that drains the ring is the tier-2 row of
the previous section (`console_tx_isr`, attached by `console_buffer_init` in
`kernel/init/console_tx.cc`). A chip whose console has no transmit interrupt drains
the ring in the writer's own context instead.

**Why whole lines.** A reader parses lines. Bytes from two writers mixed inside one
line destroy it; whole lines in any order stay legible. The ring therefore takes a
line whole or refuses it, under one short critical section.

**Handing it over, and taking it back.** When a userspace console driver publishes
itself, the kernel flushes the ring, detaches its handler and stops touching the
UART; the driver then claims the line as tier 1. If the system panics, or the driver
dies, the kernel reclaims the UART and writes polled, the one path that works with no
interrupts and no scheduler. `../reference/console.md` holds the exact ownership
states and the invariants a change must keep.

## The machinery underneath: switches, stacks and traps

Every tier-1 interrupt ends in a context switch. The device fires while some thread
runs; the core saves part of that thread's state, on Cortex-M by pushing a frame on
the thread's own stack; the handler wakes the driver; and if the driver is more
urgent, the kernel switches to it before returning. On Cortex-M the switch is the
lowest-priority exception, PendSV, which runs right after the device handler without
returning to the interrupted thread in between. An event-based interrupt model is
only as cheap as that switch, which is why the switch is written in assembly against
a precise contract with each core (Chapter 3.5).

The interrupted thread's stack carries that frame, so its size must account for it,
and the kernel must be able to trust where it points (Chapter 3.6, and Chapter 7.7,
*[Whoever stacks the trap frame owns the bounds
check](whoever-stacks-the-trap-frame-owns-the-bounds-check.md)*). The
driver's access to its own registers depends on whether the protection unit can gate
a peripheral at all (Chapter 3.7). And the syscall that `printf` ends in uses the
same entry machinery on purpose: a trap is an interrupt the program asks for, and its
dispatch must run in a context that can block (Chapter 3.9).

## Reading map

- **Prerequisite:** Chapter 0.2, *[Interrupts and traps](interrupts-and-traps.md)*:
  what an interrupt, a vector table, a controller, a trap and a mask are.
- Chapter 3.5, *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)*: what a thread's saved
  state is, and why the switch that follows an interrupt must be assembly.
- Chapter 3.6, *[Thread stacks and the KISS
  tension](thread-stacks-and-the-kiss-tension.md)*: who owns the stack every
  interrupt and switch writes to, under an allocator that never frees.
- Chapter 3.7, *[Peripheral isolation and the hardware
  ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*: whether a tier-1
  driver's register window can actually be denied to everyone else, chip by chip.
- Chapter 3.8, *[A masked interrupt is latched, not
  lost](a-masked-interrupt-is-latched-not-lost.md)*: what happens to a raise during
  the masked window between the kernel's handler and the driver's re-arm.
- Chapter 3.9, *[The syscall path: trap, dispatch,
  return](the-syscall-path-trap-dispatch-return.md)*: the deliberate trap, from the
  user stub through dispatch and back.

**In the code.** The Journey's [7. An interrupt to a
driver](../journey/07-interrupt-to-a-driver.md) follows one tier-1 interrupt through
the vector, the table, the claim, the mask, the wake and the re-arm, with the f411
UART driver as its real device. [2. The first
syscall](../journey/02-first-syscall.md) follows the trap the console write takes.
The per-ISA chapters, [14](../journey/14-riscv.md), [16](../journey/16-rx.md) and
[17](../journey/17-xtensa.md), show the first level on controllers that are not one
vector per line.

**The contract.** `../reference/architecture.md` ("Drivers & interrupts");
`../reference/invariants.md` (`isr-mask-then-wake-wait-rearm`,
`irq-table-no-null-slots`, `one-driver-per-line`, `no-driver-on-a-kernel-vector`,
`device-irq-in-maskable-band`); `../reference/porting.md` for the `arch_irq_*` seam;
`../reference/console.md` for the console's pipeline, ring and handover.

## Where to go next

- Start with Chapter 3.5, *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)*, the machinery every
  later chapter of this part rests on.
- The object a driver waits on is one use of the kernel's wait and wake primitive:
  Chapter 2.2, *[The blocking substrate](the-blocking-substrate-one-wait-wake-primitive.md)*.
- How a driver received its line capability, and why that authority is a
  capability: Chapter 8.0, *[Authority comes from possession, or from
  identity](authority-from-possession-or-from-identity.md)*.
- Each ISA's realisation of the first level and the switch: Chapter 4, starting from
  *[Porting a new ISA: RISC-V](porting-a-new-isa-riscv.md)*.
