<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 5. Call and reply: endpoints, the rendezvous, the reply capability

By the end of this chapter you will have followed one synchronous call from a client to a
server and back, on the `qemu` reference walk. An endpoint is created and capabilities on it are
handed out. The server parks waiting to receive. The client calls: the kernel copies its request
straight into the server's buffer, mints a one-shot reply capability into the server's handle
table, parks the client, lends the server the client's priority, and switches to the server.
The server replies through that capability, which is consumed, and the client wakes holding the
answer. Then you will run the same call in the other order, client first, and watch the client
park on the endpoint until the server arrives. Last, you will see the register fastpath that
the Cortex-M build adds for small calls: the whole handoff done inside the trap handler.

The concepts are in two Book chapters: [Endpoints: synchronous IPC by
rendezvous](../book/endpoints-synchronous-ipc-by-rendezvous.md) (why there is no message
buffer in the kernel) and [Synchronous call/reply: the reply
capability](../book/synchronous-call-and-reply.md) (why the parked caller is itself the reply
object, and why the call must lend its priority). The exact contract, every return code
included, is [Reference: Synchronous call/reply IPC](../reference/ipc-call-reply.md).

This chapter assumes [2. The first syscall](02-first-syscall.md) (how a `svc` reaches
`syscall_dispatch`) and [3. Sleep](03-sleep.md) (how a thread parks and is woken).

## The pair we follow

The console receives without an info buffer, so the kernel has nowhere to write a reply
capability (Stop 3). A `kos_call` to it returns `-KOS_ENOTSUP` (Stop 5 shows the test).
The bus services need a chip driver under `system/driver/`, and `mps2-an386` has none.
Instead, we use a selftest arm with a client and an echo server in
`user/apps/common/selftest/main.cc`:

- **`t_call_from_root`** is the client. It is the selftest's `main` thread (named `main`,
  priority 2). It creates an endpoint, spawns the server, and calls it twice with
  `kos_call_timed(g_ep, buf, 4, sizeof(buf), STALL_TOLERANT_US)`: first after the server has
  parked (half A), then before the server has run at all (half B).
- **`echo_server`** is the server, spawned at `KICKOS_PRIO_MIN` (priority 1) as `rtS`, then
  `rtS2`. It receives once with `kos_reply_recv`, copies what came in, and answers `"pong!"`
  with `kos_reply`.

The client outranks the server on purpose, so this walk also shows priority donation. For
the last stop, a second arm in the same file, **`t_call_reg_fastpath`** (`fp_server` /
`fp_caller`, run by `fp_run`), makes two equal-priority threads call with small payloads.

Nothing in this chapter is chip code. `f411disco` runs the same `kernel/syscall/` files and
the same `arch/arm/armv7m/switch.S`, so every stop reads the same on the silicon twin.

## Running it under gdb

Start `build/qemu/user/apps/common/selftest/selftest` halted, as in [0. Setup](00-setup.md).
`ctest --test-dir build/qemu -N -V -R '^qemu_selftest$'` names the script; the emulator line
underneath is:

```
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/selftest/selftest -gdb tcp::1234 -S
```

and in a second terminal `gdb-multiarch build/qemu/user/apps/common/selftest/selftest`,
then `target remote :1234`. The selftest runs over a hundred arms, so every "Try it" below
starts from this prelude, which stops at the right arm and skips its object-budget probe
(`TAP_ASK` creates and closes objects of each kind, endpoints included, to check the pools
can serve the arm):

```
(gdb) break (anonymous namespace)::t_call_from_root
(gdb) continue
(gdb) delete
(gdb) break selftest::objects_refused
(gdb) continue
(gdb) finish
(gdb) delete
```

Addresses and capability generations below are from one run; yours may differ. The preset
builds with `-Os`, so several kernel functions are inlined. gdb still stops at them, but a few
arguments print as `<optimized out>` or with stale register values (`cap_len` in
`endpoint_recv_locked` is one). The stops below quote only values that gdb read reliably,
most of them fields of a thread or an endpoint in memory.

### Stop 1: the endpoint is created

**Where:** `user/src/syscall_stubs.cc`, `kos_endpoint_create`; `kernel/syscall/syscall_ipc.cc`,
`endpoint_create`; `kernel/sync/sync.cc`, `endpoint_claim_created`; `kernel/include/kickos/endpoint.h`,
`struct Endpoint`

`kos_endpoint_create(&g_ep)` traps with `KOS_SYS_ENDPOINT_CREATE` (number 26), and
`syscall_body` calls `endpoint_create`. The kernel takes a free slot in the endpoint pool,
gives it one reference, and installs a capability to it in the caller's own handle table. The
creator gets every right: `CAP_WAIT` (receive), `CAP_SIGNAL` (send or call), `CAP_TRANSFER`
(hand it on at spawn) and `CAP_HANDOUT` (seat a receiver without being one).

```cpp
    int endpoint_create(uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        Thread* c = sched::current();
        // ...
        Endpoint* ep = nullptr;
        int const i = endpoint_claim_created(&ep);
        // ...
        kernel().endpoint_refs[i] = 1;
        int const obj = kernel().endpoints.handle_for(i);
        int const rc = cap_install(c, obj, CapType::CAP_ENDPOINT, CAP_RIGHTS_ALL, out_cap);
```

