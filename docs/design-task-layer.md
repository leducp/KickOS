<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# A task layer: naming the group that already exists

> **Status: LANDED**
Steps 9.1 through 9.5 have all LANDED; only 9.6 remains and it belongs to
the MMU era. This file records the decisions taken, the ones
deliberately deferred, the measured cost, and where the ruling turned out to be wrong. Written
against `34c5bf7e`; the 9.4/9.5 annotations were added at `M4.8.3-task-layer`.

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

**THE TABLE ABOVE IS STALE AS OF M4.8.1 AND IS KEPT ONLY AS THE ARGUMENT.** None of those six files
contains `console_handover_finish` any more and none of those line numbers resolves: the generic
driver service moved the tail into `user/include/kickos/sys/driver_service.h`, where ONE call site
passes a `ThreadSet` accumulated from every spawn. A driver can no longer under-report a peer,
because no driver writes the call. So step 9.1 below is DISCHARGED, not merely eased, and `rxsci`'s
third thread is expressible too. What survives is the naming argument, not the bug.

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

**Decision: the banners change to `thread '%s'` as part of this work.** Cost is `diag.h` plus the
token consumers, and **the list of four given here was short**: the real set reached
`tests/lib/gate.sh` (whose `thread_fault_re` is how four of the scripts see the token),
`tests/lib/panic.ere`, `check_qemu_panicgate.sh` and three scripts' literal `MPU FAULT: task '...'`,
plus SIX source comments that quote it verbatim, across two apps, one arch backend and the RISC-V fault reporter. What must NOT change is the same string
inside an archived silicon capture: editing those would falsify what a past image printed. This is not optional cleanup: leaving them is a second truth about what
the word means.

## 5. What a Task owns

### 5.1 The split

| owned by | thing | anchor today | why |
| --- | --- | --- | --- |
| **Task** | the `Domain` (the shared region set, later the address space) | was `Thread::domain`, now `Thread::task` | This is the "common memory domain" the proposal names, and the level `syscall.cc:824` already refuses to widen per-thread |
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

**HOW 9.4 delivered that, and the correction the wording needed.** The window is now a region of the
asking THREAD (`thread_create` composes it beside the private stack) and `domain_for` no longer takes
an MMIO argument at all, so ACCESS is thread-scoped by construction. The LIFETIME half is not a
second mechanism: it is 9.5. Nothing in the window's own bookkeeping makes it outlive one thread --
what makes it come back with the GROUP is that a member's death cancels its peers, so the holder dies
too. Read as a claim about where the window is STORED, "the task owns the lifetime" would have argued
for putting it in the domain, which is exactly what 5.2 refuses. The two halves therefore live in
different steps and the sentence is only true once both have landed.

Three consequences worth stating, because each is a place a reader would expect the opposite:

- `dev_window_free` scans live THREADS now, not domains, and it skips a thread that is `dying`. That
  reproduces the old timing exactly: `exit_current` used to drop the domain reference before the
  capability sweep so a supervisor woken by the sweep's EPIPE could respawn into the window at once,
  and with a thread-scoped window `dying` is the flag that does the same job.
- the DEV window's Rule 7 admission and its exclusivity check move OUT of `domain_for` and INTO
  `thread_create_call`, which is where the grant is asked for. `domain_for` keeps only the shared RAM
  grant, which is the one a task creates.
- an MMIO-bearing spawn no longer skips the dedup scan, because there is no MMIO in `domain_for` to
  skip it for. Two threads granting the same block therefore share ONE domain slot where they used to
  burn two. The regions they see are identical either way, so this is a slot economy and not a
  behaviour change -- but it does mean `KICKOS_MAX_DOMAINS` is now over-provisioned rather than tight.

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

What M4.7.9 ruled about **fault** death is untouched: a fault is CONTAINED rather than a panic, and
the arch seam decides only whether the fault is a thread's own. What a death does to the REST of the
dying thread's task is this section's question, and the answer here is a REVISION of the one this
document originally gave.

