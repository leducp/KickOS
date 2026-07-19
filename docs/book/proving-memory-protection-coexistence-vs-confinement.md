<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Proving memory protection: coexistence vs confinement

> A Chapter-7 companion about VALIDATION, not mechanism. Its two
> siblings explain HOW full C++ runs under an MPU:
> [`exceptions-and-rtti-under-memory-protection.md`](exceptions-and-rtti-under-memory-protection.md)
> (what a throw/catch touches at runtime) and
> [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md)
> (the writable floor + the linker split). This chapter answers a different question:
> once you have all that, *how do you know it actually protects anything* -- and how a
> test can print "ALL PASS" while proving nothing. It binds to
> [`../reference/architecture.md`](../reference/architecture.md) (the region-set model)
> and the test sources under `user/apps/{cxxtest,mpu_fault,selftest}/`.

## The trap: "the MPU is on" is not "the MPU protects"

Consider a full-C++ smoke test -- throw, catch, unwind a local destructor, grow a
`std::vector`, `dynamic_cast`, `typeid` -- that prints `ALL PASS` under a boot banner
reading `mpu     enforce`. The tempting headline writes itself: *full C++ under MPU
enforcement, proven.* The headline is wrong, and not because a check is faulty -- every
assertion can be sound -- but because of *where the code ran*.

If the test body runs **inline in `main()`**, it runs in the **privileged root thread**,
and privileged code **bypasses the MPU**. Such a pass shows only that the runtime
*coexists* with a protection unit that happens to be switched on. It shows **nothing**
about isolation, because not one of those loads and stores was ever protection-checked.

That gap -- "the MPU is on" mistaken for "the MPU protects" -- is the lesson of this
chapter. It is a general trap, not a C++ one; it applies to any claim that a memory
boundary is enforced.

## The root thread runs your main() privileged

To see why the banner lied, you have to know what `main()` actually is on KickOS.
`main()` is not the reset entry point. The reset vector runs the kernel's own bring-up
(`kmain`), which creates two bootstrap threads -- idle, then **root** -- and starts the
scheduler. Root is the kernel's **first scheduled thread**, and it is created
**privileged** on purpose (`kernel/init/kmain.cc`: `root_attr.privileged = true`). Root's
entry (`root_entry`) runs the app/library constructors, then calls `kickos_app_main` --
your `main()`. So your `main()` is executing inside a privileged kernel thread:

```
  reset -> kmain (privileged bring-up)
             |-- create idle  (privileged)
             |-- create root  (privileged)  <-- root_attr.privileged = true
             '-- sched::start()
                     '-- root_entry: run ctors -> kickos_app_main() == your main()
                                                          ^ PRIVILEGED
```

This is by design and it is correct: root is an **orchestrator**. It has to set up and
spawn things, and on every arch the kernel and its trap handlers run privileged (ARM
handler/privileged-thread mode, RISC-V M-mode, RX supervisor) precisely *so they can*
reach kernel memory and touch the protection unit. The privileged level is the level that
**bypasses** protection -- that is what "privileged" means.

The consequence for a test author is blunt: **anything you run inline in `main()` runs
with the MPU turned off for you.** A wild write from `main()` does not fault; it lands.
`main()` is the one place in the whole system where a memory-protection test cannot
prove memory protection. If you want to prove confinement, you must leave root.

## Two independent things must both hold

"Memory protection works" is really two claims, and a rigorous test has to establish each
one separately. Passing one does not imply the other.

### 1. Confined execution -- run it unprivileged, so accesses are actually checked

The code under test must run in an **unprivileged** thread, so its every load and store is
subject to the protection unit. In KickOS that is `kos::thread::spawn(..., privileged =
false)` -- the default. `cxxtest` does exactly this: rather than run the body in `main()`,
it spawns `cxx_worker` unprivileged (`user/apps/cxxtest/main.cc`) and the
throw/catch/unwind/RTTI/STL all execute there, under the unit, reaching only the worker's
granted regions. `selftest` (`user/apps/selftest/main.cc`) is built the same way:
its workers are spawned unprivileged, and `main()` is a pure orchestrator that spawns,
joins on a semaphore, and asserts -- it never does the thing it is testing.

What "unprivileged" means concretely differs per ISA, but the intent is uniform -- the
thread runs at the CPU level where the protection unit is consulted on every access:

| ISA | Unprivileged thread runs in | Kernel / handlers run in (bypass) |
|---|---|---|
| ARM (armv7m / armv6m) | Thread mode, `CONTROL.nPRIV = 1` | Handler mode / privileged thread |
| RISC-V (rv32imac) | U-mode, `mstatus.MPP = 0` on `mret` | M-mode (`MPP = M`) |
| RX (RXv3) | user mode, `PSW.PM = 1` | supervisor (`PSW.PM = 0`) |

On every one of them the kernel deliberately sits on the bypass side. So a test that runs
in kernel/root context is testing the side that was never fenced. Only the spawned,
unprivileged worker is behind the fence.