Notice what an `Endpoint` holds: two wait lists (`send_waiters`, `recv_waiters`), two holder
counts, and a pointer to the thread that last received there (`server`). There is no message
buffer. The parked side's own buffer is the storage, as the Book explains.

Notice also that there are two handles in play. The value `main` gets back in `g_ep` names a
slot in `main`'s handle table. That slot, in turn, stores the endpoint pool's own handle in its
`obj` field. Every later use of `g_ep` goes through the resolve that checks one against the
other ([Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md)).

**Try it:** after the prelude, `break kos_endpoint_create`, `continue`, `finish`, then
`p/x '(anonymous namespace)::g_ep'`. You should see something like `$3 = 0x9f0008`: the low 16
bits (`KCAP_INDEX_BITS`) are the slot index in `main`'s table (8), the high bits the slot's
generation.

**Why, and the contract:** [Book: Endpoints](../book/endpoints-synchronous-ipc-by-rendezvous.md),
[Reference: the receive out-struct and capabilities](../reference/ipc-call-reply.md)

### Stop 2: the server is given a receive-only capability

**Where:** `user/apps/common/selftest/main.cc`, `t_call_from_root`; `kernel/syscall/syscall_thread.cc`,
`spawn_masked`; `kernel/syscall/cap.cc`, `obj_ref_inc`, `cap_install_at`

`main` spawns the server with a grant list. Each entry names a capability `main` holds and the
rights the child gets. Here the server gets the arm's "done" semaphore and the endpoint with
`EP_WAIT_ONLY`, which is `KOS_CAP_WAIT` alone:

```cpp
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        // ...
        scaps[1].source_cap = g_ep;
        sv = irq_spawn(echo_server, nullptr, "rtS", KICKOS_PRIO_MIN, scaps, 2);
```

In the kernel, `spawn_masked` checks each grant before building anything: the source must carry
`CAP_TRANSFER`, and the mask may only narrow the source's rights, never widen them. Then
`obj_ref_inc` raises the object's counts (for an endpoint granted `CAP_WAIT`, that includes
`recv_holders`), and `cap_install_at` seats grant `i` at index `KOS_SPAWN_DELEGATED_CAP0 + i`
in the child. That is why `echo_server` names its endpoint as capability `3`: index 2 is the
semaphore, index 3 the endpoint. [8. Spawn and capabilities](08-spawn-and-caps.md) walks this code.

Here the client is `main` itself, which keeps its all-rights capability. In the fastpath arm
(Stop 11) the client is a spawned thread, and it receives `EP_SIGNAL_ONLY`: a client can call
but not receive, a server can receive but not call. Neither could manufacture the other's
right.

**Try it:** after the prelude, `break kickos::cap_install_at`, `continue`. The first stop is
Stop 1's own install, reached from `cap_install`: `main`'s table, `index=8`,
`type=kickos::CapType::CAP_ENDPOINT`, `rights=15` (all four rights), `obj_handle=786432`.
`continue` twice more. The third stop is the server's: `p c->name` gives `"rtS"`, `index=3`,
the same `obj_handle=786432`, `rights=1` (`KOS_CAP_WAIT`), and `bt 2` shows `spawn_masked` as
the caller. Two table entries, one endpoint, different rights.

**Why, and the contract:** [Book: How a driver reaches its client](../book/synchronous-call-and-reply.md),
[8. Spawn and capabilities](08-spawn-and-caps.md)

### Stop 3: the server waits to receive

**Where:** `user/apps/common/selftest/main.cc`, `echo_server`; `kernel/syscall/syscall_ipc.cc`,
`endpoint_reply_recv`, `endpoint_recv_locked`; `kernel/sync/sync.cc`, `endpoint_receiver_waits`,
`wq_block`, `park_queueless`

There is no plain receive syscall. A receive is `kos_reply_recv` with nothing to reply to:

```cpp
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, 0, KOS_TIMEOUT_NONE);
        int32_t n = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
```

The first argument is the reply capability to answer first; `KOS_CAP_NONE` says there is none.
`kos_call_lens_pack(0, 16)` packs "reply 0 bytes, accept up to 16" into one word. `opts`
carries the endpoint to listen on (`3`), the flags (`0`, so this receive *can* host a call),
and room for the answer: `opts.info` is where the kernel will write the reply capability.

The trap arrives as `KOS_SYS_REPLY_RECV` (68) in `endpoint_reply_recv`. It snapshots the
input half of `opts`, skips the reply half, and calls `endpoint_recv_locked`. That function
resolves capability 3 for `CAP_WAIT`, then records this thread as the endpoint's `server`
(`endpoint_receiver_waits`). It looks at `send_waiters` for a parked sender, finds none, and
parks:

```cpp
        c->ipc.buf = buf;
        c->ipc.len = cap_len;
        c->ipc.badge_out = badge_out;
        ktime_deadline_arm(c, timeout_us, dw->held());
        // Transfer the pending wake to the park's reschedule, including peer notification.
        *epoch = wq_block(ask)(e->recv_waiters, WAIT_EP_RECV, e, dw->take(), dw->held());
```