### The original ruling, and the premise it rested on

> **Decision: task-scoped death is the rule, with no policy flag, because under 5.3's default a task
> of one thread makes it a no-op.**
>
> - A thread that named no task is alone in its task. Task-scoped death then means "this thread
>   dies", which is today's behaviour, bit for bit, for all 230 existing call sites and every worker
>   pool.
> - A thread that joined a task declared itself part of one unit. Coupling its fate is the meaning
>   of having joined, not a policy on top of it.

The second bullet is the decision. The first is the reason no flag was needed, and it is a claim
about GROUPING rather than about death: it holds only while a thread that names no task is alone in
one. M6.2/T5c rules that a plain spawn is `pthread_create` -- a thread of the SPAWNER's task,
sharing its address space -- so that premise is gone, and with it the argument that task-scoped
death is free. The first worker to return would end its parent.

### The ruling, revised at M6.2/T5c

**A FAULT ends the whole task. Every death a caller asked for ends one thread, and the task ends
when its last member leaves.**

The line is not drawn between kinds of cancellation. It is drawn at whether anyone is WATCHING the
death:

- **A fault** has no caller. Nobody chose it, nobody is positioned to clean up after it, and the
  faulting thread's siblings share the address space it was writing when it died. This is the
  fault-isolation property and it is the half that must not weaken: every group whose members had
  coupled fates before still has them, because such a group is exactly an explicit task. It
  propagates `CANCEL_KILL`, the kind a fault already carried, so a peer keeps the window it holds
  long enough to quiet its device.
- **An ordinary return** is one thread finishing. Its siblings end at their own returns, and the
  task ends at `task_release`'s last decrement, where it always ended.
- **A cooperative kill** (`CANCEL_KILL`) and **a slay** (`CANCEL_SLAY`) are aimed at ONE NAMED
  THREAD by a caller holding its handle. `kos_task_kill` and `kos_task_slay` are the verbs for the
  group and each cancels every member itself, before the first victim reaches `exit_current`. So
  propagating a victim's kind from its own exit adds nothing to the group paths, and it would
  collapse `kos_thread_slay` into `kos_task_slay`: a parent could no longer stop one uncooperative
  worker without dying with it. Under the pthread reading this ruling adopts, cancelling a thread
  does not cancel its peers.

**`exit_current` cannot derive this from the thread.** A fault sets no `cancel_kind`, so a contained
fault and an ordinary return arrive identical; the fault path says which it is
(`sched::EXIT_FAULTED`). Deriving it from the exit CODE instead would put the boundary inside a
number an app may hand to `kos_exit`.

**A flag would be a knob whose default is inferable from the grouping. Deferred, not rejected:** if a
real case appears that wants a multi-thread task whose members' FAULTS do not couple, add
a per-task independence flag (name proposed in the fenced sketch above) at task-create then. The tree has no such case today: the six multi-thread
drivers all want the coupled behaviour.

### What the revision costs the witnesses

An arm that faulted a plain-spawned worker and watched root survive was not witnessing containment;
it survived because every thread was accidentally its own task, and it could not tell "died alone"
apart from "ended a one-member group". Such an arm now spawns its victim into a task of its own
through `kos_task_create`, which is the shape that makes containment observable at all.

**What this needs that does not exist:** a group kill. `thread_kill` today only sets
`t->cancelled = true` (`kernel/syscall/syscall_thread.cc:531`) and is honoured at exactly one
cancellation point, `kos_irq_wait` (`kernel/irq/irq.cc:209`). A thread parked anywhere else never
dies. **This is new kernel work, not a refactor, and it is the single largest unknown in the
proposal.** It is also the thing that would make `console_handover_finish`'s cooperative-cancel
caveat (`driver_bringup.h:108-118`) go away rather than be documented.