### 2. Proving the negative -- a wild access must FAULT, not silently succeed

Confined execution is necessary but not sufficient. A test that only performs *allowed*
accesses -- reads and writes inside its own grants -- can pass **even if the protection
unit is misconfigured or absent**, because nothing it does was supposed to fault. `ALL
PASS` from a well-behaved confined worker tells you the runtime *fits inside* its grants.
It does not tell you the **boundary bites**. For that you must assert that an
**out-of-grant** access **traps**.

KickOS proves this with a dedicated binary, `mpu_fault` (`user/apps/mpu_fault/main.cc`).
An unprivileged domain-A thread writes its own granted region (must succeed), then writes
domain B's region (must fault). The kernel reports `MPU FAULT` and shuts down; the ctest
asserts that the fault marker appeared -- and negatively asserts that the "cross-domain
write completed" wording did **not** (that wording is the no-op-MPU path on
privilege-only boards). It is a *separate* binary precisely because proving the negative
*ends the process*: you cannot both assert-the-trap-fired and continue a test suite in the
same image.

A subtle detail in `mpu_fault` shows how strict "confined" has to be: the worker receives
its two region pointers through its **thread argument** (a struct placed in region A,
which it is granted), never through a file-scope global. Under real enforcement an
unprivileged thread has **no access to `.data`/`.bss`** -- so a globals-based version of
this test would fault *reading its own global* before it could even attempt the
cross-domain write, and you would misread that as the boundary biting when it was really
the setup misfiring. The only memory the worker touches is its code (granted RX), region A
(granted), and its own stack. That discipline is what makes the trap it does hit
*unambiguous*.

`selftest` carries the positive companion to `mpu_fault`'s negative: `domain_share` (two
unprivileged threads granted the *same* region read/write it and see each other's
stores), the `mpu_privileged_guard` and `confused_deputy` checks, and the unprivileged
IRQ-as-event driver that reads only its granted MMIO. Together the positive (allowed
accesses succeed from U-mode) and the negative (a disallowed access traps) are what make
"protected" a claim and not a hope.

## Coexistence vs confinement -- two claims, don't conflate them

Running a full-C++ test image with the MPU switched on establishes something real, but
weaker than the headline: the runtime -- libstdc++/libsupc++/newlib, the EH-table-homing
layout, the boot-time FDE registration on DWARF arches -- **boots and runs to completion
with enforcement active**. That is *runtime/MPU coexistence*: the enforcement machinery
does not break the runtime, and the EH tables and writable floor are homed such that a
full-C++ image links and runs on an MPU board at all. What it is *not* is proof that an
unprivileged thread is *confined* by that MPU.

So the two are stated separately, because neither implies the other:

- **Confined execution** -- the code under test runs unprivileged, so its accesses are
  actually checked, and it stays inside its grants. A spawned unprivileged `cxx_worker`
  establishes this: the fence is up and the confined code lives inside it.
- **Coexistence** -- the runtime and the enforcement are active at the same time and
  nothing breaks. A privileged run establishes this and nothing more; it says nothing
  about confinement, because the privileged side is never fenced.

And enforcement itself -- that the boundary *bites* -- is a third, independent claim that
does not lean on the C++ test at all: it rests on `mpu_fault` (the negative), on
`selftest`'s `domain_share` / guard / confused-deputy checks (the positives), and on the
unprivileged peripheral drivers that reach only their granted MMIO.

## The transferable rule

Do not run code you want to prove *confined* inline in `main()`. `main()` is the
privileged orchestrator -- it exists to spawn and set up, and it bypasses the very unit
you are trying to test. Run the code under test in a spawned **unprivileged** worker, and
then *separately* prove the boundary bites with a test whose whole job is to make a wild
access **fault**. Two tests, two claims:

- a confined worker doing only allowed things -> *the runtime fits inside its grants*;
- a confined worker doing one forbidden thing -> *the grant boundary is enforced*.

Neither implies the other, and neither is implied by a banner that says `mpu enforce`.
"The MPU is on" is a statement about configuration. "The MPU protects" is a statement you
have to *earn* -- from an unprivileged thread, with a fault you asserted.

## Where to go next

- What a confined throw actually touches at runtime (the four pieces of memory, the three
  EH models): [`exceptions-and-rtti-under-memory-protection.md`](exceptions-and-rtti-under-memory-protection.md).
- Where the writable floor comes from and how the linker splits kernel from app:
  [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md).
- The region-set model and the C++-under-MPU contract:
  [`../reference/architecture.md`](../reference/architecture.md) ("Memory domains", "C++ decisions").
- Memory protection itself, and the root/orchestrator model: Chapter 7, *Memory
  protection (M2)*.
- Further reading: Tanenbaum, *Modern Operating Systems*, ch.1 (user vs kernel mode as the
  protection boundary) -- the coexistence/confinement distinction is that boundary applied
  to your own test harness.