`c->ipc` is the descriptor whoever arrives next will read: where the server's buffer is, how
big, and where to write the arrival's metadata. `badge_out` points at `opts.info`. A receive
with `KOS_RECV_NO_INFO` sets it to 0, and that 0 is what makes a call refuse this receiver.
`wq_block` marks the thread `BLOCKED` through `park_queueless`, links it on `recv_waiters`,
and reschedules. The server's kernel stack, frozen inside `endpoint_reply_recv`, is its
continuation ([3. Sleep](03-sleep.md) walked the same park).

**Try it:** after the prelude, `break kickos::endpoint_recv_locked`, `continue`: `cap=3`,
and `p c->name` gives `"rtS"`. (Stopping here first matters: `main` parked just before, waiting
for the witness thread as `WAIT_JOIN`, and a `park_queueless` breakpoint set from the prelude
would stop on that park.) `delete`, `break kickos::park_queueless`, `continue`. You should
see `kind=kickos::WAIT_EP_RECV`, and `p c->name` is still `"rtS"`. Then
`p *(kickos::Endpoint*)obj` shows `recv_holders = 2` (`main`'s capability and the server's),
`handout_holders = 1`, empty lists, and `server` equal to `c`. `finish`, then `p/x c->ipc`:
`{buf = 0x20029fd0, len = 0x10, badge_out = 0x20029ff0}`. `badge_out` is `buf + 0x20`, the
`info` field of the server's `opts` on its own stack.

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
[Reference: `KOS_SYS_REPLY_RECV`](../reference/ipc-call-reply.md)

### Stop 4: the client calls

**Where:** `user/src/syscall_stubs.cc`, `kos_call_timed`; `kernel/syscall/syscall.cc`, the
`KOS_SYS_CALL_TIMED` arm; `kernel/syscall/syscall_ipc.cc`, `endpoint_call`;
`kernel/include/kickos/endpoint.h`, `endpoint_takes_sends`

While the server was parking, `main` was blocked waiting for a witness thread to end. Now it
runs again and calls. `kos_call_timed` traps with number 46; the trap frame has four argument
slots, so the stub packs both lengths into one (`kos_call_lens_pack`) to make room for the
deadline. The dispatch arm unpacks them and calls `endpoint_call`.

`endpoint_call` works in a fixed order, and the order matters:

1. Reject `send_len > KOS_EP_MSG_MAX` (256) with `-KOS_EINVAL`; clamp `recv_cap`.
2. Check the one buffer is readable for the request *and* writable for the reply, in the
   caller's context, before any lock (`user_readable_and_writable_ok`). The same buffer
   carries the request out and the answer back.
3. Under `IrqLock`: exit now if this thread was cancelled, *before* touching any receiver;
   resolve the capability for `CAP_SIGNAL`; refuse an endpoint nobody can serve
   (`endpoint_takes_sends`, answered by `endpoint_unserved`); then peek, without removing, the
   best parked receiver with `wq_peek_highest(e->recv_waiters)`.

From here the code splits in three: a receiver is parked (half A, Stops 5 to 9), none is
parked yet (half B, Stop 10), or the endpoint lives in another kernel (an AMP build only; see
[11. Multicore](11-multicore.md)).

**Try it:** after the prelude, `break kickos::endpoint_call`, `continue`. gdb shows
`cap=10420232` (that is `0x9f0008`, `g_ep` from Stop 1), `send_len=4`, `recv_cap=16`,
`timeout_us=5000000`. `bt 4` shows `syscall_body (nr=46, ...)` under it and `svc_trampoline`
below that.

**Why, and the contract:** [Reference: `KOS_SYS_CALL = 34` and `KOS_SYS_CALL_TIMED = 46`](../reference/ipc-call-reply.md),
[Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md)

### Stop 5: the rendezvous: probe, pop, copy, mint

