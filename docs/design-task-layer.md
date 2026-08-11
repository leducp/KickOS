<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# A task layer: naming the group that already exists

**Status: SPIKE.** Nothing here is implemented. This file records the decisions taken, the ones
deliberately deferred, the measured cost, and the recommended place in the order. Written against
`34c5bf7e`.

The proposal in the words that set it:

> A TASK is a set of threads. A PROCESS is a task, because it is a set of threads, but it is more
> than that: it also has a virtual memory space `0..2^n`. An MCU TASK is a set of threads that share
> a common memory domain, so there may need to be a "main" domain to share.

The claim of this spike is narrower than that framing and stronger: **the group is already in the
tree, in three places, each emulating it by hand, and each getting it slightly wrong.** The work is
to name it, not to invent it.

## 1. The layer already exists, emulated by hand

### 1.1 A thread handle passed by hand because no object names the group

`console_handover_finish` took a third argument that was the IRQ thread's handle, so that a failed
handover could kill it. **M4.8.1 has since retired that header and the tail now takes a thread SET**,
which is the shape this section argues for; the convention it was forced into read:

> `A default-constructed Handle means a single-thread driver, which released the window at its own
> death.` (`driver_bringup.h:96`)

That argument is "the other thread in my task", passed manually. The kernel has no object that could
have answered the question, so the driver answers it, per driver, by remembering.

**Three of the six multi-thread drivers get it wrong, and the mistake is silent.** Verified by
reading each bring-up tail:

| driver | threads | `console_handover_finish` call | passes a handle |
| --- | --- | --- | --- |
| `system/driver/rx72m/rxsci/rxsci.cc` | 3 | `:494` | **no** |
| `system/driver/esp32c6/c6uart/c6uart.cc` | 2 | `:408` | **no** |
| `system/driver/esp32/lx6uart/lx6uart.cc` | 2 | `:400` | **no** |
| `system/driver/mk64f/k64uartirq/k64uartirq.cc` | 2 | `:548` | yes, `irqt` |
| `system/driver/rp2xxx/rpusb/rpusb.cc` | 2 | `:517` | yes, `irqt` |
| `system/driver/xmc4800/xmcuartirq/xmcuartirq.cc` | 2 | `:364` | yes, `irqt` |

A two-thread driver that omits the handle declares itself single-thread. On a failed handover its
IRQ thread is never cancelled, so the register window it holds is never released, so the console is
never reclaimed: the exact failure the parameter exists to prevent, on the exact path that reports
it. `c6uart` is the omission the console-reclaim work surfaced; `lx6uart` and `rxsci` are the same
defect, found by this spike.

`rxsci` shows the shape is not merely under-used but under-powered: it runs **three** threads (IRQ,
rx-relay, service, spawned at `:399`, `:419`, `:459`) and the parameter carries one handle. No
per-driver discipline fixes that. Only an object that means "the rest of my group" does.

**This part is a live bug and is not gated on the task layer.** See section 9.1.

### 1.2 `kos_mem_self_grant` is the confession, and the owner's premise needs one correction

The proposal says the driver threads share their ring block through `kos_mem_self_grant`. Checked,
and it is not quite that. The two driver threads share the block by each receiving a **spawn-time
`mem` grant of the same pointer** (`c6uart.cc:349` and `:389`, both `/*mem=*/g_shared`;
`k64uartirq.cc:485` and `:531`, both `/*mem=*/ctx`). `kos_mem_self_grant` is how the **bring-up
thread**, which allocated the block, joins the same sharing set before either child exists
(`c6uart.cc:309`, `k64uartirq.cc:431`, and the same line in all seven ring-block drivers).

The correction matters, because the self-grant path is where the kernel says the quiet part out
loud. `KOS_SYS_MEM_SELF_GRANT` writes the **thread's** region set and deliberately not its domain
(`kernel/syscall/syscall.cc:820`):

> `Added to the CALLER's own region set, not to its domain: a domain is shared, and widening it
> would silently hand the same window to every sibling thread.`

So the tree already has **two levels of memory authority** and already knows the difference between
them:

- `Domain::regions` (`kernel/include/kickos/domain.h:29`), the shared level, refcounted, joined at
  create (`kernel/thread/thread.cc:87`) and released at exit (`kernel/sched/sched.cc:202`).
- `Thread::regions` (`kernel/include/kickos/thread.h:152`), the effective level, composed at create
  as domain regions plus the private stack (`thread.cc:120-135`), and extended afterwards only by
  self-grant.

**The two-level model is already correct. What is missing is a name for the upper level and a
lifetime for it that is not "whoever happened to match a dedup scan".** That is the whole proposal.

## 2. Today's grouping is a heuristic, not an intent

`domain_for` (`kernel/domain/domain.cc:127`) decides membership. For an unprivileged data-only
grant it reuses a live domain only on an exact match (`domain.cc:196-208`): `refcount > 0`, not
privileged, **exactly one region**, and that region's `base`, rounded `size` and `attr == R|W` all
equal. An MMIO-bearing grant never scans at all, by construction (`domain.cc:194`):

> `An MMIO grant is a capability and is never shared: an MMIO-carrying spawn always takes a fresh
> slot, which is what makes one grant == one domain == one thread.`

Trace a two-thread UART driver through that, since it is the tree's most common driver shape:

1. The IRQ thread asks for the ring block **and** the register window. `has_mmio` is true, the scan
   is skipped, it takes a fresh slot, and its domain holds **two** regions.
2. The service thread asks for the ring block only. It scans, and it cannot match the IRQ thread's
   domain, because that domain fails `domain_region_count(&d) == 1`. It takes **another** fresh
   slot.

**Result: what a reader calls one driver is N threads in N domains.** Two threads, two domains, both
describing the same ring block, plus the bring-up thread reaching that same block through a third
mechanism that writes no domain at all. Nothing in the kernel records that these are one unit. The
grouping that does exist is an accident of a dedup predicate: two threads are in one domain when
their grants happen to be byte-identical and singular, which for the driver shape is never.

The dedup is not wrong for what it is (a way to avoid burning `KICKOS_MAX_DOMAINS` slots on
identical grants). It is simply not an expression of intent, and it is being read as one.

## 3. What M4.7.9 cannot express

M4.7.9's rule (`docs/design-m4.7.9-fault-isolation.md` section 3):

> A fault taken in unprivileged thread context, in a thread that is not already dying, kills that
> thread and nothing else.

and its domain ruling (section 2): a co-tenant sharing the domain and behaving correctly keeps it; a
domain nobody holds is reclaimed.

That is right for threads that **happen** to share a domain. It is wrong for threads that **are**
one unit. A driver whose IRQ thread died is not viable: its service thread keeps its endpoint, keeps
draining a ring nobody fills, and keeps `recv_holders` above zero, which is precisely the state
`driver_bringup.h:104` describes as "a hang on a dark console". The survivor continuing is the bug.
Today both cases are spelled the same way, so no rule can separate them.

### Which M4.7.9 rulings survive unchanged

| ruling | anchor | under a task layer |
| --- | --- | --- |
| Fault death is thread-scoped, discriminated by CPU privilege at fault time | section 3.1 | **unchanged.** The discriminator is about who faulted, not about grouping |
| Fault-death needs no new domain, teardown or ownership code; it is a routing problem | section 2 | **unchanged**, and reinforced: `sched::exit_current` stays the single funnel |
| `KOS_EXIT_FAULT` is not join-visible; supervise by activity, not by join | section 9.2 | **unchanged.** A task layer adds no join semantics |
| The teardown freeze is `cap_teardown`, and the priority deflate is rejected | section 5.1 | **unchanged.** Per-thread sweep, per-thread cost |
| The fault record is one `g_fault`, justified by `sizeof(Thread)` | section 9.3 | **unchanged**, but its justification is unenforced. See 8.2 |
| One holder per device window | `domain.cc:172-180` | **unchanged**, and explicitly kept thread-scoped. See 5.2 |

### Which need restating

**"A co-tenant sharing the domain keeps working" becomes two rules.** A co-tenant that shares a
domain because its grant matched keeps working, exactly as today. A co-tenant that shares a **task**
dies with it, because it declared itself part of one unit. Nothing in M4.7.9's mechanism changes;
what changes is that the sentence can now distinguish its two cases, which today it cannot.

