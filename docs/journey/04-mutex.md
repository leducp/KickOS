<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 4. A mutex: uncontended, contended, priority inheritance

By the end of this chapter you will have followed a mutex on the `qemu` reference walk, from user
code into the kernel and back. A KickOS mutex is one 32-bit word in the memory of the threads that
share it. You will see the word and the user call, the thread's own id that the kernel keeps in
user memory, the compare-and-swap that takes and releases a free word with no kernel entry at all,
and the C library's two locks built on it. Then the contended case: the kernel's entry reads the
word, checks who it names, keeps a record only while a thread waits, parks the caller, and hands
the word over at the unlock. Then priority inheritance: a low thread holding, a high thread
waiting, the boost, a two-hop chain, and the drop on unlock. Last come a waiter or a holder that
goes away, and the semaphore as a contrast: it parks on the same wait queue but has no owner, so
nothing is ever boosted through it.

The ideas are in two Book chapters: [The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
(how a thread parks and is woken) and [Priority inheritance](../book/priority-inheritance-lending-urgency.md)
(why the mutex exists at all, and the invariants it keeps). This chapter shows the code that
keeps them. It assumes you have read [the first syscall](02-first-syscall.md) (trap, dispatch)
and [sleep](03-sleep.md) (park, wake, the deferred switch).

## The images we use

Two images from the `qemu` build contend a mutex for real:

- **`libcrace`** (`user/apps/common/libcrace/main.cc`). Four worker threads, all named `racer`,
  run at the same priority and call `printf`, `malloc` and `free` in a loop. Every one of those C
  library calls takes one of the C library's two locks, each a mutex word. With four threads and
  a short round-robin slice, a worker often finds a lock held, so the kernel's contended path runs
  thousands of times. Before the workers start, `main` runs a few witnesses that contend a lock on
  purpose, in threads named `mholder`, `mvictim`, `mlibc` and the like. The workers share one
  priority, so nothing is boosted: this is the plain contended case.
- **`selftest`** (`user/apps/common/selftest/main.cc`). Its mutex arms build exact scenarios:
  `t_mutex_pi` (a low thread holds, a high thread waits, a medium thread must not get in),
  `t_mutex_chain` (a boost that travels two hops), `t_mutex_multi_held`, `t_mutex_owner_died`,
  `t_mutex_owner_died_nowaiter` and `t_mutex_deadlock`. We use it for priority inheritance and
  for the ways a mutex is left behind.

Launch either one stopped, as [Setup](00-setup.md) explains, and attach gdb:

```
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/libcrace/libcrace -gdb tcp::1234 -S &
gdb-multiarch build/qemu/user/apps/common/libcrace/libcrace \
    -ex 'target remote :1234'
```

Many recipes read kernel state through one convenience variable, as in
[Spawn and capabilities](08-spawn-and-caps.md):

```
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
```

Three gdb habits for this chapter. First, never `p kickos::sched::current()`: that makes gdb run
kernel code on the target, and if a breakpoint fires inside it the expression is abandoned. Read
`$k->currents.cells[0]` instead, or wait until the kernel has stored the current thread in a local
(`c` in `mutex_lock`) and print that. Second, several stops use `$_caller_is("...")` or
`$_any_caller_is("...", n)` in a breakpoint condition, so a shared function stops only when a mutex
path called it. They are gdb convenience functions that need a gdb built with Python, which the
Debian `gdb-multiarch` is. Third, the compiler inlines most helpers of the mutex path into
`mutex_lock` and `mutex_unlock`. gdb still shows each one as a frame of its own (`lock_attempt`,
`unlock_attempt`, `owner_resolve`), so a condition names those frames. Some of their locals print as
`<optimized out>`: save what you need in a `$` variable at a stop where it is still visible.

The gdb output below is trimmed: long paths are shortened to `...`, and some long frames are cut
with `...`. Line numbers and addresses are from one run of the `qemu` build and will differ on
yours.

### Stop 1: the word and the user call
**Where:** `user/include/kickos/sys/abi.h`, `kos_mutex_t`, `KOS_MUTEX_INIT`, `KOS_MUTEX_WAITERS`;
`user/include/kickos/sys.h`, `kos_mutex_lock`, `kos_mutex_unlock`; `user/src/mutex.cc`;
`user/include/kickos/kos.h`, `kos::Mutex`

```c
typedef struct kos_mutex
{
    uint32_t word;
} kos_mutex_t;
#define KOS_MUTEX_INIT {0u}
#define KOS_MUTEX_WAITERS 0x80000000u
```

A mutex is this one word, naturally aligned, in memory that every thread sharing the mutex can
write. There is no kernel object, no capability and no create call: a static
`kos_mutex_t m = KOS_MUTEX_INIT;` is a ready mutex. The word has two parts. Bits 30 to 0 name the
holder by its **lock id** (Stop 2), and 0 means free. Bit 31, `KOS_MUTEX_WAITERS`, says that a
thread is or was parked on the word, so its release must enter the kernel.

The two calls take the word's address: `kos_mutex_lock(m, timeout_us)` and `kos_mutex_unlock(m)`.
A timeout of `KOS_TIMEOUT_NONE` waits forever, and 0 is a try. The C++ class `kos::Mutex` holds a
`kos_mutex_t`, has a `constexpr` constructor, and cannot be copied: a copy would be another word,
and so another mutex. Its `lock()`, `try_lock()` and `unlock()` forward to the C calls.

The full list of codes the lock can return is in `user/include/kickos/sys.h`. Read it there. One
code needs a warning even at this distance: `-KOS_EOWNERDEAD` is negative and yet means **you hold
the mutex** (its previous holder died holding it). A caller that treats every negative return as
"not acquired" walks away from a mutex it holds. Stops 8 and 14 show where that code comes from.

**Try it:** on `libcrace`, `break kos_mutex_lock`, `continue`:

```
Breakpoint 1, kos_mutex_lock (m=0x20011c4c <(anonymous namespace)::s_libc>, timeout_us=4294967295) at .../user/src/mutex.cc:22
(gdb) p *m
$1 = {word = 0}
(gdb) ptype m
type = struct kos_mutex {
    uint32_t word;
} *
(gdb) p sizeof(*m)
$2 = 4
```

The first lock of the run is `main`'s first `printf`, and the word it locks is the C library's
lock, `s_libc` (Stop 4). It reads 0: free.

**Why, and the contract:** [Book: Priority inheritance](../book/priority-inheritance-lending-urgency.md),
[Reference: invariants](../reference/invariants.md) (`mutex-word-is-keyed-on-its-memory`)

### Stop 2: the seated id: a thread knows who it is with one load
**Where:** `kernel/include/kickos/lock_id.h`, `lock_id_pack`, `LOCK_ID_SLOT_BITS`;
`kernel/include/kickos/thread.h`, `ThreadPool::lock_id_for`, `Thread::lock_id`;
`kernel/thread/reent.cc`, `reent_seat`; `kernel/sched/sched.cc`, `seat_reent`;
`user/src/newlib_reent.cc`, `kickos_self_lock_id`, `s_self`

To take a word, a thread writes its own id into it, so it must know that id without asking the
kernel. The kernel gives each thread a **lock id** when it creates it:

```cpp
    constexpr uint32_t lock_id_pack(uint32_t gen, uint32_t slot)
    {
        return ((gen & LOCK_ID_GEN_MASK) << LOCK_ID_SLOT_BITS) | (slot + 1u);
    }
```

The low bits are the thread's slot in the thread pool, plus one, so no id is 0. The bits above
are the slot's generation, a counter that moves on each time the slot is used again, so a dead
thread's id does not name the slot's next thread. `LOCK_ID_SLOT_BITS` is as wide as the number of
slots needs. On this build there are 17 slots (`KICKOS_THREAD_SLOTS`), so the slot part is the low
5 bits. Root is slot 0 with generation 0, so its id is 1. Each `Thread` keeps its own id in
`Thread::lock_id`.

The kernel then **seats** the running thread's id in user memory. On this one-core board it is
one word of the image's data, `s_self`, beside the C library's per-thread pointer `_impure_ptr`. On
every switch, `reent_seat` stores the incoming thread's id there, and reading it is one load.
`kos_thread_self` derives the thread's handle from the same word, so it makes no kernel entry
either. Other boards keep the id elsewhere (a word of the thread's TLS control block, or a field
per thread slot), always where one load reads it. The kernel never reads this word back: every
kernel path takes the caller from its own record of the running thread. A thread that overwrites
its copy misleads only its own locks.

A loop that only reads its own id, or only takes and releases free words, makes no kernel entry.
So it is not a point where `kos_thread_kill` can end the thread (POSIX treats a mutex lock the same
way), while `kos_thread_slay` still reaches it.

**Try it:** on `libcrace`:

```
(gdb) x/3i kickos_self_lock_id
   0x26c0 <kickos_self_lock_id()>:	ldr	r3, [pc, #4]	@ (0x26c8 <kickos_self_lock_id()+8>)
   0x26c2 <kickos_self_lock_id()+2>:	ldr	r0, [r3, #0]
   0x26c4 <kickos_self_lock_id()+4>:	bx	lr
(gdb) break kickos::reent_seat
(gdb) continue
Breakpoint 1, kickos::reent_seat (space=space@entry=0x0, state=0x20010700 <s_reent>, self=1) at .../kernel/thread/reent.cc:129
(gdb) p/x self
$1 = 0x1
(gdb) p/x s_self
$2 = 0x0
(gdb) finish
(gdb) p/x s_self
$3 = 0x1
```

The first `ldr` fetches the address of `s_self` from a constant stored beside the code, and the
second loads the id. The first seat is the first switch of the run, into root: id 1 written over 0.

**Why, and the contract:** [Reference: porting, "Thread pointer and kernel stacks"](../reference/porting.md),
[Reference: invariants](../reference/invariants.md) (`task-scoped-death-and-a-cancel-that-reaches-every-park`)

### Stop 3: the user compare-and-swap: a free word costs no kernel entry
**Where:** `user/src/mutex.cc`, `kos_mutex_lock`, `kos_mutex_unlock`;
`user/include/kickos/sys/user_cas.h`, `kickos_user_cas`; `user/src/arch/armv7m/user_cas.S`

```cpp
int kos_mutex_lock(kos_mutex_t* m, uint32_t timeout_us)
{
    uint32_t const self = kickos_self_lock_id();
    uint32_t const held = kickos_user_cas(&m->word, 0u, self);
    if (held == 0u)
    {
        return 0;
    }
    if ((held & ~KOS_MUTEX_WAITERS) == self)
    {
        return -KOS_EDEADLK;
    }
    return kickos_mutex_lock_entry(m, timeout_us);
}
```

`kickos_user_cas(word, expected, desired)` stores `desired` if the word holds `expected`, and
returns what the word held. The lock asks for 0 to become `self`: a free word becomes yours. That
is the whole uncontended lock. If the word already names you, the lock is refused with
`-KOS_EDEADLK`, still in user code: the mutex is not recursive. Otherwise someone else holds it,
and only then does the thread enter the kernel (Stop 5). A try (timeout 0) also enters once on a
held word. The kernel answers `-KOS_ETIMEDOUT` at once for a live holder, and the entry is how it
notices a dead one (Stop 8).

The unlock is the mirror image: it asks for `self` to become 0, and returns 0 when the word was
exactly `self`. A word naming another thread gets `-KOS_EPERM`, again in user code. Only a word
that names you with `KOS_MUTEX_WAITERS` set makes the unlock enter the kernel, to hand the word to
a waiter (Stop 10).

On a Cortex-M the compare-and-swap is a load-exclusive and store-exclusive pair. The first branch
is for ARMv8-M Mainline, and the second is the one this Cortex-M4 runs:

```
kickos_user_cas:
#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
1:  ldaex   r3, [r0]
    cmp     r3, r1
    bne     2f
    stlex   r12, r2, [r0]
#else
1:  ldrex   r3, [r0]
    cmp     r3, r1
    bne     2f
    strex   r12, r2, [r0]
#endif
    cmp     r12, #0
    bne     1b
    movs    r0, r3
    bx      lr
2:  clrex
    movs    r0, r3
    bx      lr
```

`ldrex` loads the word and marks it for exclusive access. `strex` stores only if nothing has
cleared that mark since, and writes 0 to `r12` when it stored. Every exception entry clears the
mark, and the switch clears it again with `clrex` before it resumes a thread, so a thread
preempted between the two instructions fails its store and starts again from the load. It never writes over a value another thread stored in between. Each architecture has
its own routine under `user/src/arch/`, and some need help from the kernel to keep that promise.
The porting reference lists what each one needs.

**Try it:** on `libcrace`, set breakpoints on the kernel's two mutex paths first, so you can see
that they do not fire. The routine is assembly with no frame information, so read its arguments in
registers (`r0` the word, `r1` the expected value, `r2` the new one) and its caller with
`info symbol $lr`. To stop where it returns, put a temporary breakpoint on the return address,
with the Thumb bit cleared:

```
(gdb) break kickos::mutex_lock
(gdb) break kickos::mutex_unlock
(gdb) break kickos_user_cas
(gdb) continue
Breakpoint 3, kickos_user_cas () at .../user/src/arch/armv7m/user_cas.S:21
(gdb) info symbol $lr
kos_mutex_lock + 23 in section .text
(gdb) info registers r0 r1 r2
r0             0x20011c4c          536943692
r1             0x0                 0
r2             0x2                 2
(gdb) p $w = (unsigned*)$r0
$1 = (unsigned int *) 0x20011c4c <(anonymous namespace)::s_libc>
(gdb) tbreak *($lr & ~1)
(gdb) continue
Temporary breakpoint 4, kos_mutex_lock (m=0x20011c4c <(anonymous namespace)::s_libc>, timeout_us=4294967295) at .../user/src/mutex.cc:24
(gdb) p/x $r0
$2 = 0x0
(gdb) x/xw $w
0x20011c4c <_ZN12_GLOBAL__N_1L6s_libcE>:	0x00000002
(gdb) continue
Breakpoint 3, kickos_user_cas () at .../user/src/arch/armv7m/user_cas.S:21
(gdb) info symbol $lr
kos_mutex_unlock + 21 in section .text
(gdb) info registers r0 r1 r2
r0             0x20011c4c          536943692
r1             0x2                 2
r2             0x0                 0
(gdb) tbreak *($lr & ~1)
(gdb) continue
Temporary breakpoint 5, kos_mutex_unlock (m=0x20011c4c <(anonymous namespace)::s_libc>) at .../user/src/mutex.cc:39
(gdb) p/x $r0
$3 = 0x2
(gdb) x/xw $w
0x20011c4c <_ZN12_GLOBAL__N_1L6s_libcE>:	0x00000000
```

The take swaps 0 for 2, which is `main`'s id (slot 1, generation 0), and returns 0, what it found.
The release swaps 2 back for 0 and returns 2. Breakpoints 1 and 2 never stopped in between: the
kernel's mutex code did not run for either call.

**Why, and the contract:** [Book: An atomic buys definedness, not atomicity](../book/an-atomic-buys-definedness-not-atomicity.md),
[Reference: porting, "Compare-and-swap at both levels"](../reference/porting.md),
[Reference: invariants](../reference/invariants.md) (`user-cas-fails-across-a-return`)

### Stop 4: the C library's locks are two words
**Where:** `user/src/newlib_lock.cc`, `LibcLock`, `s_libc`, `s_malloc`, `holder_of`,
`libc_lock_take`, `libc_lock_acquire`, `libc_lock_release`, `kos_libc_lock`;
`user/include/kickos/sys.h`, `kickos_mutex_lock_entry`

The C library (newlib) protects its own shared state, the `FILE` buffers and the heap, with locks.
It lets the system supply them by defining the `__retarget_lock_*` functions. KickOS maps them onto
**two** mutex words in the static data of whatever owns the heap (on this MPU board, the whole
image): one for `malloc`, one for everything else, so a `printf` does not wait behind another
thread's allocation. The kernel holds nothing for them until a thread has to wait.

```cpp
    struct LibcLock
    {
        kos_mutex_t m;
        // Written only by the holder.
        unsigned depth;
    };

    LibcLock s_libc = {KOS_MUTEX_INIT, 0};
    LibcLock s_malloc = {KOS_MUTEX_INIT, 0};
```

newlib nests its locks (`printf` can take a `FILE` lock and then the `FILE` table's), and one
mutex is not recursive. So the recursion is done here, and the word itself says who holds it:

```cpp
    void libc_lock_acquire(LibcLock& l)
    {
        uint32_t const self = kickos_self_lock_id();
        refuse_inversion(l, self);
        if (holder_of(l) == self)
        {
            l.depth++;
            return;
        }
        (void)libc_lock_take(l, KOS_TIMEOUT_NONE);
    }
```

`holder_of` reads the owner bits of the word. The word names the caller only while the caller
holds it, so a thread that finds its own id there really is the holder, and only the holder
touches `depth`. A nested take is a load, a compare and an add.

`libc_lock_take` adds the rules a C library needs. A wait that a kill cancelled
(`-KOS_ECANCELED`) asks again through `kickos_mutex_lock_entry`, straight to the kernel and never
through the compare-and-swap. So the killed thread ends at that kernel entry, and never takes, with
no entry at all, a lock that was freed meanwhile. Any other refusal traps the thread, and so does
`-KOS_EOWNERDEAD` on malloc's lock: a heap that a dead thread left half edited would corrupt every
later allocation silently. The libc lock accepts `-KOS_EOWNERDEAD`. Taking the libc lock while
holding malloc's traps too, since newlib never takes them in that order. A program that needs the
word itself, for instance to try it, gets it from `kos_libc_lock(KOS_LIBC_LOCK_STDIO)` or
`kos_libc_lock(KOS_LIBC_LOCK_MALLOC)`.

**Try it:** on `libcrace`, `break kos_mutex_lock`, `continue`, `bt 7`:

```
#0  kos_mutex_lock (m=0x20011c4c <(anonymous namespace)::s_libc>, timeout_us=4294967295) at .../user/src/mutex.cc:22
#1  (anonymous namespace)::libc_lock_take (l=..., timeout_us=4294967295) at .../user/src/newlib_lock.cc:47
#2  __sfp_lock_acquire () at .../newlib/libc/stdio/findfp.c:269
#3  __sinit (s=0x20010840 <s_reent+320>) at .../newlib/libc/stdio/findfp.c:247
#4  _vfprintf_r (data=0x20010840 <s_reent+320>, fp=0x20011cf0 <__sf+104>, fmt0=0x15125 "libcrace: %d workers x %d rounds\n", ...) at .../newlib/libc/stdio/vfprintf.c:599
#5  printf (fmt=0x15125 "libcrace: %d workers x %d rounds\n") at .../newlib/libc/stdio/printf.c:56
#6  kickos_app_main () at .../user/apps/common/libcrace/main.cc:653
```

Then `finish` twice (the lock returns `0`, the take `true`) and print both locks and the seat:

```
(gdb) p/x '(anonymous namespace)::s_libc'
$3 = {m = {word = 0x2}, depth = 0x1}
(gdb) p/x '(anonymous namespace)::s_malloc'
$4 = {m = {word = 0x0}, depth = 0x0}
(gdb) p/x s_self
$5 = 0x2
```

The libc lock now holds `main`'s id, 2, at depth 1, and malloc's is free. The take is inlined into
newlib's `__sfp_lock_acquire`, so the second `finish` lands in `__sinit`.

**Why, and the contract:** [Book: What's under #include](../book/whats-under-include-libc-and-the-cxx-runtime.md),
[Reference: invariants](../reference/invariants.md) (`libc-locks-belong-to-the-heap-owner`)

### Stop 5: the kernel's entry: two syscalls that take an address
**Where:** `kernel/syscall/syscall.cc`, `syscall_dispatch`, `syscall_body`, the `KOS_SYS_MUTEX_LOCK`
and `KOS_SYS_MUTEX_UNLOCK` cases; `user/src/mutex.cc`, `kickos_mutex_lock_entry`

The kernel's part begins only when the compare-and-swap found the word held. The two arms are
short:

```cpp
        case KOS_SYS_MUTEX_LOCK:
        {
            // -KOS_EOWNERDEAD is negative but still an ACQUIRE.
            return static_cast<uint64_t>(mutex_lock(a0, static_cast<uint32_t>(a1)));
        }
        case KOS_SYS_MUTEX_UNLOCK:
        {
            return static_cast<uint64_t>(mutex_unlock(a0));
        }
```

`a0` is the word's address and nothing more: there is no handle to resolve and no right to check.
The kernel treats the address, and what the word holds, as hostile input (Stop 7). Neither arm
opens the kernel's critical section, the `IrqLock`. `mutex_lock` may park, and the parked thread
must read its result with the critical section closed (Stop 10), so both functions open a short
`IrqLock` of their own for each attempt.

One more line, in the dispatcher, is about the unlock:

```cpp
    if (caller->cancel_kind != CANCEL_NONE and not caller->dying and nr != KOS_SYS_MUTEX_UNLOCK)
    {
        sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN);
    }
```

A thread that `kos_thread_kill` has marked ends at its next syscall entry
([Fault and kill](09-fault-and-kill.md)). The unlock is the one exception: a holder killed while it
runs gives the lock back, and dies at its next entry. Without it, a killed holder whose release had
a waiter would die holding the lock, and the waiter would be told `-KOS_EOWNERDEAD` about state its
holder had finished changing.

**Try it:** on `libcrace`, `break kickos::mutex_lock`, `continue`:

```
Breakpoint 1, kickos::mutex_lock (va=va@entry=536943684, timeout_us=timeout_us@entry=4294967295) at .../kernel/sync/sync.cc:657
(gdb) bt
#0  kickos::mutex_lock (va=va@entry=536943684, timeout_us=timeout_us@entry=4294967295) at .../kernel/sync/sync.cc:657
#1  (anonymous namespace)::syscall_body (nr=24, a0=536943684, a1=4294967295, a2=0, a3=0) at .../kernel/syscall/syscall.cc:530
#2  syscall_dispatch (nr=24, a0=536943684, a1=4294967295, a2=0, a3=0) at .../kernel/syscall/syscall.cc:421
#3  svc_trampoline () at .../arch/arm/armv7m/switch.S:540
(gdb) p/x va
$1 = 0x20011c44
(gdb) p $k->currents.cells[0]->name
$2 = "mvictim\000\000\000\000\000\000\000\000"
(gdb) p/x '(anonymous namespace)::s_malloc'
$3 = {m = {word = 0x3}, depth = 0x1}
```

`nr=24` is `KOS_SYS_MUTEX_LOCK`, and `a0` is the word's address, `0x20011c44`: malloc's lock. The
first contended lock of the run is `mvictim`'s, one of `main`'s witnesses. It wants malloc's lock
while `mholder` holds it (id 3: slot 2, generation 0). The backtrace stops at `svc_trampoline`
because the user frames are on the other stack (the process stack, PSP).

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md),
[Reference: invariants](../reference/invariants.md) (`task-scoped-death-and-a-cancel-that-reaches-every-park`)

### Stop 6: the kernel keeps a record only while a thread waits
**Where:** `kernel/include/kickos/mutex.h`, `struct Mutex`; `kernel/include/kickos/instance.h`,
`mutex_records`, `mutex_active`; `kernel/include/kickos/thread.h`, `Thread` fields `prio`,
`base_prio`, `held_list`, `wait_kind`, `wait_obj`, `wait_va`, `wait_result`

```cpp
    struct Mutex
    {
        // The word's physical address (kickos/word.h).
        arch_phys_addr_t key = 0;
        Thread* owner = nullptr;
        List waiters;
        Mutex* next_held = nullptr;   // intrusive link in the owner's held list
        Mutex* prev_active = nullptr; // intrusive links in the kernel's active list
        Mutex* next_active = nullptr;
    };
```

Despite its name, this is not the mutex: the mutex is the word. This is the kernel's **record** of
a word that some thread waits on. It lives only while it has a waiter: it is taken at the first
park on a word and freed when its last waiter leaves. While it lives, `owner` is the authority on
who holds the word, never the word itself. `key` is the word's physical address, so two threads
find the same record only when they name the same memory (Stop 7). `waiters` is a **wait queue**,
the same intrusive list of parked threads every blocking object uses ([sleep](03-sleep.md) met
it). `next_held` chains every record one thread owns into that thread's `held_list`. Priority
inheritance needs that list to answer "who is waiting on anything I hold?". `prev_active` and
`next_active` chain the live records both ways into the kernel's list, `mutex_active`, so a record
whose last waiter leaves comes off it in one step.

The records sit in a fixed array inside the kernel instance, `Mutex mutex_records[KICKOS_THREAD_SLOTS]`.
One per thread slot is always enough: each waiter parks on one record, and a record with no waiter
is free, so there are never more live records than threads. There is no heap in the kernel, and
no budget per mutex: a program may use as many words as it likes.

The other half of the state is on the threads. Each `Thread` carries two priorities: `prio`, the
**effective** priority the scheduler reads, and `base_prio`, the one it was given. Inheritance only
ever raises `prio`. A parked thread also carries a **wait edge**: `wait_kind` (what it waits for)
and `wait_obj` (for a mutex, the record), plus `wait_va`, the address through which it named the
word, and `wait_result`, where whoever wakes it writes the outcome.

**Try it:** on `libcrace`, `break kickos::mutex_lock`, `continue`:

```
(gdb) p sizeof($k->mutex_records) / sizeof($k->mutex_records[0])
$1 = 17
(gdb) p $k->mutex_records[0]
$2 = {key = 0, owner = 0x0, waiters = {head = 0x0, tail = 0x0}, next_held = 0x0, prev_active = 0x0, next_active = 0x0}
(gdb) p $k->mutex_active
$3 = (kickos::Mutex *) 0x0
```

At the first contended lock of the run no thread waits anywhere: the records are free, one per
thread slot, and the active list is empty.

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
[Reference: invariants](../reference/invariants.md) (`mutex-owner-death-reaches-the-next-taker`)

### Stop 7: the contended entry reads the word
**Where:** `kernel/sync/sync.cc`, `mutex_lock`, `lock_attempt`, `record_find`, `record_take`,
`owner_resolve`; `kernel/syscall/word.cc`, `word_ref`, `word_could_hold`;
`kernel/include/kickos/word.h`, `WordAccess`

`mutex_lock` calls `lock_attempt` in a loop, each attempt inside its own `IrqLock`. An attempt
whose compare-and-swap fails drops the lock and starts again from the top, so a killed thread
reaches its death point, and a word that keeps changing makes only the threads writing it spin.
One attempt, in order:

1. **The cancel check** (`park_cancel_pending`). A thread told to die exits here, before it
   changes anything.
2. **`word_ref(c, va, &w)`**: may the caller name this word, and what does it name? It refuses a
   misaligned word (`-KOS_EINVAL`), a word the caller cannot write (`-KOS_EFAULT`), and a word in
   uncached or device memory or in the AMP partition's shared memory (`-KOS_EINVAL`). It answers
   the **key**: the word's physical address. On this MPU board that is the address itself. On a
   board with an MMU it is the frame the address maps to, so one piece of memory mapped at two
   addresses, in one address space or two, is one mutex. Nothing reads the word before this
   check.
3. **`WordAccess word(w)`, then `word.load()`**: the read. The kernel reaches a user word only
   through a `WordAccess`, one per attempt, and its reads and writes go through it. On a board with
   an MMU it holds the kernel's own mapping of the word's page from the first read to the last
   write, and lets it go before the park. On a board without one it is inline, a plain access at
   the word's own address.
4. **`record_find(w.key)`**: does a thread already wait on this word?
   - With a record, the record is the authority. A word whose owner bits disagree with
     `m->owner` was broken by a writer: `-KOS_EINVAL`, and nothing is written. A record the caller
     owns is `-KOS_EDEADLK`. A cleared `KOS_MUTEX_WAITERS` is set again. The holder is the record's
     owner.
   - With no record, the holder is whoever the word names. A word that is 0 now was freed
     meanwhile: one compare-and-swap takes it and the call returns 0. The caller's own id is
     `-KOS_EDEADLK`. Any other id goes to `owner_resolve`.
5. **A try** (timeout 0) answers `-KOS_ETIMEDOUT` here: no boost, no deadline, no park.
6. **Pass 1**, the cycle check of Stop 12.
7. **The record.** With none yet, a compare-and-swap sets `KOS_MUTEX_WAITERS` in the word and
   `record_take` keys a free record on the word, owned by the holder, on the holder's held list
   and the active list. The caller's `wait_va` becomes `va`.
8. **Pass 2**, the boost of Stop 11, then the deadline and the park (Stop 9).

Every refusal and every retry comes before step 7, and step 7 leads only to the park.

The id read from the word is only a candidate:

```cpp
        Thread* owner_resolve(uint32_t id, WordRef const& w)
        {
            uint32_t const slot = id & LOCK_ID_SLOT_MASK;
            if (slot == 0 or slot > static_cast<uint32_t>(KICKOS_THREAD_SLOTS))
            {
                return nullptr;
            }
            Thread* const t = &kernel().threads.slots[slot - 1u];
            // INACTIVE is a slot never used or still being built, which has held nothing.
            if (t->state == ThreadState::EXITED or t->state == ThreadState::INACTIVE or t->dying
                or t->lock_id != id or not word_could_hold(t, w))
            {
                return nullptr;
            }
            return t;
        }
```

The kernel acts on the id only when its slot holds a live thread that is not on its way out
(`dying`), with exactly that id (so the generation matches), and that could have written this word
with its own memory access (`word_could_hold`). On this board that means the thread's MPU regions
let it write the word. Any other id names no live holder (Stop 8). So a forged word can make the
kernel boost only a thread that could have taken the lock itself.

On this board that is wider than it sounds. Every app's static data sits in one window that every
task can write, so a task can write a static word naming a live thread of another task and wait on
it. The kernel then boosts that thread to the waiter's priority, possibly above the priority ceiling
of its own task, for as long as the waiter waits. The boost lends only the waiter's own priority,
and this board already lets any task write any other task's static data. A try of the same word
tells the forger whether that thread is alive: `-KOS_ETIMEDOUT` for a live one, `-KOS_EOWNERDEAD`
for a dead one. On a
translating board the named thread's space must map the forger's own frame, so a forged id in
private memory is stolen as dead.

**Try it:** on `libcrace`, `mvictim`'s lock again, with one breakpoint per step. Each `finish`
returns to `lock_attempt`:

```
(gdb) break kickos::mutex_lock
(gdb) continue
(gdb) p/x va
$1 = 0x20011c44
(gdb) p $k->mutex_active
$2 = (kickos::Mutex *) 0x0
(gdb) delete
(gdb) break kickos::word_ref
(gdb) continue
Breakpoint 2, kickos::word_ref (c=c@entry=0x200052e0 <kickos::detail::g_instance+2848>, va=va@entry=536943684, out=...) at .../kernel/syscall/word.cc:110
(gdb) p $o = out
$3 = (kickos::WordRef *) 0x20000fa0 <kickos::(anonymous namespace)::g_kstacks+3904>
(gdb) finish
Value returned is $4 = 0
(gdb) p/x *$o
$5 = {key = 0x20011c44, space = 0x0, va = 0x20011c44}
(gdb) delete
(gdb) break kickos::WordAccess::load
Breakpoint 3 at 0x7158: kickos::WordAccess::load. (2 locations)
(gdb) continue
Breakpoint 3.1, kickos::WordAccess::load (this=<synthetic pointer>) at .../kernel/include/kickos/word.h:94
(gdb) finish
(gdb) p/x *(unsigned*)$o->va
$6 = 0x3
(gdb) delete
(gdb) break kickos::word_could_hold
(gdb) continue
Breakpoint 4, kickos::word_could_hold (t=0x20005188 <kickos::detail::g_instance+2504>, w=...) at .../kernel/syscall/word.cc:147
(gdb) p t->name
$7 = "mholder\000\000\000\000\000\000\000\000"
(gdb) p/x t->lock_id
$8 = 0x3
(gdb) finish
0x0000725a in kickos::(anonymous namespace)::owner_resolve (id=3, w=...) at .../kernel/sync/sync.cc:416
Value returned is $9 = true
(gdb) delete
(gdb) break arch_cas_u32
(gdb) continue
Breakpoint 5, arch_cas_u32 (word=word@entry=0x20011c44 <(anonymous namespace)::s_malloc>, expected=..., desired=desired@entry=2147483651) at .../arch/include/kickos/arch/atomic_cas_masked.h:17
(gdb) p/x *expected
$10 = 0x3
(gdb) p/x desired
$11 = 0x80000003
(gdb) finish
Value returned is $12 = true
(gdb) p/x *(unsigned*)$o->va
$13 = 0x80000003
(gdb) delete
(gdb) break kickos::wq_block
(gdb) continue
Breakpoint 6.3, kickos::(anonymous namespace)::lock_attempt (lock=..., c=0x200052e0 <...>, va=536943684, ...) at .../kernel/sync/sync.cc:582
(gdb) p $m = m
$14 = (kickos::Mutex *) 0x20004a00 <kickos::detail::g_instance+576>
(gdb) p/x *$m
$15 = {key = 0x20011c44, owner = 0x20005188, waiters = {head = 0x0, tail = 0x0}, next_held = 0x0, prev_active = 0x0, next_active = 0x0}
(gdb) p $m->owner->name
$16 = "mholder\000\000\000\000\000\000\000\000"
(gdb) p $m->owner->held_list == $m
$17 = true
(gdb) p $k->mutex_active == $m
$18 = true
(gdb) p/x c->wait_va
$19 = 0x20011c44
```

Read it in order. `word_ref` accepts the word, and the key is the address itself (this board has no
address spaces, so `space` is null). `WordAccess::load` is inline in its two callers, the lock's
and the unlock's, so the breakpoint has two locations and `finish` prints no value: the word holds
`0x3`, `mholder`'s id. `word_could_hold` is asked about `mholder` and answers true, so the holder is
live. The compare-and-swap, `arch_cas_u32` in the one-core form this board builds, sets the waiters
bit: `0x3` becomes `0x80000003`. `wq_block` is inlined
at six places, so gdb stops at the one in `lock_attempt`. There the record is keyed on the word, owned by `mholder`, first on `mholder`'s held
list and on the active list, and `mvictim`'s `wait_va` is the word's address. The record's queue is
still empty: `wq_block` appends `mvictim` next.

**Why, and the contract:** [Reference: invariants](../reference/invariants.md)
(`mutex-word-is-keyed-on-its-memory`, `ring0-reaches-user-pages-only-through-kaccess`),
[Book: Priority inheritance](../book/priority-inheritance-lending-urgency.md)

### Stop 8: a dead holder's word is taken, and the taker is told
**Where:** `kernel/sync/sync.cc`, `lock_attempt` (the branch with no record), `owner_resolve`;
`user/apps/common/selftest/main.cc`, `t_mutex_owner_died_nowaiter`, `od_solo_owner`

A thread that dies holding a word that nobody waits for was never on the kernel's books. Its id
stays in the word, and that id is the mark. The next taker's compare-and-swap fails, since the word
is not 0, so it enters the kernel. There it finds no record, and `owner_resolve` finds the id dead.
The kernel then takes the word for the caller with one compare-and-swap, from the dead id to the
caller's id, and answers `MUTEX_OWNER_DIED`, which is `-KOS_EOWNERDEAD`:

```cpp
                    if (t == nullptr)
                    {
                        uint32_t expected = v;
                        if (not word.cas(&expected, c->lock_id))
                        {
                            return Attempt::RETRY;
                        }
                        *rc = 0;
                        if (id != 0)
                        {
                            *rc = static_cast<int>(MUTEX_OWNER_DIED);
                        }
                        return Attempt::ANSWERED;
                    }
```

Exactly one taker is told: its own id replaces the dead one, so the next taker finds a live holder
or a free word. The holder's id was written by its own compare-and-swap, so there is no moment at
which the word is held and names nobody. A thread ended between any two of its instructions leaves
the word free or naming itself. A dead id reads as live only when the generation of its slot has
gone all the way round to the same value and a new thread there could write the word. The
invariant below states that limit. A holder that dies with waiters is the other case: its death
hands the word to a waiter (Stop 14).

**Try it:** on `selftest`. In `t_mutex_owner_died_nowaiter`, `odSolo` locks a fresh word and exits,
and then `main` locks it. Stop on that word. `0x1f` is the slot mask of this build:

```
(gdb) break kickos::mutex_lock if va == (unsigned)&'(anonymous namespace)::g_ods_m'
(gdb) continue
(gdb) p $va = va
$1 = 536939872
(gdb) p $k->currents.cells[0]->name
$2 = "main", '\000' <repeats 11 times>
(gdb) p/x $k->currents.cells[0]->lock_id
$3 = 0x2
(gdb) p/x *(unsigned*)va
$4 = 0x483
(gdb) p $s = &$k->threads.slots[(*(unsigned*)va & 0x1f) - 1]
$5 = (kickos::Thread *) 0x20005188 <kickos::detail::g_instance+2504>
(gdb) p $s->name
$6 = "odSolo\000\000\000\000\000\000\000\000\000"
(gdb) p $s->state
$7 = {v_ = kickos::ThreadState::EXITED}
(gdb) p/x $s->lock_id
$8 = 0x483
(gdb) p $k->mutex_active
$9 = (kickos::Mutex *) 0x0
(gdb) finish
Value returned is $10 = -130
(gdb) p/x *(unsigned*)$va
$11 = 0x2
```

The word holds `0x483`: the low five bits are 3, so slot 2, and the generation above them is 36.
Slot 2 is `odSolo`, with that same id, and `EXITED`. Nobody waits, so there is no record. The lock
returns `-130`, `-KOS_EOWNERDEAD`, and the word now holds `main`'s id, 2.

**Why, and the contract:** [Book: Priority inheritance, "Owner-died"](../book/priority-inheritance-lending-urgency.md),
[Reference: invariants](../reference/invariants.md) (`mutex-owner-death-reaches-the-next-taker`)

### Stop 9: the contended lock parks the caller
**Where:** `kernel/sync/sync.cc`, `mutex_lock`, `lock_attempt` (the two passes and the park),
`wq_block`, `park_queueless`; `kernel/include/kickos/thread.h`, `Thread::wait_mutex`, `WAIT_MUTEX`

When a live thread holds the word, the caller must wait. Before it parks it lends its priority
down the chain of holders (Stops 11 and 12). The walk follows `wait_mutex()`: for a thread parked
on a mutex it returns that record, and for every other kind of wait it returns null, which ends the
walk. It runs twice. Pass 1 only reads: if the chain leads back to the caller, locking would
deadlock, and the call returns `-KOS_EDEADLK` having written nothing. Pass 2 does the boosting. In
`libcrace` all workers share priority 10, so pass 2 stops at its first test (`o->prio >= c->prio`)
and boosts nothing.

The park is the last thing `lock_attempt` does. Back in `mutex_lock`, with the critical section
closed, comes the resume:

```cpp
            if (got == Attempt::PARKED)
            {
                break;
            }
        }
        // The wait_result read must be BOTH outside the critical section AND after the
        // barrier, or it returns the pre-block value.
        wq_confirm_resume(c, epoch);
        return static_cast<int>(c->wait_result);
```

`wq_block` is the shared park used by every blocking object. It marks the thread `BLOCKED`, takes
it off the ready list, seats the wait edge (`WAIT_MUTEX` and the record), appends it to the
record's `waiters`, and asks the scheduler to switch away. It returns the thread's `switch_count`
before the switch, called the epoch:

```cpp
    uint32_t wq_block(List& q, WaitKind kind, void* obj, Thread* woken, Held held)
    {
        Thread* c = sched::current();
        uint32_t const epoch = park_queueless(c, kind, obj, held);
        c->wait_queue = &q;
        q.push_back(&c->link);
        // BEFORE the reschedule, which on a stall never returns.
        KOS_TRACE(::kickos::KOS_TR_PARK, KOS_TRACE_ID(c), KOS_TRACE_ID(&q));
        sched::reschedule(woken, held);
        return epoch;
    }
```

The edge and the queue link are written by the same call, inside the same critical section.
Another locker walking the chain a moment later must already see this thread as "waiting on this
record", or it would stop the walk one hop short.

The queue is not sorted. A thread is appended at the tail, and the waker scans for the highest
priority at pop time. That is what lets a boost change a parked thread's priority with no re-queue.

**Try it:** on `libcrace`, stop at a worker's contended lock. The condition reads the running
thread's name from `$k->currents.cells[0]`, which skips the witnesses `main` runs before its workers
start. Save the thread and the word:

```
(gdb) break kickos::mutex_lock if $_streq($k->currents.cells[0]->name, "racer")
(gdb) continue
(gdb) p $c = $k->currents.cells[0]
$1 = (kickos::Thread *) 0x200052e0 <kickos::detail::g_instance+2848>
(gdb) p $c->prio
$2 = 10 '\n'
(gdb) p/x *(unsigned*)va
$3 = 0xc3
(gdb) p $va = va
$4 = 536943692
(gdb) p $k->mutex_active
$5 = (kickos::Mutex *) 0x0
```

The word holds `0xc3`, another worker's id, with no waiters bit yet. Now `delete`,
`break kickos::mutex_unlock if va == $va`, `continue`. The holder is giving the word back, and the
worker you saved is parked:

```
(gdb) p $k->currents.cells[0] == $c
$6 = false
(gdb) p $c->state
$7 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) p $c->wait_kind
$8 = kickos::WAIT_MUTEX
(gdb) p $c->wait_obj
$9 = (void *) 0x20004a00 <kickos::detail::g_instance+576>
(gdb) p/x $c->wait_va
$10 = 0x20011c4c
(gdb) p $m = (kickos::Mutex*)$c->wait_obj
$11 = (kickos::Mutex *) 0x20004a00 <kickos::detail::g_instance+576>
(gdb) p/x $m->key
$12 = 0x20011c4c
(gdb) p $m->owner == $k->currents.cells[0]
$13 = true
(gdb) p $m->owner->prio
$14 = 10 '\n'
(gdb) p/x *(unsigned*)va
$15 = 0x800000c3
(gdb) p/x $m->owner->lock_id
$16 = 0xc3
```

The record is keyed on the libc lock's word, its owner is the thread unlocking, and the word now
carries the waiters bit, which is why this unlock entered the kernel. All workers have the same
name, `racer`, so compare thread addresses, not names.

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
[Reference: invariants](../reference/invariants.md) (the deferred-switch entries)

### Stop 10: the holder unlocks and hands the word over
**Where:** `kernel/sync/sync.cc`, `mutex_unlock`, `unlock_attempt`, `wq_peek_highest`,
`wq_pop_highest`, `transfer_to`, `record_free`, `wq_confirm_resume`;
`kernel/include/kickos/word.h`, `WordAccess`; `kernel/sched/sched.cc`, `sched::wake`

`unlock_attempt` starts as the lock does: `word_ref`, the `WordAccess` and its load, `record_find`. With no record, the
waiters bit is left over from a waiter that left (Stop 14): if the word names the caller, one
compare-and-swap sets it to 0, and otherwise the answer is `-KOS_EPERM`. With a record, the caller
must be its owner, and the unlock does not free the word for the waiters to race for. It gives the
word directly to one of them:

```cpp
                // The next holder comes from the record, never from the word.
                Thread* const next = wq_peek_highest(m->waiters);
                KICKOS_ASSERT(next != nullptr); // a record lives only while it has a waiter
                uint32_t handed = next->lock_id;
                if (m->waiters.head != m->waiters.tail)
                {
                    handed = handed | KOS_MUTEX_WAITERS;
                }
                word.store(handed);
            }
            held_remove(c, m);
            Thread* const woken = wq_pop_highest(m->waiters);
            transfer_to(m, woken, 0);
            if (m->waiters.empty())
            {
                held_remove(woken, m);
                record_free(m);
            }
            uint8_t const np = thread_effective_prio(c);
            if (np != c->prio)
            {
                sched::set_prio(c, np, lock);
            }
            sched::wake(woken, lock);
```

`wq_peek_highest` walks the queue and keeps the first thread with the strictly highest `prio`, so
among equals the one that waited longest wins (FIFO). The kernel writes that thread's id into the
word, with the waiters bit while others still wait, through the unlocker's own view of the word.
Then the record moves. The pop unlinks the winner and **clears its wait edge**. `transfer_to` makes
it the record's owner, writes `0` to its `wait_result` (a normal grant) and pushes the record onto
its held list. If it was the last waiter, the record is freed at once: the new holder holds the
word, and the kernel forgets it. The releaser's priority is recomputed over what it still holds.
Last, `sched::wake` moves the new holder to `READY` and lets the scheduler decide whether it runs
now.

Everything the waiter needs is written **by the waker**, while the waiter is still parked. A parked
thread never clears its own edge or writes its own result. On this Cortex-M the switch is deferred:
the kernel pends PendSV and the switch happens once the critical section ends. If the waiter cleared
its own edge after waking, there would be a window where it is still parked but already looks like
it waits on nothing.

That deferred switch is also why the waiter's side has a barrier. When `wq_block` returns inside
`lock_attempt`, the thread may not have switched out yet. `wq_confirm_resume` spins until the
thread's `switch_count` has moved past the epoch, which proves a real switch out and back in
happened. Only then is `wait_result` the waker's value.

**Try it:** on `libcrace`, stop at a hand-over with more than one waiter, so the record outlives it.
`up` shows the unlock's own variables, and `down` comes back before `finish`:

```
(gdb) break kickos::wq_pop_highest if $_any_caller_is("kickos::mutex_unlock", 2) && q.head != q.tail
(gdb) continue
(gdb) up
#1  0x00007422 in kickos::(anonymous namespace)::unlock_attempt (lock=<synthetic pointer>..., c=0x20005438 <...>, va=536943692, ...) at .../kernel/sync/sync.cc:637
(gdb) p $m = m
$1 = (kickos::Mutex *) 0x20004a00 <kickos::detail::g_instance+576>
(gdb) p $c = c
$2 = (kickos::Thread *) 0x20005438 <kickos::detail::g_instance+3192>
(gdb) p/x *$m
$3 = {key = 0x20011c4c, owner = 0x20005438, waiters = {head = 0x200055a8, tail = 0x200051a0}, next_held = 0x0, prev_active = 0x0, next_active = 0x0}
(gdb) p/x *(unsigned*)va
$4 = 0x80000006
(gdb) p/x c->lock_id
$5 = 0x25
(gdb) down
(gdb) finish
Value returned is $6 = (kickos::Thread *) 0x20005590 <kickos::detail::g_instance+3536>
(gdb) p $w = $
$7 = (kickos::Thread *) 0x20005590 <kickos::detail::g_instance+3536>
(gdb) p $w->state
$8 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) p $w->wait_kind
$9 = kickos::WAIT_NONE
(gdb) p/x $w->lock_id
$10 = 0x6
```

The store has happened before the pop: the word already holds `0x80000006`, the next holder's id
with the waiters bit, while the record still names the releaser (`0x25`) as owner. The waiter is
still `BLOCKED`, but its edge is already gone. Now `delete`, `break kickos::sched::wake if t == $w`,
`continue`:

```
(gdb) p $m->owner == $w
$11 = true
(gdb) p $w->wait_result
$12 = 0
(gdb) p $w->held_list == $m
$13 = true
(gdb) p $c->held_list
$14 = (kickos::Mutex *) 0x0
(gdb) p $c->prio
$15 = 10 '\n'
(gdb) finish
(gdb) p $w->state
$16 = {v_ = kickos::ThreadState::READY}
```

The new holder is `READY`. It does not run yet: it has the same priority as the releaser, which
keeps the CPU until its slice ends.

For the waiter's side, start again with
`break kickos::wq_confirm_resume if $_caller_is("kickos::mutex_lock") && $_streq(c->name, "racer")`,
`continue`:

```
Breakpoint 1.1, kickos::wq_confirm_resume (c=0x20005590 <kickos::detail::g_instance+3536>, epoch=2) at .../kernel/sync/sync.cc:70
(gdb) p c->switch_count
$1 = {v_ = {... _M_i = 3}, ...}
(gdb) delete
(gdb) finish
(gdb) p c->wait_result
$2 = 0
(gdb) p/x c->lock_id
$3 = 0x6
(gdb) p/x *(unsigned*)va
$4 = 0x80000006
```

The epoch was 2 and the count is 3: the switch happened, so the read is safe. The word holds this
thread's own id, with the waiters bit because other workers still wait.

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
[Book: Context switching](../book/context-switching-and-the-silicon-contract.md)

### Stop 11: priority inheritance, the boost
**Where:** `kernel/sync/sync.cc`, `lock_attempt` (pass 2); `kernel/sched/sched.cc`,
`sched::set_prio`; `user/apps/common/selftest/main.cc`, `t_mutex_pi`, `pi_low`, `pi_high`, `pi_med`

Now switch to the `selftest` image. In `t_mutex_pi`, `piLo` (priority 8) locks the mutex, then
wakes `piHi` (20), which preempts it. `piHi` wakes `piMd` (12) and then locks the same mutex.
Without inheritance, `piMd` would now run ahead of `piLo` and keep `piHi` waiting for as long as it
liked: that is **priority inversion**. Pass 2 prevents it:

```cpp
            {
                Thread* o = t;
                int depth = 0;
                while (o != nullptr)
                {
                    if (o->prio >= c->prio)
                    {
                        break;
                    }
                    sched::set_prio(o, c->prio, lock);
                    if (o->wait_mutex() == nullptr)
                    {
                        break;
                    }
                    o = o->wait_mutex()->owner;
                    // ...
                }
            }
```

`t` is the holder: the record's owner, or the live thread `owner_resolve` found in the word.
`sched::set_prio` is the only function that writes an effective priority. On this single-core
build, its job is to keep the ready list consistent: a ready list is filed by priority, so a
`READY` or `RUNNING` thread is taken off, changed, and put back. A `BLOCKED` thread is on no ready
list and its wait queue is not sorted, so the value is simply written.

```cpp
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
                policy_on_remove(t);
                t->prio = p;
                policy_on_ready(t);
                return;
            }
            // A BLOCKED thread is on no ready list, and neither queue it can be on is
            // prio-ordered: wq_pop_highest rescans at pop, the timer list is deadline-sorted.
            t->prio = p;
```

`base_prio` is never touched. The boost is a loan, recorded only in `prio`, and it goes to whatever
owner the record names, which on this board can be another task's thread that a forged word named
(Stop 7).

**Try it:** on `selftest`. The first boost of the run is this one:

```
(gdb) break kickos::sched::set_prio if $_caller_is("kickos::(anonymous namespace)::lock_attempt")
(gdb) continue
Breakpoint 1, kickos::sched::set_prio (t=t@entry=0x20005188 <...+2504>, p=20 '\024', held=held@entry=...) at .../kernel/sched/sched.cc:910
(gdb) p t->name
$1 = "piLo", '\000' <repeats 11 times>
(gdb) p t->prio
$2 = 8 '\b'
(gdb) p t->base_prio
$3 = 8 '\b'
(gdb) p t->state
$4 = {v_ = kickos::ThreadState::READY}
(gdb) p/x t->lock_id
$5 = 0x303
(gdb) p $t = t
$6 = (kickos::Thread *) 0x20005188 <kickos::detail::g_instance+2504>
(gdb) up
(gdb) p c->name
$7 = "piHi", '\000' <repeats 11 times>
(gdb) p/x *(unsigned*)va
$8 = 0x80000303
(gdb) p $t->held_list->key == va
$9 = true
(gdb) p $t->held_list->owner == $t
$10 = true
```

`piLo` is `READY`, not running: `piHi` preempted it. The word names `piLo`, with the waiters bit
`piHi` has just set, and `piLo`'s held list starts with the record keyed on it. After this call
`piLo` is ready at 20, so when `piHi` parks, `piLo` runs next, ahead of `piMd`.

**Why, and the contract:** [Book: Priority inheritance](../book/priority-inheritance-lending-urgency.md),
[Reference: Scheduler](../reference/architecture.md#scheduler-the-core-constraint)

### Stop 12: the chain
**Where:** `kernel/sync/sync.cc`, `lock_attempt` (passes 1 and 2); `kernel/include/kickos/thread.h`,
`Thread::wait_mutex`; `user/apps/common/selftest/main.cc`, `t_mutex_chain`, `ch_a`, `ch_b`, `ch_c`

A boost must follow the chain of holders. In `t_mutex_chain`, `chC` (5) holds M2. `chB` (10) holds
M1 and then blocks on M2, which boosts `chC` to 10. Then `chA` (20) blocks on M1. Pass 2 starts at
M1's holder, `chB`: it is below 20, so it is boosted. It is itself parked on M2, so `wait_mutex()`
returns M2's record and the walk moves to that record's owner, `chC`, and boosts it too. Without
that second hop, `chD` (15) would run ahead of `chC`, and `chA` would wait behind `chD`.

The walk ends when a holder is not parked on a mutex (`wait_mutex()` returns null), or when a
holder is already at the caller's priority or above. That early stop is safe: every holder further
along was boosted at least that high when its own waiter blocked. A thread parked on a
**semaphore** answers null too, so inheritance stops there: a semaphore has no owner to boost.
Pass 1 follows the same edges first, read-only. If they lead back to the caller, the lock would
deadlock. It is refused before pass 2 has boosted anyone, so a refused lock leaves no boost behind.
A depth bound of `KICKOS_THREAD_SLOTS` backs both walks up. An honest chain visits each thread
once, and a cycle that does not pass through the caller, which honest use never builds, is cut
there instead of walked forever.

**Try it:** on `selftest`, stop only at a boost of a thread that is itself waiting on a mutex:

```
break kickos::sched::set_prio if $_caller_is("kickos::(anonymous namespace)::lock_attempt") && t->wait_kind == kickos::WAIT_MUTEX
```

```
Breakpoint 1, kickos::sched::set_prio (t=t@entry=0x200052e0 <...+2848>, p=20 '\024', held=held@entry=...) at .../kernel/sched/sched.cc:910
(gdb) p t->name
$1 = "chB", '\000' <repeats 12 times>
(gdb) p t->state
$2 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) p ((kickos::Mutex*)t->wait_obj)->owner->name
$3 = "chC", '\000' <repeats 12 times>
(gdb) p ((kickos::Mutex*)t->wait_obj)->owner->prio
$4 = 10 '\n'
(gdb) up
(gdb) p c->name
$5 = "chA", '\000' <repeats 12 times>
```

To see every loan and every return in the whole run, log both directions:

```
break kickos::sched::set_prio if $_caller_is("kickos::(anonymous namespace)::lock_attempt")
commands
silent
printf "boost %s: %d -> %d (base %d)\n", t->name, t->prio, p, t->base_prio
continue
end
break kickos::sched::set_prio if $_caller_is("kickos::(anonymous namespace)::unlock_attempt")
commands
silent
printf "drop  %s: %d -> %d (base %d)\n", t->name, t->prio, p, t->base_prio
continue
end
continue
```

The first lines it prints:

```
boost piLo: 8 -> 20 (base 8)
drop  piLo: 20 -> 8 (base 8)
boost chC: 5 -> 10 (base 5)
boost chB: 10 -> 20 (base 10)
boost chC: 10 -> 20 (base 5)
drop  chC: 20 -> 5 (base 5)
drop  chB: 20 -> 10 (base 10)
boost odOwn: 8 -> 12 (base 8)
boost mhB: 6 -> 20 (base 6)
drop  mhB: 20 -> 6 (base 6)
```

Read the chain lines: one hop when `chB` blocks, two hops when `chA` blocks, and each thread handed
back down to its own base when it unlocks. `odOwn` gets no drop line: it dies holding its mutex
(Stop 14). Run the same breakpoints on `libcrace` and only a few of `main`'s witnesses show up
(`mholder`, `mrunner`), never a `racer`: equal priorities never boost.

**Why, and the contract:** [Book: Priority inheritance, "Boosting: the lock path"](../book/priority-inheritance-lending-urgency.md)

### Stop 13: the drop on unlock
**Where:** `kernel/sync/sync.cc`, `unlock_attempt`, `thread_effective_prio`;
`user/apps/common/selftest/main.cc`, `t_mutex_multi_held`

On unlock the releaser's priority is not "reset to base". It is recomputed from what is still
lending to it:

```cpp
        uint8_t p = t->base_prio;
        for (Mutex* h = t->held_list; h != nullptr; h = h->next_held)
        {
            Thread const* const w = wq_peek_highest(h->waiters);
            if (w != nullptr and w->prio > p)
            {
                p = w->prio;
            }
        }
```

The held list holds only records, so only words that someone waits on, and those are exactly the
ones that lend. `thread_effective_prio` goes on to count two more kinds of donor: callers waiting
for this thread's reply, and callers queued on an endpoint it serves. Those are
[call and reply](05-call-and-reply.md)'s, and one function computes all of them, so a mutex unlock
in the middle of serving a call never throws away the caller's loan.

The recompute is what makes holding two mutexes correct. In `t_mutex_multi_held`, `mhB` (6) holds
M1 and M2, and `mhH` (20) waits on M1. When `mhB` releases M2, M1 still has a waiter at 20, so the
recompute gives 20 again and `set_prio` is not called at all. That is why the log above shows one
`boost mhB` and one `drop mhB`, the drop coming only at the unlock of M1. A reset to base at the
first unlock would let `mhD` (12) in.

The drop is followed by `sched::wake` of the new holder, whose scheduling pass takes over. An
unlock with no waiter has nothing to drop: when the last waiter left without the lock, its
departure already took the loan back (Stop 14).

**Try it:** on `selftest`:

```
(gdb) break kickos::sched::set_prio if $_caller_is("kickos::(anonymous namespace)::unlock_attempt")
(gdb) continue
(gdb) p t->name
$1 = "piLo", '\000' <repeats 11 times>
(gdb) p t->prio
$2 = 20 '\024'
(gdb) p p
$3 = 8 '\b'
(gdb) p t->state
$4 = {v_ = kickos::ThreadState::RUNNING}
(gdb) p $lo = t
$5 = (kickos::Thread *) 0x20005188 <kickos::detail::g_instance+2504>
(gdb) delete
(gdb) break kickos::wq_confirm_resume if $_caller_is("kickos::mutex_lock")
(gdb) continue
Breakpoint 2.1, kickos::wq_confirm_resume (c=0x200052e0 <...+2848>, epoch=2) at .../kernel/sync/sync.cc:70
(gdb) p c->name
$6 = "piHi", '\000' <repeats 11 times>
(gdb) p c->prio
$7 = 20 '\024'
(gdb) p c->wait_result
$8 = 0
(gdb) p $lo->prio
$9 = 8 '\b'
(gdb) p $lo->state
$10 = {v_ = kickos::ThreadState::READY}
```

The releaser is lowering itself. `piHi` got the mutex and the CPU straight away. `piLo` is back at
8 and waits behind `piMd`.

**Why, and the contract:** [Book: Priority inheritance, "Reverting: the unlock path"](../book/priority-inheritance-lending-urgency.md)

### Stop 14: when a waiter or a holder goes away
**Where:** `kernel/thread/park.cc`, `thread_abort_park` (the `WAIT_MUTEX` case);
`kernel/sync/sync.cc`, `mutex_waiter_left`, `mutex_owner_exit`, `mutex_reach_lost`;
`kernel/sched/sched.cc`, `exit_current`; `user/apps/common/selftest/main.cc`, `t_mutex_deadlock`,
`t_mutex_owner_died`

Two exits from the normal story.

A **waiter that leaves** without the lock, because its time ran out or its thread or task is being
killed, is not handed the word. `thread_abort_park` undoes its park:

```cpp
                Mutex* const m = t->wait_mutex();
                KICKOS_ASSERT(m != nullptr); // WAIT_MUTEX never parks without one
                t->wait_queue->unlink(&t->link);
                Thread* owner = m->owner;
                mutex_waiter_left(m);
                for (int depth = 0; owner != nullptr and depth <= KICKOS_THREAD_SLOTS; depth++)
                {
                    uint8_t const p = thread_effective_prio(owner);
                    if (p == owner->prio)
                    {
                        break;
                    }
                    sched::set_prio(owner, p, held);
                    Mutex const* const next = owner->wait_mutex();
                    if (next == nullptr)
                    {
                        break;
                    }
                    owner = next->owner;
                }
```

It unlinks the waiter first, so the recompute no longer counts it. `mutex_waiter_left` frees the
record if that was its last waiter. Then the holder's priority is recomputed, and the holder's own
holder after it, down the chain for as long as a priority changes. The word is never touched: a
waiters bit left set costs the holder one unlock entry, which finds no record and clears the word.
On a board with address spaces, a waiter whose word leaves its reach (for instance a window
unmapped or a stack freed) is ended the same way, with `-KOS_EFAULT` (`mutex_reach_lost`). So a parked waiter can always
write its word, which the next case relies on.

A **holder that dies** with waiters must not strand them. `exit_current` calls
`mutex_owner_exit` in the locked section that marks the thread `dying`, after the thread's windows
and stack are unmapped, so only words that stay are handed over:

```cpp
        while (dying->held_list != nullptr)
        {
            Mutex* const m = dying->held_list;
            held_remove(dying, m);
            Thread* const next = wq_pop_highest(m->waiters);
            KICKOS_ASSERT(next != nullptr); // a record lives only while it has a waiter
            uint32_t handed = next->lock_id;
            if (not m->waiters.empty())
            {
                handed = handed | KOS_MUTEX_WAITERS;
            }
            // A waiter can always write its word: every call removing that reach ended the
            // waits through it first, so the store cannot fail.
            {
                WordAccess word(word_ref_parked(next, m->key));
                KICKOS_ASSERT(word.reached());
                word.store(handed);
            }
            transfer_to(m, next, MUTEX_OWNER_DIED);
            if (m->waiters.empty())
            {
                held_remove(next, m);
                record_free(m);
            }
            sched::wake(next, held);
        }
```

It is the hand-over of Stop 10 with two differences. The word is written through the waiter's own
view of it (`word_ref_parked`: the waiter's address space and `wait_va`), since the dying thread's
memory may be leaving. The waiter's result is `MUTEX_OWNER_DIED` (`-KOS_EOWNERDEAD`), so it knows the
protected state may be inconsistent. The dying thread's priority is not recomputed: it keeps its
boost for the rest of its teardown. A word the dying thread holds with no waiter is not on its held
list at all, and keeps its id for the next taker to find dead (Stop 8).
[Fault and kill, Stop 7](09-fault-and-kill.md) shows the dying side of this hand-over.

**Try it:** a waiter that times out. In `t_mutex_deadlock`, `main` tries a mutex that `cycA` (10)
holds, with a 2 ms timeout, and the timer ends the wait. On `selftest`:

```
(gdb) break kickos::thread_abort_park if t->wait_kind == kickos::WAIT_MUTEX
(gdb) continue
Breakpoint 1, kickos::thread_abort_park (t=0x20005030 <kickos::detail::g_instance+2160>, result=-110, held=...) at .../kernel/thread/park.cc:25
(gdb) p t->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p result
$2 = -110
(gdb) p $m = (kickos::Mutex*)t->wait_obj
$3 = (kickos::Mutex *) 0x20004a00 <kickos::detail::g_instance+576>
(gdb) p/x *$m
$4 = {key = 0x20010d80, owner = 0x20005188, waiters = {head = 0x20005048, tail = 0x20005048}, next_held = 0x0, prev_active = 0x0, next_active = 0x0}
(gdb) p $o = $m->owner
$5 = (kickos::Thread *) 0x20005188 <kickos::detail::g_instance+2504>
(gdb) p $o->name
$6 = "cycA", '\000' <repeats 11 times>
(gdb) p $o->prio
$7 = 10 '\n'
(gdb) p $o->base_prio
$8 = 10 '\n'
(gdb) p $va = t->wait_va
$9 = 536939904
(gdb) p/x *(unsigned*)$va
$10 = 0x800003c3
(gdb) finish
(gdb) p $o->prio
$11 = 10 '\n'
(gdb) p/x *$m
$12 = {key = 0x0, owner = 0x0, waiters = {head = 0x0, tail = 0x0}, next_held = 0x0, prev_active = 0x0, next_active = 0x0}
(gdb) p $k->mutex_active
$13 = (kickos::Mutex *) 0x0
(gdb) p/x *(unsigned*)$va
$14 = 0x800003c3
(gdb) delete
(gdb) break kickos::mutex_unlock if va == $va
(gdb) continue
(gdb) p $k->currents.cells[0]->name
$15 = "cycA", '\000' <repeats 11 times>
(gdb) p/x *(unsigned*)va
$16 = 0x800003c3
(gdb) finish
Value returned is $17 = 0
(gdb) p/x *(unsigned*)$va
$18 = 0x0
```

`-110` is `-KOS_ETIMEDOUT`. `main` was the record's only waiter, so the record is free afterwards and
no record is active. `cycA` was at its base priority, so there was no loan to take back, and it
stays at 10. The word still carries the waiters bit, so `cycA`'s unlock enters the kernel, finds no
record and clears the word to 0.

The waiter's side of a holder's death: in `t_mutex_owner_died`, `odOwn` exits while it holds a
mutex that `odWt` waits on. On `selftest`,
`break kickos::wq_confirm_resume if $_caller_is("kickos::mutex_lock") && $_streq(c->name, "odWt")`,
`continue`, `delete`, `finish`:

```
(gdb) p c->wait_result
$1 = -130
(gdb) p/x c->lock_id
$2 = 0x2c4
(gdb) p/x *(unsigned*)va
$3 = 0x2c4
(gdb) p c->held_list
$4 = (kickos::Mutex *) 0x0
```

`-130` is `-KOS_EOWNERDEAD`: `odWt` holds the mutex, and the word holds its id, written by the
kernel during `odOwn`'s exit. `odWt` was the only waiter, so the record was freed at the hand-over
and `odWt`'s held list is empty.

**Why, and the contract:** [Book: Priority inheritance, "Owner-died"](../book/priority-inheritance-lending-urgency.md),
[Reference: invariants](../reference/invariants.md) (`mutex-owner-death-reaches-the-next-taker`,
`task-scoped-death-and-a-cancel-that-reaches-every-park`)

### Stop 15: the semaphore, the same substrate without an owner
**Where:** `kernel/include/kickos/sync.h`, `struct Semaphore`; `kernel/sync/sync.cc`, `sem_wait`,
`sem_post`; `kernel/syscall/syscall.cc`, the `KOS_SYS_SEM_WAIT` and `KOS_SYS_SEM_POST` cases

A semaphore is a count and a wait queue: `int count` and `List waiters`. `sem_wait` takes a token
if `count > 0`, returns at once if the caller passed a zero timeout, and otherwise arms a deadline
(`ktime_deadline_arm`) and parks with the same `wq_block`, tagged `WAIT_SEM`. `sem_post` is the
same pop-transfer-wake as the mutex hand-over, with a token in place of the word:

```cpp
    bool sem_post(Semaphore* s)
    {
        IrqLock lock;
        Thread* w = wq_pop_highest(s->waiters);
        if (w != nullptr)
        {
            w->wait_result = 0; // the token goes straight to the waiter, count stays put
            sched::wake(w, lock);
            return true;
        }
        if (s->count >= KOS_SEM_COUNT_MAX)
        {
            return false; // refuse rather than wrap a signed int (UB)
        }
        s->count++;
        return true;
    }
```

Compare it with Stop 10, line by line. The differences are why the mutex is a primitive of its
own:

- A semaphore is a kernel object, made by `kos_sem_create` and named by a capability. A mutex is
  a word in user memory, and the kernel keeps state for it only while a thread waits.
- A semaphore has **no owner**. Any thread holding the `SIGNAL` right can post, and `sem_post`
  takes its own `IrqLock`, so it may also run in interrupt context. A mutex unlock must come from
  the holder.
- With no owner there is nobody to lend priority to. `wait_mutex()` answers null for a `WAIT_SEM`
  park, so the chain walk of Stop 12 stops there.
- With no waiter, a post is **banked** in `count` for a later wait. A mutex has nothing to bank.
- A semaphore's rights are split (`CAP_WAIT`, `CAP_SIGNAL`), checked at resolve. A mutex has no
  rights: being able to write the word is what lets a thread use it.

**Try it:** on `libcrace`, `break kickos::sem_post`, `continue`. The first post is the malloc holder
of the first witness telling `main` it now holds the malloc lock. `main` is parked in
`kos_sem_wait` on that semaphore, which reaches the holder as its first delegated capability:

```
Breakpoint 1, kickos::sem_post (s=0x20004904 <kickos::detail::g_instance+324>) at .../kernel/sync/sync.cc:137
(gdb) bt 2
#0  kickos::sem_post (s=0x20004904 <kickos::detail::g_instance+324>) at .../kernel/sync/sync.cc:137
#1  (anonymous namespace)::syscall_body (nr=6, a0=1, a1=<optimized out>, a2=<optimized out>, a3=0) at .../kernel/syscall/syscall.cc:521
(gdb) p *s
$1 = {count = 0, waiters = {head = 0x20005048 <kickos::detail::g_instance+2184>, tail = 0x20005048 <kickos::detail::g_instance+2184>}}
(gdb) p $k->currents.cells[0]->name
$2 = "mholder\000\000\000\000\000\000\000\000"
```

One waiter: `0x20005048` is the `link` inside `main`'s `Thread`, which starts at `+2160` (slot 1,
the thread whose id is 2).

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
[Reference: Object model, capabilities & IPC](../reference/architecture.md#object-model-capabilities--ipc)
("Synchronization surface")

## Where to next

- [5. Call and reply](05-call-and-reply.md): the other two kinds of priority donor, and the
  endpoint wait queues.
- [11. Multicore](11-multicore.md): `sched::set_prio` when the holder's ready list belongs to
  another core (the `RESEAT` request), and what the kernel lock adds to the `IrqLock`.
- [9. Fault and kill](09-fault-and-kill.md): the exit that hands a dying holder's words to their
  waiters.
- [Book: Priority inheritance](../book/priority-inheritance-lending-urgency.md) for the full
  argument, and [Book: One kernel lock, many ready queues](../book/one-lock-many-ready-queues.md).

## Exercises

1. **Self-deadlock.** In `selftest`, `t_mutex_deadlock` locks a mutex it already holds, more than
   once and in more than one way. Predict where each refusal is decided, in user code or in the
   kernel, and what it returns. Then catch the first one in user code, where the word already
   names the caller:

   ```
   break kos_mutex_lock if (m->word & 0x7fffffff) == s_self
   continue
   bt 2
   finish
   ```

   Then catch the one the kernel answers, on the same word:

   ```
   delete
   break kickos::mutex_lock if va == (unsigned)&'(anonymous namespace)::g_dl_self'
   continue
   finish
   ```

   Check your prediction against the values `finish` prints and against `KOS_EDEADLK` in
   `system/include/kickos/sys/errno.h`. Then read the test to see how its second refusal reaches
   the kernel at all, when `kos_mutex_lock` would have answered it in user code.
2. **Who wins among equals.** In `libcrace`, at a hand-over (Stop 10), save `$h = q.head` in
   `wq_pop_highest` before `finish`. Predict whether `&$w->link == $h`. What would change if
   `wq_peek_highest` used `>=` instead of `>`?
3. **Break the chain.** In `t_mutex_chain`, imagine `chB` waits on a semaphore instead of on M2.
   Predict the log of Stop 12, then reason about what `chD` (15) would do to `chA`.