**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_call`; `kernel/syscall/cap.cc`,
`cap_can_take_reply`, `cap_install_reply`; `kernel/include/kickos/cap.h`, `cap_reply_seq_seat`;
`kernel/syscall/syscall_mem.cc`, `ep_copy`

A receiver is parked, so the call completes the rendezvous now, with the running thread (the
client) doing the copy. Everything that can refuse runs *before* the receiver is taken off its
list:

```cpp
            if (w != nullptr)
            {
                // Probe before removing the receiver so a refusal cannot strand it.
                if (w->ipc.badge_out == 0)
                {
                    return -KOS_ENOTSUP; // Calls require receive metadata.
                }
                // ...
                if (not cap_can_take_reply(w))
                {
                    return -KOS_EMFILE;
                }
                // ...
                (void)wq_pop_highest(e->recv_waiters); // IrqLock keeps w at the head.
                // ...
                size_t const n = ipc_len_min(send_len, w->ipc.len);
                // ...
                if (not ep_copy(ipc_buf_space(w), w->ipc.buf, user_space_of(c), buf, n))
```

The first probe is the info-less test from Stop 3: a receiver with nowhere to put a reply
capability cannot host a call. This is how the console protects itself: it receives
info-less, so a hostile `kos_call` bounces instead of filling the console's table. The second
probe asks whether the *server's* table has a free slot and the server is under its
`KICKOS_CAP_REPLY_MAX` bound. Had either failed after the pop, the server would be off its
list with nobody to wake it.

`ep_copy` copies `min(send_len, receiver capacity)` bytes from the client's buffer into the
server's. A shorter receive buffer truncates the request; that is not an error. On this board
both "address space" arguments are null: an MPU board has one physical address space, and the
buffers were already checked against each owner's regions.

Then the reply capability is minted into the server's table:

```cpp
                c->call_seq++; // Advance before packing the reply capability.
                uint32_t rcap = KCAP_INVALID;
                // ...
                int const minted = cap_install_reply(w, c, &rcap);
```

`cap_install_reply` writes an ordinary handle-table entry of type `CAP_REPLY` with **rights 0**
(it cannot be passed on, duplicated or waited on). Its `obj` field is the *caller's thread
handle*, generation included, and the low byte of the caller's `call_seq` rides in two spare
bitfields (`cap_reply_seq_seat`). There is no reply object anywhere else: the parked caller is
the reply object. `write_recv_info` then writes `{badge, reply_cap}` into the server's
`opts.info`, through the `badge_out` pointer the server left in Stop 3.

**Try it:** after the prelude, `break kos_call_timed`, `continue`, `delete`, then
`break kickos::ep_copy`, `continue`: `n=4`, and `x/4c src` prints `'p' 'i' 'n' 'g'`. Then
`delete`, `break kickos::cap_install_reply`, `continue`: `p c->name` is `"rtS"` (the table
being written), `p caller->name` is `"main"`, `p caller->call_seq` is `1`. `finish` back
into `endpoint_call` and `p/x rcap`: `0x4`, generation 0, slot 4 of the server's table (2 and
3 were granted at spawn).

**Why, and the contract:** [Book: What KickOS chose: the parked caller IS the reply object](../book/synchronous-call-and-reply.md),
[Reference: The reply capability (`CAP_REPLY`)](../reference/ipc-call-reply.md)

### Stop 6: the client parks, lends its priority, and switches to the server

**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_call`; `kernel/syscall/syscall_internal.h`,
`reply_wait_seat`; `kernel/sync/sync.cc`, `park_queueless`, `reply_donor_park`;
`kernel/sched/sched.cc`, `sched::set_prio`, `sched::wake`; `arch/arm/common/arch_arm_common.cc`,
`arch_switch`

```cpp
                w->wait_result = static_cast<intptr_t>(n);
                // The request was fully copied above, so the reply may overwrite buf.
                reply_wait_seat(c, buf, recv_cap);
                // Donate to the server before waking it.
                if (c->prio > w->prio)
                {
                    // ...
                    sched::set_prio(w, c->prio, lock);
                    // ...
                }
                // ...
                epoch = park_queueless(ask)(c, WAIT_EP_REPLY, w, lock);
                reply_donor_park(w, c); // Off the ready queue; link can be reused.
                // Arm the reply deadline before the wake can switch threads.
                ktime_deadline_arm(c, timeout_us, lock);
                // ...
                sched::wake(w, lock);
```

Read it in order:

- `w->wait_result = n` is the server's answer to its receive, waiting in its control block for
  when it resumes.
- `reply_wait_seat` turns the client's `ipc` descriptor around: it now describes where the
  *reply* goes (`buf`, up to `recv_cap` bytes) and sets `call_state = CALL_REPLY_WAIT`.
- `set_prio(w, c->prio)` is donation: the priority-2 client lends its priority to the
  priority-1 server *before* waking it, so the server runs the client's request at the
  client's urgency. Without it, a thread ranked above the server but below the client could
  preempt the transaction and hold the client up for as long as it ran.
- `park_queueless` blocks the client on **no list at all**. Its wait edge says
  `WAIT_EP_REPLY` and names the server. `reply_donor_park` then links the client onto the
  server's `reply_waiters`, which is how the priority funnel later finds every caller the
  server owes an answer to.
- `ktime_deadline_arm` arms the five-second deadline of `kos_call_timed`. One deadline covers
  the whole call.
- `sched::wake(w)` readies the server. The client has just parked, so the server, now at
  priority 2, is the best ready thread and the scheduler picks it. On this Cortex-M `arch_switch` only records the next thread and pends PendSV; the
  real switch happens when `IrqLock` drops and PendSV runs
  ([Book: Context switching](../book/context-switching-and-the-silicon-contract.md)).

**Try it:** after the prelude, `break kos_call_timed`, `continue`, `delete`, then
`break kickos::sched::set_prio`, `continue`: `p t->name` is `"rtS"`, `p t->prio` is `1`,
`p p` is `2`. Next `delete`, `break kickos::park_queueless`, `continue`. In this build the park
is inlined, so gdb stops in `endpoint_call` on the `park_queueless` line: `p w->prio` is now
`2`, `p w->base_prio` still `1`, `p c->call_state` is `2` (`CALL_REPLY_WAIT`),
`p/x c->ipc` is `{buf = 0x20027eb0, len = 0x10, badge_out = 0x0}`. Then `delete`,
`break kickos::sched::wake`, `continue`: `p t->state` is `BLOCKED` (the server, about to be
readied). `delete`, `break arch_switch`, `continue`, `bt 2` shows `endpoint_call` as the
caller; `delete`, `break PendSV_Handler`, `continue` stops in `arch/arm/armv7m/switch.S`, where
the switch really happens.

**Why, and the contract:** [Book: Why donation is required](../book/synchronous-call-and-reply.md),
[Reference: Priority donation, D1](../reference/ipc-call-reply.md),
[4. Mutex](04-mutex.md) (the same `set_prio` funnel under priority inheritance)

### Stop 7: the server wakes holding the request and a reply capability

**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_reply_recv`; `kernel/sync/sync.cc`,
`wq_confirm_resume`; `user/apps/common/selftest/main.cc`, `echo_server`

PendSV restores the server's context, and the server continues inside `endpoint_reply_recv`
from where `wq_block` left it. It runs `wq_confirm_resume` (wait until the switch that resumes
it really happened before reading anything the waker wrote), then reads `c->wait_result`: 4.
The syscall returns 4, and `opts.info.reply_cap` already holds the handle the client wrote
there in Stop 5.

`echo_server` does its work, still at the client's priority 2, and answers:

```cpp
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5); // reply from the server's OWN stack (unpriv-readable)
            kos_reply(info.reply_cap, rpl, 5);
        }