**Section 6.1's gate reasoning gains a third posture.** The four fault gates are already
posture-aware (flat versus enforcing). A task-scoped death adds "enforcing, and the faulting thread
was in a multi-thread task", which is a new arm rather than a change to the existing two.

## 4. Naming: the framing holds, with one relocation

The framing to test: `task` is the general term, `process` is the specialisation that adds an
address space, so the MMU era adds a **field** rather than a second type, and the kernel API never
says "task or process depending on your chip".

**It holds.** Three checks against the tree.

1. **`process` is entirely unclaimed.** There is no `Process` type, field, or global anywhere. The
   only backing state for the word is `Kernel::live`, an unsigned count, and "ends the process"
   means `live == 0` (`kernel/sched/sched.cc:254`). The public API already carries the disclaimer
   the specialisation would retire (`user/include/kickos/kos.h:377`):

   > `Start a thread (not a process: KickOS has one address space, isolation is by MPU +
   > privilege).`

   Under this proposal that sentence stops being a disclaimer and becomes a definition: it starts a
   thread in a task, and a task is not a process **on this chip** because its domain carries regions
   rather than an address space.

2. **The field already has its home, one level below where the framing puts it.**
   `docs/design-mmu-era-exploration.md:100-112` already ruled where an address space attaches, and
   it is not Task:

   > `Domain becomes a page-table root instead of a region array. [...] A Domain already means "the
   > memory a set of threads may touch, refcounted, joined by threads, freed at last exit". [...]
   > This is the seL4 shape: a VSpace is a first-class object a task holds a capability to.`

   That document names the group "a task" while having no task, which is independent evidence that
   the noun is missing. **Decision: the address space attaches to `Domain`, not to `Task`.** A task
   owns a domain; a domain on an MMU part owns an `arch_aspace*`. The framing survives intact,
   because the kernel API still never branches on chip class; the relocation only moves the field
   one level down, to the object the MMU spike already made the fulcrum.

3. **The `exit()` ruling and this layering are the same idea seen twice, and the task layer collapses
   them into one chip-independent rule.** The separate ruling was: `exit()` scope is chip-defined,
   ending the thread on an MCU because thread and process are the same thing there, and ending the
   whole process on an A-class part. With a task noun, both are:

   > **`exit()` ends the caller's task.**

   On an MCU, where the default grouping is one thread per task (5.3), that **is** ending the
   thread, which is today's behaviour (`KOS_SYS_EXIT` dispatch at `kernel/syscall/syscall.cc:526`
   reaching `sched::exit_current`). On an MMU part, a process is a task, so it ends the process. The
   chip-dependence was never real: it was an artifact of having no word for the thing `exit()` acts
   on. **This is the strongest argument in the spike, because it removes a chip-conditional rule
   from the ABI rather than adding a concept to the kernel.**

**Decision: the type is `Task`. There is no `Process` type, now or in the MMU era.**

### 4.1 The cost of the name: `task` already means something else

`task` appears in roughly 50 comments and, more expensively, in two shipped diagnostic strings that
gates parse (`include/kickos/diag.h:91-100`):

```
"\n=== THREAD FAULT === task '%s' killed, system continues\n"
"\nMPU FAULT: task '%s' attempted %s at %p -- reported\n"
```

Both are `%s`-substituted with `Thread::name` (`kernel/init/fault.cc:101-105`,
`kernel/init/console.cc:346-363`), and `diag.h:10-22` declares these tokens frozen. Once a Task
exists these banners are actively wrong: they name a thread and say "task".

**Decision: the banners change to `thread '%s'` as part of this work.** Cost is `diag.h` plus four
gate scripts (`tests/check_mpu_fault.sh:58,74`, `check_fault_dump.sh`, `check_rootfault.sh`,
`check_qemu_ringppb.sh`). This is not optional cleanup: leaving them is a second truth about what
the word means.

## 5. What a Task owns

### 5.1 The split