### 6.1 What 9.5 built, and the answer to open question 1

The question was whether a real kill needs PREEMPTION, or whether "every blocking primitive is a
cancellation point" is enough. **Neither, and the framing was the problem: it conflated the place a
park ENDS with the place a thread DIES.** Separating them costs one test in one function.

- **The park ends wherever it is.** `thread_abort_park` (`kernel/thread/park.cc`) is TOTAL over
  `WaitKind`. Its arms are not wakes: a mutex waiter does not acquire the mutex and the owner's
  inherited priority is reverted, a semaphore's count is untouched so a later post still hands its
  token to a genuine waiter, an endpoint park goes through the same `endpoint_wait_abort` a deadline
  expiry uses, and a sleeper's deadline is dropped. The result handed over is -KOS_ECANCELED where
  the primitive has a code to carry one, and IGNORED where it has none -- which is the point: the
  wake is a nudge towards the death point, not a report.
- **The thread dies at its next syscall ENTRY.** One test in `syscall_dispatch`. That is what reaches
  a thread parked in `sem_wait` or `sleep`, which have no error return to cooperate through, and a
  thread that read the code and carried on anyway.
- **Entry and deliberately not exit.** On exit it would pre-empt the one window the broken park
  exists to give: a driver returning from a cancelled `irq_wait` gets to quiet its device over the
  window it already holds before the next syscall ends it.