```

The test is against `KOS_CAP_NONE`, never a sign test: a handle fills all 32 bits. A plain
`kos_send` arrival would have delivered `KOS_CAP_NONE` here, with nothing to answer.

**Try it:** after the prelude, `break kos_reply`, `continue`. gdb shows
`kos_reply (reply_cap=4, buf=0x20029fc8, len=5)`, and `bt 2` names `echo_server`. `x/5c buf`
prints `'p' 'o' 'n' 'g' '!'`.

**Why, and the contract:** [Reference: The receive out-struct](../reference/ipc-call-reply.md),
[Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)

### Stop 8: the reply: resolve, consume, copy, revert, wake

**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_reply`, `endpoint_reply_locked`,
`DeferredWake`; `kernel/syscall/cap.cc`, `cap_reply_caller`, `cap_reply_thread`,
`cap_slot_vacate`; `kernel/sync/sync.cc`, `reply_donor_unpark`, `thread_effective_prio`

`kos_reply` traps with `KOS_SYS_REPLY` (35). `endpoint_reply` clamps the length, checks the
server's buffer is readable, and calls `endpoint_reply_locked` under one `IrqLock`. It looks up
the handle and refuses anything that is not a `CAP_REPLY` with `-KOS_EBADF`. Then:

```cpp
        // The vacate overwrites `obj`, so the caller is resolved first.
        Thread* caller = cap_reply_caller(*e);
        cap_slot_vacate(c, reply_cap & KCAP_INDEX_MASK, e);
        // Unlink the donor before recomputing priority. A caller now waiting on another
        // server no longer belongs to this reply.
        if (caller != nullptr and not reply_donor_unpark(c, caller))
        {
            caller = nullptr;
        }
```

`cap_reply_caller` turns the entry back into a thread through `cap_reply_thread`, which
accepts it only if all four checks pass: the thread index is in the pool, the slot's
generation matches the one stored in the handle, the thread is `BLOCKED` in
`CALL_REPLY_WAIT`, and its `call_seq` matches the byte stored at the mint. The last check is
what makes a late reply harmless. If the client had timed out and made a new call since, its
sequence moved on and the old capability resolves to nothing (`-KOS_ESRCH`).

`cap_slot_vacate` empties the slot and bumps its generation *on every path*, success or not.
That is the one-shot rule: a second `kos_reply` on handle 4 finds an empty slot.

The rest copies and wakes:

```cpp
        size_t const n = ipc_len_min(len, caller->call_rx_cap);
        // ...
        bool const ok = ep_copy(ipc_buf_space(caller), caller->ipc.buf, user_space_of(c), buf, n);
        // ...
        caller->call_state = CALL_NONE;
        // ...
        sched::set_prio(c, thread_effective_prio(c), dw->held());
        // ...
        if (sched::wake_no_resched(caller, dw->held()))
        {
            dw->offer(caller);
        }
```

The reply lands in the client's `ipc.buf`, which Stop 6 pointed at the client's own `buf`.
The server's priority is not "restored to base". It is **recomputed** by
`thread_effective_prio` from every live reason to be boosted: its base priority, the mutexes
it holds, the callers behind its remaining reply capabilities, and the callers queued on
endpoints it serves. Here none is left, so the answer is 1. The wake goes through
`DeferredWake`, so the reschedule runs once, from that object's destructor at the end of
`endpoint_reply`, and the client (priority 2) takes the CPU back at once. That deferral is
what lets `kos_reply_recv` reply and then park in one kernel entry without switching away in
between. `wake_no_resched` is also where the client's deadline is cancelled
(`ktime_deadline_cancel`).