| owned by | thing | anchor today | why |
| --- | --- | --- | --- |
| **Task** | the `Domain` (the shared region set, later the address space) | `Thread::domain`, `thread.h:151` | This is the "common memory domain" the proposal names, and the level `syscall.cc:824` already refuses to widen per-thread |
| **Task** | the kill group and the death policy | nothing today | Section 6 |
| **Task** | the lifetime claim on a DEV window | `dev_window_free`, `domain.cc:172` | A window returns when the **task** dies, not when one thread does |
| **Thread** | the effective region set (domain regions + private stack + self-grants) | `Thread::regions`, `thread.h:152` | Already correct, already composed at `thread.cc:120-135` |
| **Thread** | DEV window **access** | the MMIO region in `Thread::regions` | See 5.2 |
| **Thread** | the capability table, its width, its inbound-reply bound | `Thread::caps` `thread.h:208`, `cap_width` `:215`, `cap_reply_live` `:218` | See 5.4 |
| **Thread** | priority, policy, quantum, scheduling state | `thread.h:109-129` | A task is not a scheduling entity. See 7 |

### 5.2 The DEV window stays thread-scoped, and this is a deliberate refusal

The tempting move is to make the register window task-wide, since "the driver" holds it. **Rejected.**
The service thread does not touch registers and says so (`c6uart.cc:387`):

> `No MMIO window, because a DEV window has exactly one holder.`

Making the window task-wide would silently grant register access to a thread that never asked for
it, to fix a lifetime problem. That is the failure mode the isolation principle exists to prevent:
grant the narrowest unit, refuse rather than silently widen.

**Decision: a task owns the window's LIFETIME (it is released when the task dies), and the thread
that asked owns its ACCESS.** This splits `dev_window_free`'s current conflation, and it is what
lets `console_handover_finish` drop its handle parameter without widening anything.

### 5.3 The "main domain": yes, and it is created by task creation

The proposal asks whether a task needs a domain that exists before its first thread. **It does, and
that is the change with the most leverage.**

Today the domain is a **side effect of the first thread**: `domain_for` is called from thread create
(`thread.cc:84`) and from spawn (`syscall_thread.cc:335`). There is no moment at which a group
exists and is empty, which is exactly why membership has to be inferred from grant shapes.

**Decision:**

- Task creation creates the domain, from an explicit grant, before any thread exists.
- Thread spawn either **names a task to join**, or names none.
- **Naming none creates an implicit task holding exactly that one thread.**

The third clause is what makes the whole proposal incremental. Under it, every one of the 230
existing spawn call sites keeps its exact meaning: one thread, its own group. Nothing in
`user/apps/` changes, and the dedup scan can be kept, weakened, or deleted later as a separate
question, because it is no longer load-bearing for intent.

### 5.4 `M4.7.3` used the word first, and this completes it rather than colliding

`roadmap.md:146` says M4.7.3 delivered "per-task table width, and a per-task cap on inbound
replies". Checked: **"per-task" there means per-Thread.** The width is spent once per spawn into a
`ThreadAttr` (`kernel/syscall/syscall_thread.cc:432`), and both the width and the bound are TCB
fields (`thread.h:215`, `:218`), under a comment that reads `This task's addressable capacity`.
`roadmap.md:213` even switches to "thread" mid-sentence for the same object.

**Decision: the capability table stays per-Thread. A Task does not own a table.** Reasons:

- M4.7.3's entire content is that provisioning is per-thread and asymmetric (root wide, children
  narrow, `KCAP_ROOT_CHUNKS` / `KCAP_CHILD_CHUNKS` at `cap.h:240-241`). A per-task table would undo
  it and reintroduce the fleet-wide-width problem `roadmap.md:213` calls out.
- The cap layer is already the most address-space-agnostic layer in the kernel
  (`design-mmu-era-exploration.md:64-70`) and the MMU spike's advice is to keep it that way.

So this proposal **completes** M4.7.3's vocabulary rather than colliding with its mechanism: the
word "task" gets a referent, and the thing M4.7.3 sized keeps being sized per thread. The comments
saying "per-task table" become wrong and must be reworded, which is part of 4.1's sweep.

## 6. Death semantics

Thread-scoped **fault** death stays exactly as M4.7.9 ruled it. The question is whether a task dies
when one of its threads does, and what the default is.