**The residual, stated because it is real:** a thread that never enters the kernel again never dies.
A pure compute loop is unreachable without preemption, and preemption remains a much larger change
(it needs a safe point at which another thread may run a stranger's teardown). Every thread shape in
the tree loops through a syscall, so this is a documented floor rather than a live gap -- and 0 from
`kos_thread_kill` or `kos_task_kill` means the request was ACCEPTED, never that the thread is gone.

**One guard, not two.** `task_cancel_group` deliberately takes no "except this thread" argument. The
only caller with a thread to spare is `exit_current`, whose thread is already `dying`, and
`thread_cancel` refuses a dying thread -- which it must anyway, to stop two members that die together
from marking each other. Mutation-testing found the pair: with both guards in place, removing the
`except` argument changed no observable at all, which is what a redundant authority looks like.

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
  M7 and remains `design-mmu-era-exploration.md`'s subject.
- **M4.7.9's fault path.** Discriminator, redirect, fault record, teardown latency decision: all
  unchanged (section 3).
- **The one-holder-per-DEV-window rule** (5.2).
- **`sizeof(Thread)`**, if the design is the pointer swap rather than a new field (8.2).

## 8. Cost and blast radius, counted

The ABI is unstable until the ABI-freeze milestone, so a correct ABI change
needs no migration ceremony. The cost that
matters is call sites and TCB bytes.

### 8.1 Call sites

Counted with `(`-anchored greps over `*.c,*.cc,*.cpp,*.h,*.hpp`, declarations and wrapper bodies
subtracted.

| surface | sites | shape under 5.3's default |
| --- | --- | --- |
| `kos::thread::create_caps` | 147 | **no change.** Task is a defaulted parameter |
| `kos::thread::create` | 68 | **no change** |
| `kos_thread_create` | 5 | **no change** |
| `kickos::driver::spawn_unprivileged` | 6 | changed, this is the driver seam |
| kernel `thread_create_call` / `thread_create` | 4 (`syscall.cc:499`, `syscall_thread.cc:483`, `kmain.cc:233`, `:278`) | changed |
| **total spawn sites** | **230** | **~10 must change** |

Concentration worth knowing: `user/apps/common/selftest/main.cc` holds 152 of the 230. If the task
parameter were **not** defaulted, that one file is the milestone. Defaulting it is therefore not a
convenience, it is the difference between a contained change and a tree-wide one.

| surface | sites | note |
| --- | --- | --- |
| Domain code sites | 27 (of 54 raw hits; half are prose) | 4 `.cc` and 3 headers total |
| `Domain*` fields in the tree | 2, both now `Task*` since 9.3 | |
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

**A FOURTH FINDING, added 2026-08-11 when 9.3 landed: this table prices `Thread` and says nothing
about the POOL.** The pointer swap is genuinely free on all three presets, and the layer is not: a
`Task` pool costs 32 bytes of `.bss` on microbit and 144 on picopi. On microbit that is decisive for a
reason no byte count predicts, because `__kickos_ram_start` IS `_ebss` there and the arena granule is
32 bytes: ANY non-zero `.bss` addition moves the arena base a full granule. Measured both ways, a
4-byte `Task` and a single-slot pool move it identically. So `mem_self_grant` loses its grain and
skips, and the `uint16_t task_id` design this table rejects would have flipped the same arm rather
than a different one. The lesson for the table is that "size-neutral by construction" was a claim
about a struct being read as a claim about a change.

3. **Nothing enforces the number.** There is no `static_assert` on `sizeof(Thread)` anywhere, and
   the literal `2480` does not appear in the repository at all. Three documents budget against a
   hand-counted comment.

**Decision: the design is `Thread::domain` replaced by `Thread::task`, reaching the domain through
the task. Size-neutral on every preset by construction, and it collapses the domain surface at the
same time.** Adding a field is rejected on the `microbit` number.

### 8.3 The 9.4/9.5 mutation run

Twenty-five mutants, each applied alone to the task-scoped death and group-kill path, rebuilt, run,
reverted. **23 killed, 2 filed.**

| # | mutant | outcome |
| --- | --- | --- |
| M1 | no task-scoped death | KILLED (`TaskDeath` x4) |
| M2 | `except` ignored | SURVIVED -> code deleted |
| M3 | cancel reaches only `WAIT_IRQ` | KILLED |
| M4 | no `dying` guard in `thread_cancel` | KILLED |
| M5 | semaphore park not unlinked | KILLED |
| M6 | sleeper's deadline kept | KILLED |
| M7 | mutex boost not reverted | KILLED |
| M8 | mutex ownership transferred to the cancelled waiter | KILLED |
| M9 | endpoint park not unwound | KILLED |
| M10 | cancel mark dropped | KILLED |
| M11 | no death point at syscall entry | KILLED |
| M12 | task handle unbiased | KILLED |
| M13 | `task_resolve` ignores the generation | KILLED |
| M14 | implicit task nameable | SURVIVED -> arm added -> KILLED |
| M15 | creator gate open on spawn | SURVIVED -> arm added -> KILLED |
| M16 | creator gate open on kill | SURVIVED -> arm added -> KILLED |
| M17 | member may bring its own memory | KILLED |
| M18 | kill does not drop the hold | KILLED |
| M19 | reserved slot reusable | SURVIVED -> arm added -> KILLED |
| M20 | window not composed per thread | KILLED |
| M21 | window exclusivity dropped | survived on sim -> KILLED on qemu |
| M22 | `dev_window_free` counts a dying holder | SURVIVED (filed; `TODO.md`) |
| M23 | window Rule 7 admission dropped | survived on sim/qemu -> KILLED on qemu-riscv |
| M24 | `bring_up` spawns outside the group | KILLED |
| M25 | `task_release` moved after the sweep | SURVIVED (filed; the diagnosis for M22, `TODO.md`) |

Two method facts, both traps that would silently invalidate a future run:

- **`shutil.copy2` preserves mtime.** Restoring a baseline file this way leaves it looking OLDER
  than its object file, so ninja skips the rebuild -- every mutant after the sixth then runs against
  the previous mutant's binary and reports an identical, meaningless kill set. Restore with an
  explicit mtime bump, and re-check the baseline between mutants.
- **A survival on the sim can mean unreachable, not vacuous.** The sim declares no reserved blocks
  and admits one MMIO window shape, so `dev_window_exclusive` and `grant_reserved` are PARTIAL
  there. M21 and M23 surviving on the sim said nothing about the arm; both had to be rerun where
  those arms run in full (`qemu`, and for M23, `qemu-riscv`) before the KILLED verdict stands.

M22 and M25 are the same finding two ways -- a mutant that survives because the choreography the gate
would need does not exist yet -- and both are filed at the `TODO.md` entry that already carries their
substance ("Release the DEV window BEFORE the capability sweep" has NO gate).

## 9. What can land incrementally

The answer is not "big bang". Four of the five steps are independently gateable, and the first two
are not even part of the milestone.

**9.1. DONE, by M4.8.1's generic driver service rather than by this.** `rxsci`,
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
**MET EXCEPT ON ONE BOARD, and the exception is the finding.** `sizeof(Thread)` held on all three,
the ABI and every call site outside the kernel are untouched, and every suite is green. But this
paragraph prices the STRUCT and the layer also needs a POOL, and on `microbit` the arena base is the
aligned end of `.bss`, so `mem_self_grant` lost its last allocation grain and is now a declared skip.
The step is inert everywhere except the one board that had no slack to be inert with. Section 8.2's
fourth finding has the measurement.

**9.4. Explicit task creation and spawn-into-task. LANDED.** `KOS_SYS_TASK_CREATE = 51` makes an
EMPTY group holding a domain built from its own grant, and `kos_thread_params::task` seats a member
into it. The gate is CREATORSHIP, the same non-transferable parenthood `thread_kill` takes, and it is
enforced twice: on the spawn and on the kill.

**Four things this needed that section 5 did not say.**

1. **A task needs an identity that is not a thread.** Open question 3 asked whether that touches the
   capability codec. It does not: a `kos_task_t` is a plain word like `kos_thread_t`, generation over
   a BIASED index, and possession is not authority -- the creator tag is. The bias is what makes the
   all-zero word unmintable, so `kos_thread_params` zeroed by an app that predates the field means
   "no task" and every one of the 230 existing spawn sites keeps its meaning with no edit.
2. **An implicit task must be UNNAMEABLE.** `task_resolve` refuses a slot with no creator tag. Skip
   that and a guessed handle resolves onto idle's or root's implicit task, whose domain is the
   KERNEL domain, and an unprivileged spawn joining it is granted the whole arena. The check is one
   line and it is the difference between a group name and a privilege escalation.
3. **An empty explicit task has to exist, so a slot needs a state that is neither free nor held by a
   thread.** The creator tag is that state: a slot with one is not free even at refcount 0. It cost
   no bytes -- `Task` was repacked to `{Domain*, uint8_t refcount, uint8_t creator_tag, uint16_t
   gen}`, still 8 bytes on 32-bit and 16 on the host, so `microbit`'s `.bss` did not move and the
   skip set 9.3 grew did not grow again.
4. **The bound in question 5 does not cover it.** `KICKOS_MAX_TASKS` budgets one task per live TCB,
   which an explicit group of N members repays N-1 times over -- but a group holding NO member is a
   slot on top. `porting.md` says so; the refusal is a clean -KOS_ENOMEM either way.

**The drivers opt in through ONE call site.** `drv::bring_up` creates a task and spawns every thread
into it, so all twelve drivers are converted by editing the generic service rather than any of them.
`ThreadSet` is gone: `unwind` and `console_handover_finish` take the task handle, and the reverse-order
per-thread sweep is one `kos_task_kill`.

**Two honest costs.** The ring block is the TASK's shared region now, so a driver's per-thread
memory declaration became the GROUP's -- and `rx72m/rxsci`'s relay thread, which declared no block
alongside two peers that declared one, SEES the block it did not ask for. That is one thread in the
fleet, it is that driver's own state, and the grant the isolation principle is about -- the register
window -- stays its own. The alternative, giving the task no shared region and keeping the per-thread
grants, would have left 1.2's "two levels of memory authority" a fiction in practice.
And the sim's WINDOWED console posture deliberately keeps its window thread OUT of the driver's group
(`system/init/sim/service_list.cc`): it models a FOREIGN holder of the registers, which is the only
shape in which the deferred console reclaim is observable at all. Coupling it would have deleted that
gate's subject.

**9.5. Task-scoped death and the group kill. LANDED**, and it was smaller than this paragraph
feared. Section 6.1 has the mechanism and the answer to open question 1: a total per-`WaitKind` park
abort, plus ONE test at the syscall entry, and the "own design pass" it asked for turned out to be
the observation that ending a park and ending a thread are different problems.

`console_handover_finish` has dropped its handle parameter and the hand-rolled group kill is gone.
What did NOT go away is the caveat: cancellation is still asynchronous and a thread that never
re-enters the kernel is still unreachable. The claim that changed is the REACH -- from one park to
every park -- not the guarantee.

**The thing that needed care was the ORDER, not the reach.** The cancel sits in `exit_current`'s first
IrqLock block, before `task_release`, for two reasons that pull in opposite directions: it must
precede the release, because the release can free the task slot and leave `c->task` a dangling name;
and its wake of a strictly higher-priority peer is admitted by the M4.8.2 dying guard, so the switch
lands BEFORE the rest of the exit. `tests/unit/taskdeath/group_kill.cc` asserts both directions of
that as ordered traces, because a counter oracle cannot fail on a reordering.

**9.6 (M7).** `Domain` gains an `arch_aspace*` and a task whose domain has one is a process.
Belongs to `design-mmu-era-exploration.md`, not here.

Steps 9.3, 9.4 and 9.5 can be three submilestones or one; they cannot be reordered. In the event
9.3 landed alone and 9.4 + 9.5 landed together, and the pairing was right rather than convenient: 9.4
makes the window thread-scoped, which drops the lifetime guarantee 5.2 wants, and 9.5's coupled death
is what puts it back. Landing 9.4 alone would have shipped a window that a surviving peer outlives.

## 11. Open questions

1. **The group kill. ANSWERED by 9.5, and the question contained a false dichotomy.** Neither a
   preemptive kill nor "every blocking primitive is a cancellation point": the park abort is total
   over `WaitKind` and the DEATH POINT is the syscall entry, which are two mechanisms rather than
   one. Section 6.1 has it. What survives unanswered is the residual it names -- a thread that never
   re-enters the kernel -- and that one does need preemption.
2. **Does the `domain_for` dedup survive? IT DID NOT: DELETED AT M6.2's T5.** The ruling below is
   kept as the argument that made deleting it cheap, and the prediction in its last sentence is what
   happened. What forced it was `docs/design-m6-mmu.md` F2: under a process model the dedup puts two
   kill groups inside one address space, and then "the set of threads sharing a domain" names
   something bigger than a task. The harder half was not the scan at all but the immortal
   default-user domain every no-grant task was handed, which is a shared identity by construction;
   that became a per-task template at the same step.
   *The argument, as it stood:* with the MMIO window out of `domain_for` an MMIO-bearing spawn no
   longer skipped the scan, so two threads granting the same block shared one domain where they used
   to burn two. It was a slot economy and explicitly NOT an expression of intent, and
   `KICKOS_MAX_DOMAINS` was over-provisioned for the alternative.
3. **What is a task's identity in the handle space? ANSWERED: a plain word, not a capability.**
   `kos_task_t` is generation over a biased index, resolved against the task pool, and the cap codec
   is untouched -- 5.4 holds. Possession is NOT authority, exactly as a thread handle's is not: the
   gate is the CREATOR tag, so guessing a handle buys nothing. The bias exists so the all-zero word
   cannot name a live task, which is what lets `kos_thread_params::task` default to "none" for every
   spawn that predates the field.
4. **Does `kos_wait_last` become task-scoped?** It is entangled with the reaper-init problem already
   in `TODO.md`, which is core-path work with its own number. Deliberately not answered here, and 9.4
   and 9.5 left it alone: it still means "last thread in the system", backed by `Kernel::live`.

6. **When does an explicit task's slot come back? ANSWERED, and the question was filed as the wrong
   KIND of problem.** It is not a slot leak: `kill_tag_for_index` derives a tag from the pool slot,
   so the SUCCESSOR of a dead creator's thread slot passes `task_created_by` for the predecessor's
   groups -- it can kill them, and it can seat a child in one and hand that child the group's domain
   regions. **That also refutes the third candidate below: declaring the hold the creator's for life
   IS the escape.** The second candidate landed -- `task_orphan_created_by`, a task-pool sweep at
   `exit_current` keyed on the creator tag, which is the one that composes with `ThreadPool::alloc`'s
   spawner-tag sweep because both exist for the same reason. `exit_current` is TOTAL over deaths where
   `alloc`'s reclaim point is not, which is why the sweep sits there and not at reuse. Gated by
   `tests/unit/taskdeath/creator_hold.cc` over the real `kernel/task/task.cc`, now in the K-seam
   source set; the syscall refusals it feeds (`-KOS_EPERM` for a stranger, `-KOS_EBADF` for a dead
   group's handle) are outside that set and remain the selftest's to show.
5. **the task-pool sizing symbol. ANSWERED, and the bound stated here was off by one.** Live TCBs are
   idle + root + `KICKOS_MAX_THREADS`, which is `KICKOS_THREAD_SLOTS + 1`, not `KICKOS_THREAD_SLOTS`:
   `KICKOS_THREAD_SLOTS` is itself `KICKOS_MAX_THREADS + 1` and THAT `+1` is root's, so the OUTER
   one is idle's, idle being the TCB outside the pool. At the smaller figure a microbit would
   have three task slots for four live threads and `task_for` would refuse the second concurrent
   spawn. `KICKOS_MAX_TASKS` ships as `(KICKOS_THREAD_SLOTS + 1)` with a `static_assert` on that
   floor, so `task_for`'s ENOMEM stays COINCIDENT with the thread pool's instead of arriving one
   spawn earlier. The `.bss` this costs is 32 bytes on microbit and 144 on picopi, and section 8.2
   has what that did.

7. **May a member decline the group's memory? REFUSED, and the declaration that implied it is
   gone.** A driver descriptor carried a per-thread memory flag whose only reader was an
   OR-reduction into the group's grant, so a thread declaring `false` beside peers declaring `true`
   was widened by them -- `rx72m/rxsci`'s relay, the fleet's one instance. The filed fix was a third
   memory scope: seat a member in the group with NO domain regions. It is refused, because a task is
   DEFINED here as the set of threads that share one memory domain (section 5.1), and a member that
   shares the task but not its domain makes that definition false -- two answers to "what memory do
   this task's threads see" is exactly the second truth the tiebreaker forbids. The right
   decomposition for a thread that must not reach the block is a task of its own, and the only thing
   stopping that is that a task is also the kill group; separating "shares memory" from "dies
   together" is an M6-scale change to the model, not a bring-up tweak.
   **What landed instead is the collapse.** The flag equalled `arg == KOS_DRV_ARG_BLOCK` in every
   descriptor in the tree, so it was a second truth for that too, and it is deleted: the group's
   shared region is the block whenever there is one. Validator leg L4 grew the converse arm, refusing
   a block no thread reads, so the widest ask a descriptor can make is now a compile error rather
   than a silent grant.
   **The residual, stated once:** every thread of a block-owning driver sees the whole block,
   whatever its argument, because a task owns exactly one `Domain` and a member may bring no grant of
   its own. The register window, which is what 5.2 is about, stays per-thread.