**Try it:** after the prelude, `break kos_reply`, `continue`, `delete`, then
`break kickos::cap_reply_caller`, `continue`, `finish`. gdb prints the returned
`(kickos::Thread *)`; `set $caller = (kickos::Thread*)$`, then `p $caller->name` is `"main"`,
`p $caller->call_state` is `2`, `p $caller->call_seq` is `1`, `p $caller->wait_kind` is
`kickos::WAIT_EP_REPLY`. Now `delete`, `break kickos::reply_donor_unpark`, `continue`:
`p server->prio` is `2`, `p server->base_prio` is `1`. `delete`,
`break kickos::thread_effective_prio`, `continue`, `finish`: `Value returned is ... 1`.
`delete`, `break kickos::sched::wake_no_resched`, `continue`: `p t->name` is `"main"` and
`p t->on_timer` is `true`, the deadline about to be cancelled.
Finally `delete`, `break kickos::sched::resched_after_wake`, `continue`, `bt 3`: the caller is
`DeferredWake::~DeferredWake` inside `endpoint_reply`, and `p t->name` is `"main"`.

**Why, and the contract:** [Reference: `KOS_SYS_REPLY = 35`, Lifecycle / death matrix, D3](../reference/ipc-call-reply.md),
[Book: one funnel that keeps it honest](../book/synchronous-call-and-reply.md)

### Stop 9: the client resumes with the answer

**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_call` (its tail)

The client was parked inside `endpoint_call`. It resumes after the `IrqLock` block:

```cpp
        wq_confirm_resume(c, epoch);
        c->call_state = CALL_NONE;
        // ...
        return static_cast<int32_t>(c->wait_result);
```

`wait_result` is 5, the reply length after clamping to `recv_cap`. The bytes are already in
`buf`: the same buffer that carried `"ping"` out now holds `"pong!"`. That in-place reuse is
safe because the request was copied in full before the client parked. `call_state` is written
`CALL_NONE` on every return path, so a later plain `kos_send` from this thread is never
mistaken for a call.

**Try it:** after the prelude, `break kos_call_timed`, `continue`, then
`set $ret = $lr & ~1` (the return address in `t_call_from_root`), `delete`, `tbreak *$ret`,
`continue`. You land back in `t_call_from_root`: `p $r0` is `5`, and `p buf` starts with
`"pong!"`.

**Why, and the contract:** [Reference: `KOS_SYS_CALL = 34`](../reference/ipc-call-reply.md),
[Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
(the confirm-resume barrier)

### Stop 10: the server is not there yet: the client parks on the endpoint

**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_call` (its last arm), `endpoint_recv_locked`
(the `CALL_SEND_WAIT` arm), `caller_refuse`

Half B of `t_call_from_root` creates a second endpoint, spawns `rtS2` at priority 1, and calls
at once. `main` outranks the new server, so `rtS2` has not run: `recv_waiters` is empty. The
endpoint still *takes sends* (`main` holds `CAP_WAIT` on it), so the call does not fail. It
parks:

```cpp
                c->ipc.buf = buf;
                c->ipc.len = send_len;
                c->ipc.badge_out = 0;
                c->call_rx_cap = recv_cap;
                c->call_seq++; // Packed when the receiver removes this call.
                c->call_state = CALL_SEND_WAIT;
                if (e->server != nullptr and c->prio > e->server->prio)
                {
                    // ...
                    sched::set_prio(e->server, c->prio, lock);
                    // ...
                }
                // ...
                ktime_deadline_arm(c, timeout_us, lock);
                // Same wait kind as a plain sender; call_state is what separates the two.
                epoch = wq_block(ask)(e->send_waiters, WAIT_EP_SEND, e, nullptr, lock);
```

The client sits on `send_waiters` exactly like a plain sender, with `CALL_SEND_WAIT` marking it
as a call. Nothing is minted yet: the server's table is the server's business, and the server
is not here. If the endpoint already had a server busy elsewhere, `set_prio(e->server, ...)`
would boost it now, so a low-priority server does not finish other work at low priority while
an urgent caller queues (D2). Here `e->server` is still null, because nobody has received on
this endpoint yet.

The client blocking lets `rtS2` run. Its receive lands in `endpoint_recv_locked`, which pops
the parked client and sees `call_state == CALL_SEND_WAIT`. It runs the same probes as Stop 5,
from the other side: an info-less receive, or a full table, refuses that one caller
(`caller_refuse`) and keeps scanning. Then the **server** does the copy, out of the parked
client's buffer, mints the reply capability into its own table, and finishes the client's
park:

```cpp
                reply_wait_seat(s, s->ipc.buf, s->call_rx_cap);
                reply_donor_park(c, s);
                if (s->prio > c->prio)
                {
                    sched::set_prio(c, s->prio, dw->held());
                }
                return static_cast<int>(n);
```

The client moves from `send_waiters` to the server's `reply_waiters` without ever being
readied, and its deadline goes with it. The server never parks at all: its receive returns at
once with the request and the reply capability, running at the client's priority. From here
the reply is Stop 8 unchanged.

This is the rendezvous rule from the Book in action: whichever side arrives second does the
copy, while it holds the lock, out of or into the side that is parked.

If the endpoint could not take sends at all (no holder of `CAP_WAIT`, or every receiver has
left and none has waited since), the call would not park: `endpoint_unserved` answers
`-KOS_EAGAIN` while a holder of `CAP_WAIT` or `CAP_HANDOUT` remains, so a receiver may still
come, and `-KOS_ECONNREFUSED` once none does. A server that dies while the client is still on
`send_waiters` wakes it with one of the same two codes.