The proposal notes that a driver task and a pool of independent workers want opposite defaults.
**That tension dissolves once grouping is explicit, and the spike takes the simpler answer.**

**Decision: task-scoped death is the rule, with no policy flag, because under 5.3's default a task
of one thread makes it a no-op.**

- A thread that named no task is alone in its task. Task-scoped death then means "this thread dies",
  which is today's behaviour, bit for bit, for all 230 existing call sites and every worker pool.
- A thread that joined a task declared itself part of one unit. Coupling its fate is the meaning of
  having joined, not a policy on top of it.

A flag would be a knob whose default is inferable from the grouping. **Deferred, not rejected:** if a
real case appears that wants a multi-thread task with independent thread fates, add
a per-task independence flag (name proposed in the fenced sketch above) at task-create then. The tree has no such case today: the six multi-thread
drivers all want the coupled behaviour, and the worker pools are all one-thread tasks under the
default.

**What this needs that does not exist:** a group kill. `thread_kill` today only sets
`t->cancelled = true` (`kernel/syscall/syscall_thread.cc:531`) and is honoured at exactly one
cancellation point, `kos_irq_wait` (`kernel/irq/irq.cc:209`). A thread parked anywhere else never
dies. **This is new kernel work, not a refactor, and it is the single largest unknown in the
proposal.** It is also the thing that would make `console_handover_finish`'s cooperative-cancel
caveat (`driver_bringup.h:108-118`) go away rather than be documented.

## 7. What this does NOT change

Stated explicitly so this cannot be read as a rewrite.

- **Scheduling.** A task is not a scheduling entity. No gang scheduling, no task priority, no task
  quantum. Priority, policy and quantum stay per-thread (`thread.h:109-129`), and the RR/FIFO
  machinery is untouched.
- **The capability ABI.** Handle codec, table width, inbound-reply bound, `cap_teardown`: all
  per-thread, all unchanged (5.4).
- **`kos_wait_last`.** It keeps meaning "last thread in the system", backed by `Kernel::live`
  (`syscall_thread.cc:598`, `sched.cc:231`). Retargeting it at tasks is a separate question and is
  entangled with the reaper-init problem `TODO.md` already carries.
- **The single physical address space.** Nothing here introduces translation. The MMU era remains
  post-M6 and remains `design-mmu-era-exploration.md`'s subject.
- **M4.7.9's fault path.** Discriminator, redirect, fault record, teardown latency decision: all
  unchanged (section 3).
- **The one-holder-per-DEV-window rule** (5.2).
- **`sizeof(Thread)`**, if the design is the pointer swap rather than a new field (8.2).

## 8. Cost and blast radius, counted

The ABI is unstable until M6, so a correct ABI change needs no migration ceremony. The cost that
matters is call sites and TCB bytes.

### 8.1 Call sites

Counted with `(`-anchored greps over `*.c,*.cc,*.cpp,*.h,*.hpp`, declarations and wrapper bodies
subtracted.

| surface | sites | shape under 5.3's default |
| --- | --- | --- |
| `kos::thread::spawn_caps` | 147 | **no change.** Task is a defaulted parameter |
| `kos::thread::spawn` | 68 | **no change** |
| `kos_thread_spawn` | 5 | **no change** |
| `kickos::driver::spawn_unprivileged` | 6 | changed, this is the driver seam |
| kernel `thread_spawn` / `thread_create` | 4 (`syscall.cc:499`, `syscall_thread.cc:483`, `kmain.cc:233`, `:278`) | changed |
| **total spawn sites** | **230** | **~10 must change** |

Concentration worth knowing: `user/apps/common/selftest/main.cc` holds 152 of the 230. If the task
parameter were **not** defaulted, that one file is the milestone. Defaulting it is therefore not a
convenience, it is the difference between a contained change and a tree-wide one.