**Try it:** after the prelude, `break kos_call_timed`, `continue`, `continue` (the second
call), `delete`. Then `break kickos::park_queueless`, `continue`: `kind=kickos::WAIT_EP_SEND`,
`p c->name` is `"main"`, `p c->call_state` is `1` (`CALL_SEND_WAIT`), `p c->call_seq` is `2`,
`p/x c->ipc` is `{buf = 0x20027eb0, len = 0x4, badge_out = 0x0}`, and
`p ((kickos::Endpoint*)obj)->server` is `0x0`. `delete`, `break kickos::cap_install_reply`,
`continue`, `bt 4`: now the caller is `endpoint_recv_locked` inside `endpoint_reply_recv`,
`p c->name` is `"rtS2"`, `p caller->call_state` is still `1`. `delete`,
`break kickos::sched::set_prio`, `continue`: `p t->name` is `"rtS2"`, `p p` is `2`, and `bt 2`
shows `endpoint_recv_locked`. The call still returns 5 and `"pong!"`.

**Why, and the contract:** [Reference: Slowpath, D2, and the death matrix](../reference/ipc-call-reply.md),
[Book: The rendezvous: the arriving thread copies, under the lock](../book/endpoints-synchronous-ipc-by-rendezvous.md)

### Stop 11: the register fastpath: the whole handoff inside the trap

**Where:** `user/src/syscall_stubs.cc`, `kos_call`; `arch/arm/armv7m/switch.S`,
`arch_syscall_reg`, `SVC_Handler`; `kernel/syscall/syscall_ipc_fast.cc`, `kickos_ipc_fastpath`;
`kernel/sched/sched.cc`, `sched::switch_prepare`, `switch_book`;
`arch/arm/common/arch_arm_common.cc`, `arch_ctx_set_syscall_result`

Everything above ran through `syscall_dispatch` in thread context, on the generic path. This
build adds a shorter path for one common case, a small untimed call
(`KICKOS_ARCH_HAS_IPC_FASTPATH`, selected for armv6m, armv7m, RXv3 and rv32imac in
`arch/Kconfig`). Use the `t_call_reg_fastpath` arm for this stop: `fp_server` and `fp_caller`
both run at priority 11, and the first call sends 8 bytes and accepts 8.

The client side decides by size only. `kos_call` sees both lengths within
`KOS_CALL_REG_BYTES` (20 bytes, five words), copies the request into a small array `io`, and
calls `arch_syscall_reg(io)`, with `io[0] = KOS_SYS_CALL_REG` (56), `io[1]` the endpoint,
`io[2]` the packed lengths, `io[3..7]` the payload. `arch_syscall_reg` loads those into
`r5`-`r11`, so the request travels in registers, and executes `svc`.

`SVC_Handler` looks at the syscall number before doing anything else:

```
    ldr     r1, [r0]                /* the stacked r0 is the syscall number */
    cmp     r1, #56                 /* KOS_SYS_CALL_REG (sys/abi.h) */
    bne     .Lsvc_slow
```

For 56 it saves the callee registers exactly as PendSV would, which turns them into the
array `args` (`args[0]` = `r4`, the result slot; `args[1]` the endpoint; and so on), and
calls `kickos_ipc_fastpath(args)` with interrupts masked at the kernel-lock level. That function
follows one rule: decide everything before writing anything. It falls through (returns
`nullptr`, nothing changed) when the caller is cancelled or dying, the payload is too big, the
capability fails to resolve for `CAP_SIGNAL`, the endpoint is in another kernel, no receiver is
parked, the receiver is info-less or dying, the caller outranks the receiver, the receiver
cannot take a reply capability, or the copy is refused. `SVC_Handler` itself also declines
before calling it when interrupts were already masked or a PendSV is already pending. The
priority test is the one that explains the equal priorities of this arm:

```cpp
        if (c->prio > w->prio)
        {
            // A caller outranking the receiver would need priority donation, so the fastpath
            // refuses.
            return nullptr;
        }
```

A fall-through costs one extra trap: the generic dispatch answers `KOS_SYS_CALL_REG` with
`KOS_CALL_REG_FALLBACK`, and the stub re-issues the call as `kos_call_generic`, which is
Stop 4. So `t_call_from_root`, whose client outranks its server, could never have taken this
path, even if it had called `kos_call`.

When nothing refuses, the fastpath does what Stops 5 and 6 did, with two differences. The
request is copied out of the caller's saved registers rather than its buffer. And the caller's
reply target is set to its own saved frame (`reply_wait_seat(c, &args[1], recv_cap)`), marked
with `call_frame_parked`, because this caller has no kernel continuation: it parked inside the
trap handler, not inside a C function that will resume. It then picks the server and returns
`sched::switch_prepare(next)`, and `SVC_Handler` restores the server's saved context and
returns from the exception straight into it. That context is the server's kernel continuation
inside `endpoint_reply_recv`, parked there in Stop 3. On the caller's side there is no PendSV
and no trip through `svc_trampoline`.