| surface | sites | note |
| --- | --- | --- |
| Domain code sites | 27 (of 54 raw hits; half are prose) | 4 `.cc` and 3 headers total |
| `Domain*` fields in the tree | 2 (`thread.h:151`, `thread.h:342`) | |
| `domain_ref` / `domain_release` pairs | 1 (`thread.cc:87` / `sched.cc:202`) | |
| Thread-death funnel | **1** (`sched::exit_current`, `sched.cc:186`) | sole caller of `cap_teardown` and `domain_release` |
| `sched::exit_current` callers | 3 (`thread.cc:165`, `fault.cc:133`, `syscall.cc:536`) | |
| `Handle::kill()` sites | 9 | 6 of them are the hand-rolled group kill of section 1.1 |
| Drivers spawning >1 thread | 6 of 10 | |

**The domain surface is the cheapest part and the teardown funnel is a single insertion point.** The
expensive part is section 6's group kill, which has nothing to build on.

### 8.2 `sizeof(Thread)`: measured, and the received number needs qualifying

The tree's stated budget is "exactly 256 on armv6m and 2480 on host"
(`kernel/init/fault.cc:18`, restated at `design-m4.7.9-fault-isolation.md:407` and
`docs/reference/invariants.md:118`). Measured at this tree by compiling a kernel TU's own flags with
a deliberately failing `static_assert`:

| preset | ISA | baseline | `+ uint16_t task_id` | `Domain* domain` replaced by `Task* task` |
| --- | --- | --- | --- | --- |
| `microbit` (Cortex-M0) | armv6m | **256** | 264 (+8) | **256 (+0)** |
| `picopi` (Cortex-M0+) | armv6m | **264** | 272 (+8) | **264 (+0)** |
| `sim` | host x86_64 | **2480** | 2480 (**+0**) | **2480 (+0)** |

Three findings, all load-bearing.

1. **"256 on armv6m" is really "256 on `microbit`".** `picopi` is also armv6m and is 264, because it
   gets `KCAP_RUN_CHUNKS == 2` and so compiles in the two `uint16_t` at `thread.h:217,223` that
   `microbit` does not. The budget should name the board, not the ISA.
2. **A new `uint16_t` is free on the host and costs 8 bytes on target.** `offsetof` shows the
   padding before `domain` is exactly saturated on 32-bit (`spawner_tag` at 98, `domain` at 100,
   zero slack, matching the comment at `thread.h:146`), there is zero tail padding, and
   `alignof(Thread)` is 8. On the host the 4-byte hole before an 8-aligned `Domain*` swallows it.
   **A host-only measurement would have priced this proposal as free.** On `microbit` the true cost
   is +8 per TCB across 4 TCBs (3 pool slots at `KICKOS_MAX_THREADS 2`, plus idle outside the pool
   at `thread.h:389`), so +32 bytes of `.bss` on a 16 KiB part.
3. **Nothing enforces the number.** There is no `static_assert` on `sizeof(Thread)` anywhere, and
   the literal `2480` does not appear in the repository at all. Three documents budget against a
   hand-counted comment.

**Decision: the design is `Thread::domain` replaced by `Thread::task`, reaching the domain through
the task. Size-neutral on every preset by construction, and it collapses the domain surface at the
same time.** Adding a field is rejected on the `microbit` number.

## 9. What can land incrementally

The answer is not "big bang". Four of the five steps are independently gateable, and the first two
are not even part of the milestone.

**9.1. Fix the three drivers that omit the IRQ handle. Now, and not as part of this.** `rxsci`,
`c6uart` and `lx6uart` leak a live thread and a register window on a failed handover (1.1). This is
a present bug on shipped drivers, it needs no new kernel concept, and holding it hostage to a task
layer would be the wrong trade. `rxsci`'s third thread stays unreachable by that mechanism, which is
the standing argument for the rest of this document.

**9.2. Add the missing `static_assert` on `sizeof(Thread)`.** One line, plus the board-qualified
number from 8.2. Three documents already argue from this figure; nothing checks it. Independent of
everything else here.

**9.3. Introduce `Task` with implicit one-thread-per-task grouping.** A `Task` slot pool alongside
the existing domain pool, `Thread::domain` becomes `Thread::task`, the task owns the `Domain*`, and
`domain_for` is called from task creation instead of thread creation. **No ABI change, no new
syscall, no call-site change, no behaviour change.** The gate is exactly that: the full fleet
witness is unchanged and `sizeof(Thread)` is unchanged on all three measured presets. This is a
refactor that can be proven inert, which is the right first step for core-path work.