The server's side is unchanged: it was parked in an ordinary `kos_reply_recv` and resumes as in
Stop 7. When it replies, `endpoint_reply_locked` copies the answer into the caller's saved frame
(that is where `ipc.buf` points). When the caller is next switched in, `switch_book` sees
`call_frame_parked` and writes the result into the saved `r4` with `arch_ctx_set_syscall_result`.
The exception return then lands in `arch_syscall_reg` just after its `svc`, which copies `r4` and
`r5`-`r9` back into `io`, and `kos_call` copies the reply bytes into the user's `buf`.

**Try it:** start fresh, `break (anonymous namespace)::t_call_reg_fastpath`, `continue`,
`delete`, `break kos_call`, `continue`: `kos_call (ep=2, buf=..., send_len=8, recv_cap=8)`;
`set $ret = $lr & ~1`, `delete`. Then `break arch_syscall_reg`, `continue`, `x/8wx $r0`:
`0x00000038 0x00000002 0x00001008 0x04030201 0x08070605 0x00000000 ...` (56, endpoint 2, the
packed 8|8, the payload bytes 1 to 8). `delete`, `break kickos_ipc_fastpath`, `continue`,
`bt 2` shows `SVC_Handler` as the caller, and `x/8wx args` shows the same words from `args[1]`
on (`args[0]` is whatever `r4` held). `delete`, `break kickos::cap_install_reply`, `continue`:
`p c->name` is `"fpS"`, `p c->prio` and `p caller->prio` are both `11`. `delete`,
`break kickos::sched::switch_prepare`, `continue`: `p next->name` is `"fpS"`. `delete`,
`break kickos::ep_copy`, `continue`: this is the reply, `bt 2` names `endpoint_reply_locked`,
and `p/x dst` is `args + 4`, the caller's saved frame (`0x2002bf18` when `args` was
`0x2002bf14`). `delete`, `break arch_ctx_set_syscall_result`, `continue`: `result=8`, called
from `switch_book`. Last, `delete`, `tbreak *$ret`, `continue`: gdb lands in `fp_caller`,
inside the inlined store of the result into `g_fp_rc` (it shows `__i=8`), and `p $r0` is `8`.

**Why, and the contract:** [Book: Why the fast path is the isolation](../book/synchronous-call-and-reply.md),
[Reference: A copy the boundary check cannot promise](../reference/ipc-call-reply.md) (why this
file discards its `write_recv_info` answer), [12. ARMv6-M](12-armv6m.md),
[14. RISC-V](14-riscv.md) and [16. RX](16-rx.md) for the other fastpath prologues

## What changes elsewhere

- **With an MMU** ([10. The MMU](10-the-mmu.md)): `ep_copy` gets real address spaces, the copy
  can be refused if a page was unmapped under a parked thread, and both ends then see
  `-KOS_EFAULT`. No translating arch builds the register fastpath.
- **Above one kernel core** ([11. Multicore](11-multicore.md)): the register fastpath is refused
  at configure time (`syscall_ipc_fast.cc` asserts `KICKOS_KERNEL_CORES == 1`), and
  `DeferredWake::offer` places extra woken threads on other cores. On an AMP partition a call to
  an endpoint in another kernel leaves through `far_publish` and parks as `WAIT_EP_FAR_REPLY`.
- **A server loop** in real code uses the fused form,
  `kos_reply_recv(reply_cap, buf, lens, &opts)`: Stop 8 and Stop 3 in one kernel entry. The
  `DeferredWake` you met in Stop 8 is what keeps the answered client from running before the
  server has parked again, so the client's next call finds the server waiting and takes Stop 5,
  not Stop 10. `frr_server` in the same selftest file is a two-request example.

## Where to next

- [6. Shared memory](06-shared-memory.md): moving data too large for a 256-byte message.
- [7. An interrupt to a driver](07-interrupt-to-a-driver.md): the notification a server loop
  can accept in the same `kos_reply_recv`.
- [8. Spawn and capabilities](08-spawn-and-caps.md): Stop 2 in full.
- [9. Fault and kill](09-fault-and-kill.md): what happens to a parked client when its server
  dies (the `CAP_REPLY` close arm wakes it with `-KOS_EPIPE`).
- [Reference: Synchronous call/reply IPC](../reference/ipc-call-reply.md), the death matrix
  especially.

## Exercises

1. In `t_call_from_root`, the server runs at `KICKOS_PRIO_MIN`. Predict what Stop 6 shows if the
   server is spawned at priority 2 instead (equal to `main`). Check with `break kickos::sched::set_prio`:
   does it hit from `endpoint_call` at all?
2. `trunc_server` in the same file receives into a 3-byte buffer and replies 8 bytes into a
   3-byte `recv_cap`. Predict `n` at `kickos::ep_copy` for the request and for the reply, and the
   value `kos_call` returns. Break on `(anonymous namespace)::t_call_truncation` and check.
   Then predict whether this call takes the register fastpath (look at the priorities
   `serve_one_call` uses). Check with `break kickos_ipc_fastpath` and `finish`: what does it
   return, and which refusal produced that?
3. After Stop 8, the server's slot 4 was vacated. Predict what a second
   `kos_reply(4, ...)` would return, and which test in `endpoint_reply_locked` returns it.