**9.4. Explicit task creation and spawn-into-task.** New syscalls, and the drivers opt in. The six
multi-thread drivers become one task each. Gateable by a driver-level witness on the boards that
have those drivers.

**9.5. Task-scoped death and the group kill.** The largest and least certain step (section 6), and
the one that finally lets `console_handover_finish` drop its handle parameter and the 6
`Handle::kill()` sites go away. Needs its own design pass; the cooperative-cancellation limit is a
real obstacle, not a detail.

**9.6 (post-M6).** `Domain` gains an `arch_aspace*` and a task whose domain has one is a process.
Belongs to `design-mmu-era-exploration.md`, not here.

Steps 9.3, 9.4 and 9.5 can be three submilestones or one; they cannot be reordered.

## 10. Sequencing

**Recommendation: not before M4.8.1, and not in M4.8.x at all. Land it after the host unit-test
layer, as the first submilestone of M4.9 that follows it.**

Four reasons, in order of weight.

1. **M4.8.1 is finished, reviewed and silicon-witnessed on six boards, awaiting merge.** A witness is
   valid for a tree. Inserting kernel-core work that touches `Thread`, `Domain`, spawn and teardown
   ahead of that merge invalidates six board captures to gain nothing, and buys a second fleet pass.
2. **M4.8.2 and M4.8.3..N are already allocated**, to the USB CDC console and to the fleet witness
   pass plus the per-chip `arch_console_reclaim` bodies (`roadmap.md:153-154`). Banner and package
   versions are `0.<milestone>.<submilestone>` and must stay monotonic (`roadmap.md:157`), so
   numbering this into M4.8.x means renumbering allocated work. That is the exact mistake
   `roadmap.md:130-134` records having already been paid for once.
3. **A host unit-test layer is the right predecessor, not an unrelated neighbour.** This proposal
   re-plumbs thread create and thread teardown: the paths that are most expensive to witness on
   silicon and cheapest to test on a host. Step 9.3's whole claim is "provably inert", and a host
   test layer is what makes that claim cheap to prove. Landing in the other order means proving it
   with fleet passes.
4. **The driver era should finish first, because the drivers are the evidence.** M4.8.3..N is the
   fleet witness pass over the driver tier. The task layer's per-driver payoff (9.4) is much easier
   to argue against a driver tier that is complete and witnessed than against one in motion.

**Two ledger corrections this needs.** `roadmap.md`'s table carries no `M4.7.9` row despite that
milestone being merged, and no `M4.9` row at all, so the host unit-test layer has no number in the
file that `roadmap.md:130-134` designates as the numbering authority. Both should be added before
this proposal is numbered against them.

**What should not wait for any of it:** 9.1 and 9.2.

## 11. Open questions

1. **The group kill.** Cooperative cancellation is honoured at one point (`irq.cc:209`). A task-wide
   kill that is honoured only where threads happen to park is not a kill. Does this need a
   preemptive kill, which is a much larger change, or is "every blocking primitive is a
   cancellation point" sufficient and affordable? **This gates 9.5 and nothing before it.**
2. **Does the `domain_for` dedup survive?** Once grouping is explicit, the scan is no longer an
   expression of intent, but it still saves `KICKOS_MAX_DOMAINS` slots on identical grants. Keep,
   restrict to within-task, or delete. Measurable: count the domains a full fleet image actually
   allocates before deciding.
3. **What is a task's identity in the handle space?** A `Task` handle would be the natural way to
   name a group for a kill or a join, but caps are per-thread by 5.4 and adding a task-typed object
   touches the codec. Deferred until 9.4 needs it.
4. **Does `kos_wait_last` become task-scoped?** It is entangled with the reaper-init problem already
   in `TODO.md`, which is core-path work with its own number. Deliberately not answered here.
5. **the task-pool sizing symbol.** Under 5.3 the default is one task per thread, so the pool is bound
   by `KICKOS_THREAD_SLOTS` in the worst case, which on a 16 KiB part is `.bss` that must be
   accounted before 9.3 lands.
