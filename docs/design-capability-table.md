<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# Design -- the capability table, from a clean sheet

> **Status: LANDED** -- landed on `master`. Every section below has landed except where the text
> says otherwise.
> Witnessed at `da716a8` (2026-08-04, `docs/reference/boards.md`'s capture-provenance table):
> two captures, `m472-{xmc,k64}-st`, both `1..79`. Two boards, one ISA, one geometry.

The capability table landed in stages during M4.5/M4.6 and works. This document re-derives it from
scratch, because the mechanism that grew around it costs more than it buys: a three-class slab of
which no board declares the third class, a per-spawn interface whose only non-zero caller is the
test of itself, a codec welded to a provisioning constant, and an exhaustion errno that cannot say
which pool ran out.

It also answers a question the existing record never states plainly -- **what a capability
actually is here** -- and it fixes the direction of travel: the fleet will span `microbit`'s
16 KiB to an i.MX8MP's 4 to 8 GB, so every number in this subsystem has to survive a factor of
5 x 10^5.

## 1. What a capability is here

Four concerns, deliberately not in the same place.

`CapEntry` (`kernel/include/kickos/cap.h`), 8 bytes:

- `obj` -- **the reference**: a global generational object handle into an object pool.
- `type` -- **which pool** `obj` indexes.
- `rights` -- **what the holder may do** (`CAP_WAIT`, `CAP_SIGNAL`, `CAP_TRANSFER`).
- `gen` -- **the per-slot epoch**, so a stale handle cannot resolve.

The fourth concern is `Thread::authority`, and it is **not in the table** because it names no
object. It is six bits, not four: `AUTH_MEMORY`, `AUTH_PINMUX`, `AUTH_PSTATE`, `AUTH_IRQ`,
`AUTH_SYSTEM`, `AUTH_CONSOLE` (`kernel/include/kickos/cap.h`, `CapAuthority`). That split is the one
non-obvious thing in the current design and it is correct: permission over a specific object is a
table entry, permission to do something object-less is a word on the TCB. `AUTH_CONSOLE` is what
carries the console handover discussed below, and it is its own bit precisely because the thread
that publishes the console and the thread that ends the system are different threads.

**What userspace holds is not the entry.** It is a word of `(gen << index_bits) | index`, a
task-relative name carrying **no rights at all**. Rights cannot be read out of it, forged into it,
or widened.

`cap_resolve` is **two-level**, and O(1) at both levels (`kernel/syscall/cap.cc`, `cap_lookup` then
`cap_resolve_e`): `cap_lookup` masks the index, bound-tests it against **this task's** capacity,
compares the per-slot cap generation and tests `(rights & need) == need`; then the object pool's own
`resolve` repeats index mask, bound test and generation compare against the object epoch. Two guards,
not one, because capability liveness and object liveness are different facts.

The property that follows, and the answer to "is a capability a right or a token":

> The handle is a pure token. Rights are an attribute of the **(task, slot)** pair -- not of the
> object, and not of the handle.

So two tasks may hold one object with different rights, and one task may hold one object twice at
two indices with different rights. The console publish path depends on the second: the publisher
holds a console endpoint with full rights while index 0 is a send-only `CAP_SIGNAL` copy of that
same endpoint.

`CAP_REPLY` is the one entry that breaks the "`obj` names a pool slot" rule: `obj` holds a
generational THREAD handle instead, whole and unshifted, so it takes all 32 bits and is routinely
negative. The 8-bit call sequence that rides with it lives in the spare bits beside the type and
the rights (`KCAP_REPLY_SEQ_LO_BITS` / `KCAP_REPLY_SEQ_HI_BITS`, seated by `cap_reply_seq_seat`;
`cap_install_reply`, `kernel/syscall/cap.cc`). Any re-cut of the entry has to re-cut
that packing too; section 5 is where that was worked out.

## 2. Why capabilities, and not an access-control list

The two are transposes of one access matrix: an ACL stores it per object (who may), a capability
system stores it per subject (what it holds). The storage choice is mechanical. The semantic choice
is not: **an ACL derives authority from identity, checked at access time; a capability derives it
from possession.**

**The decisive argument is the IPC path.** In a microkernel, IPC is the hot path and everything
else is built on it. Possession is an O(1) test on a reference the caller already holds. An ACL
check is a lookup keyed on the caller's identity, on every send. That cost is not affordable where
it falls.

Three further properties follow from possession and are load-bearing here:

- **No ambient authority.** Under an ACL every subject can *name* every object and the check is
  the only thing between them. Here a task cannot name what it was not given, which is what makes
  `spawn_caps` -- an enumerated list of objects with per-cap rights -- an auditable statement of a
  child's entire authority at the spawn site.
- **Two rights on one object**, which the console seat needs (section 1).
- **A reference that names no pool object**, which `CAP_REPLY` needs.

An ACL's one genuine advantage is revocation: removing an entry revokes instantly and everywhere,
because the check happens at access time. Capability systems pay for revocation instead --
seL4 spends 16 of every 32-byte slot on a derivation tree, present whether or not a capability is
ever derived. This kernel has no revocation and does not want it, so the advantage is not one it
would collect.

### The transpose was priced and rejected

Storing rights in the object indexed by task (as Zephyr does, one bit per object per thread) was
evaluated because it removes the per-task table, and with it any per-task limit. Priced on one
consistent basis -- the fleet default, `KICKOS_MAX_THREADS + 2` = 18 runs, and the four object pools
at their `kernel/include/kickos/config/system.h` defaults (16 semaphores + 8 mutexes + 4 endpoints +
8 IRQ bindings = 36 objects):

| model | `.bss` |
| --- | --- |
| current single run size (18 runs x 10 slots x 8 B) | 1440 B |
| transpose, rights nibble per (object, task) (36 x 18 x 0.5 B) | 324 B |
| capability rows sized to real demand (18 x 6 slots x 8 B) | 864 B |

**The apparent 4.4x saving is mostly padding, not model.** The current row is padded to the full
width at 8 bytes per slot whether used or not, while measured demand is 1 to 6 capabilities
(`TODO.md`: a polled console driver holds 1, an IRQ-driven UART or SPI service 2, and the selftest's
deadlock case holds 6 while spawning children at 5 grants). A row sized to that halves the gap, and
what remains is one typed, generational, rights-bearing entry against a nibble that carries none of
the three. Size does not decide this.

What the transpose costs is three real properties: the capability model itself (a global object
namespace means any task can name any object and rely on refusal), two rights on one object, and a
home for `CAP_REPLY`. It also keys authority on **thread index**, and an index outliving its
occupant is a hazard **this tree has already met**: `ThreadPool::alloc`
(`kernel/include/kickos/thread.h`) clears `spawner_tag` on every reuse precisely because "a slot's
kill tag is its INDEX and so outlives its occupant", and it does so at the reclaim point rather
than at exit because reuse is the only event that makes the tag ambiguous. That is one bit of
index-keyed authority needing a fix-up loop; a transpose multiplies it by the object count.
CVE-2026-10681 corroborates rather than carries the argument: in Zephyr 2.0.0 through 4.4.1,
patched in 4.5.0, a race in `thread_idx_alloc()` aliased one permission bit across two threads.

Rejected on those grounds, not on footprint.

## 3. What is deleted

The deletions are **strictly ordered 1 -> 2 -> 3**: each removes the ground the next stands on, and
none of them is vacuous given only its predecessor.

1. **The size-class mix** -- the multiclass switch, the three slot/count knob pairs, the class
   table, the class-selection loop, and the smallest/largest-class helpers. **No board declares
   class 2.** One uniform run size replaces them.

   Ten `static_assert`s in `kernel/include/kickos/cap.h` reference the class machinery. **Five
   vanish** -- class 0 is required, the multiclass variable agrees with the class table, class 2
   requires class 1, and the two strictly-ascending checks. **Five are restated** against the single
   uniform size -- no class exceeds the addressable ceiling, the smallest class holds the reserved
   plane plus one dynamic slot, the largest class equals the ceiling root asks for at boot, the
   default spawn capacity fits the largest class, and `KCAP_TEARDOWN_CHUNK` stays strictly below the
   run size.
2. **Per-spawn declared capacity**, the ABI field and the parameter that feeds it. With one class
   every request rounds to that class, so the field can only ever change behaviour by *failing*, and
   its only non-zero caller in the tree is the selftest arm that tests it. Section 7 removes the
   remaining reason to keep it: the chunk count comes from the configure-time sum, so no per-spawn
   quantity is left to declare.
3. **The narrow-only clamp against the spawner.** It is *not* the identity while the ABI field
   survives -- the selftest arm relies on `0xFFFF` being clamped to the parent's capacity so it can
   ask for root's ceiling without knowing it, which is a behaviour, not a no-op. Nor is every task's
   capacity equal: idle is capacity 0 **by construction** (`idle_attr.cap_run = CapRun{}` in
   `kernel/init/kmain.cc`), which is what makes "idle holds no capability" structural. The clamp is
   the identity only for threads that can issue a spawn, and once the field of (2) is gone it has
   nothing left to clamp. It conserved
   nothing in any case -- narrow-only is a ceiling, not a budget, so a parent of capacity N could
   spawn any number of N-children, and the run count was the only real wall.
4. **`Thread::cap_class`.** Its only reader is the slab detach path; with one class it is a
   constant.
5. **The per-board default spawn capacity knob.** It exists only to name a board default for the
   field deleted in (2), and when this was written the only override in the tree came from the
   `mps2` chip, which serves all four `qemu*` boards (`boards/Kconfig`), so it was one declaration
   and not one board.
6. **`KICKOS_MAX_HANDLES` as a board knob.** The per-task width becomes a configure-time sum of
   four declarations (section 6). The codec stops deriving `KCAP_INDEX_BITS` from it, which is what
   couples the handle bit-layout to a per-board RAM decision (section 5).

The mix silently cut the sim from 16 usable thread slots to 7: every spawn took the default
capacity, only the wider class fitted it, and refuse-never-spill left the narrower class
unreachable, so the wider class's count of 8 minus root's one run was the whole concurrency wall.
Any claim that the mix was behaviour-identical, or that it moved no board's refusal points, is
false for that board; section 10 carries the sweep.

## 4. What survives, and why

- **O(1) `cap_resolve`.** Every object-naming syscall funnels through it. This is what kills any
  chained or walked representation, and what constrains the segmented layout of section 7.
- **A contiguous per-task run with a task-relative index.** Required by the reserved index plane,
  and it rests on **one** seated index, not two. `KOS_CAP_STDOUT` is 0 in every task and is genuinely
  seated -- `cap_install_defaults` and `cap_seat_stdout` write it, and
  `system/init/common/default_init_run.cc`, `user/include/kickos/sys/driver_service.h` and
  `user/apps/common/drvdeath/main.cc` read it. `KOS_CAP_CLOCK` at index 1 has no reader that names
  it by that constant, but it is written: `KOS_SPAWN_DELEGATED_CAP0` is 1 and `cap_install_at`
  rejects only index 0, so every DEFAULTED spawn grant seats index 1 -- which is the aliasing
  recorded in section 11 (`system/include/kickos/sys/cap_index.h`,
  `user/include/kickos/sys/abi.h`). One seated index is enough for the layout argument: a
  global index space cannot give every task its own index 0.
- **No fragmentation.** One size class gives this for free; it does not need fixed classes to
  achieve it. The slab-level "refuse, never spill" becomes vacuous rather than load-bearing, which
  is the point -- and it is a different refusal from `cap_install`'s, which stays load-bearing
  (section 7).
- **The bounded interrupt-masked window in `cap_teardown`**, chunked behind `Thread::dying`.
  There is exactly **one** such window, not two: the effective-priority funnel does not read the
  capability table at all (`kernel/sync/sync.cc`), and `kernel/include/kickos/thread.h` forbids
  adding a table walk there.
- **The per-slot generation**, at the full 16 bits of its `uint16_t` (section 5). It is the rarest
  feature in the prior-art set (section 9) and the best justified one here, because slot reuse is
  routine rather than exceptional: a healthy `sim_stress` round runs **2040** spawn/exit churn cycles
  (`STATE.md`), and every one of them tears a table down and hands its run to the next occupant. The
  alternatives each cost more -- an O(object-count) scrub per task creation, eager global revocation
  machinery, or a cooperative release protocol with a documented residual race.
- **An 8-byte `CapEntry`** -- but the alignment has to be **added**, not merely kept. The fields are
  `int32_t; uint8_t; uint8_t; uint16_t`, so `sizeof` is 8 and `alignof` is **4**. Eight bytes is
  worth keeping because a dead entry is wide enough to hold the free-list link on a 64-bit target
  (section 7) and because two entries pack into one 64-bit word. Whether `cap_resolve` can ever be
  lock-free is an M6 question and it does not turn on the entry width: it turns on object
  reclamation (section 8). What lands here is `alignas(8)` on the struct plus
  `static_assert(alignof(CapEntry) == 8)`, so the property is stated where it can be broken.

One item previously treated as load-bearing is **not**: returning the run at slot reclaim rather
than at thread exit is inherited from the thread stack, which genuinely needs it (the dying thread
is still executing on that stack). The table has no such property -- `cap_teardown` provably
finishes with it, and nothing between there and the park loop reads the run. Detaching at the end
of `cap_teardown` would remove the allocated-versus-live gap that the counts must currently cover.
**Stated as a claim to prove by mutation, not as a fact**: if a reader were missed, the failure is
a live table aliased across tasks, which is an isolation bug rather than a leak.

## 5. The codec

**The handle leaves the errno-carrying return value.** Today it is packed into a signed return whose
negative values are error codes, which is what caps the word at 31 bits. Return a status and write
the handle to an out-parameter -- `kos_sem_create(&h)` rather than `h = kos_sem_create()` -- and the
budget is a full 32 bits. That is an ABI change across every capability-minting call, and the ABI is
unstable exactly until the ABI-freeze milestone, so it is as cheap now as it will
ever be. It is also what makes the rest
of this section a free choice rather than a trade.

**The split is chosen once, fleet-wide, and never tuned to a board.** It costs nothing either way:
`cap_resolve` is a shift and a mask by compile-time constants. The only quantity traded is slots
against generation margin, so there is no efficiency argument for a narrow index and no reason for
the value to track anyone's RAM.

**The kernel does not get to decide the application's working set.** It is the ceiling, not the
final product, so "nobody needs that many" is not an argument available to it -- and an argument
from today's measured demand of 1 to 6 capabilities is that argument wearing a measurement. The
rule instead:

> Pick the largest ceiling that costs nothing at the small end, and where a ceiling would have to
> be traded against a guarantee, remove the trade rather than choose a side.

**Codec width costs no RAM**; only the configured width does. A 16-bit index does not make
`microbit`'s table larger, it makes the encoding stop being the constraint. The requirement driving
16 index bits is explicit, not extrapolated: **a task holding 40000 to 60000 capabilities
simultaneously.** `2^16 - 1` = 65535 covers it; `2^15 - 1` = 32767 does not.

So **`index_bits = 16` and `gen_bits = 16`**, with 32 bits available and nothing traded:

| | value |
| --- | --- |
| index | 16 bits, so at most 65535 slots |
| generation | 16 bits, 65536 reuses before a stale handle can alias |
| largest mintable handle | `(0xFFFF << 16) \| 0xFFFE` = `0xFFFFFFFE` |

**16 generation bits is what removes the mask, not merely what widens the margin.** `CapEntry::gen`
is a `uint16_t` and the counter is incremented bare at both sites -- on close and in the teardown
sweep (`kernel/syscall/cap.cc`, `handle_close` and `cap_teardown`) -- with **no mask at either**. At
16 generation bits the storage width *is* the field width and that is correct by construction. At 15
the same unmasked increment overflows into the 16th bit after 32768 closes, and every handle the slot
then mints carries a generation the codec cannot represent: the slot is bricked.
`SlotPool::free` in `kernel/include/kickos/slotpool.h` has the identical shape on the object side.

The residual escape risk is stated per the Book's criterion
(`docs/book/stale-handles-generation-width-and-allocation-policy.md`): 16 bits is **1 in 65536**
per erroneous use. The blast radius is bounded by construction -- the table is task-relative, so an
aliased stale handle resolves to a different object **in the holder's own table**, one it already
holds. That is an application-correctness failure, not a privilege boundary, which is why a global
handle arena needs an owner check and a per-holder mask on top of its generation and this design
needs neither.

**The rule that keeps `KOS_CAP_AUTHORITY` unmintable: maximum capacity is `2^index_bits - 1`.** The
pseudo-handle is `0x7FFFFFFF`, all ones in its low 31 bits, so **its index field is all ones at
every split**. Forbid the top index value as a slot and no encodable handle can collide with it --
at 16/16, at 15/16, at 17/14 alike. That is why it must be a stated rule of the codec rather than a
coincidence of how wide the capacity happens to be: relying on the capacity's own type -- the deleted
`Thread::cap_capacity` was a `uint16_t` whose maximum was one below 2^16 -- would make the property
vanish the moment that type widened. The capacity is not a field at all now: `thread_cap_capacity`
(`kernel/include/kickos/thread.h`) computes it.

**The rule has to be asserted as `capacity <= 2^index_bits - 1`, not `<= 2^index_bits`.** The
weaker form **permits** capacity == `2^index_bits`, which is exactly the colliding case: the top
index becomes a seatable slot and the pseudo-handle is mintable. `KCAP_RESERVED_INDEX` names that
one forbidden value in `kernel/include/kickos/cap.h` and the assert is written against it, so the
capacity rule and the two pseudo-handles (`KCAP_INVALID`, `KOS_CAP_AUTHORITY`) all stand on the same
stated constant rather than on an incidental field width. The same fact was previously spelled out
in the 15/16 arithmetic in two further places -- a sign-wall comment in `cap.h` and a claim in
`system/include/kickos/sys/cap_index.h` that `KOS_CAP_AUTHORITY` capped `KICKOS_MAX_HANDLES` at
32767 -- and the lesson is the one that made three copies stale at once: state the rule in one
place and derive everything from it.

**The capacity is carried as a `uint32_t`**, the natural word on every target. It costs nothing, it
removes a narrowing conversion, and it needs no change if the index ever widens. It is also why the
assert above is not optional: a `uint32_t` capacity can express a value the 16-bit index cannot
address, so the "index field cannot address the whole table" guard becomes the **real** wall rather
than an incidental one.

The ceiling is `2^index_bits - 1` slots, never the width of the handle word: the generation adds
distinct handle values for one slot over time, not slots. Spending all 32 bits on the index would
reach 2^32 slots and leave no generation at all, which is why the split would be a trade if the
generation were paid for out of the same budget. It is moot in any case -- 2^32 slots is a 32 GB
table for one task, so RAM binds first at every split.

**All three codecs land on 16/16**, and the uniformity is deliberate: one rule and one pair of
numbers, rather than three splits each defended by a different estimate of demand.

| codec | user-visible | budget | what 16/16 costs |
| --- | --- | --- | --- |
| capability handle | yes, via the out-parameter | 31 -> 32 | the ABI change above |
| object pools (`SlotPool`) | no, kernel-internal | 31 -> 32 | the `handle < 0` guards, section 5a |
| thread handle | yes, and was masked to 24 bits for `CAP_REPLY` | 24 -> 32 | relocating the call sequence, below |

**The thread pool needed `CAP_REPLY`'s call sequence relocated, and the packing hid that.**
The reply encoder masked the thread handle to `0xFFFFFF` and packed an 8-bit call sequence into bits
[31:24] of `CapEntry::obj`. That mask was a no-op **only by coincidence**: `ThreadPool::INDEX_BITS`
was 8 and the generation a `uint16_t`, so the handle was exactly 24 bits wide. Widening the thread
index would have let the mask silently eat the generation from the top -- at 15 index bits it left
**9** generation bits, collapsing the late-reply ABA guard in `cap_reply_caller` from 65536 reuses to
512, with no assert anywhere in the tree to say so. The Book chapter on generation width identifies
that counter as the most exposed of the three, being cross-task, holder-controlled and unbounded in
retention.

The way out was spare space already in the entry: `CapType` uses 3 of 8 bits and `CapRights` 3 of 8.
Relocating the call sequence there freed `obj` entirely, so the thread handle now carries a full 32
bits, `CapEntry` stays 8 bytes, and the packing no longer depends on a coincidence. `KCAP_TYPE_BITS`
plus `KCAP_REPLY_SEQ_LO_BITS` is asserted to fill exactly one byte, so the arrangement cannot become
a silent truncation when an unrelated knob moves.

Two reasons not to defer the choice. A re-cut **renumbers every handle value**, which is an ABI
change, and the ABI freezes at the freeze milestone -- so "later" means under
freeze or never. And the split was
*derived per board* from `KICKOS_MAX_HANDLES`, which made the same logical handle print differently
on `microbit` and on `mk64f`; one fixed split removed that.

**Do not widen the handle to 64 bits.** Every MCU target in the fleet is 32-bit -- `armv6m`,
`armv7m` (which is where the Cortex-M33 parts sit too: there is no `armv8m` arch, `qemu-m33` and
`pizero2350` both declare `armv7m`), `rv32imac`, `rxv3`, `lx6` -- so a 64-bit handle costs a
register pair on every syscall return on every one of them to benefit a chip that is not yet in the
fleet. `arch/sim/` is a first-class 64-bit target already and loads a 32-bit handle in one register.
If the M7 MMU horizon makes it worth doing, the codec is one constant by then.

## 5a. The object codec is the binding limit, and it is in scope

Widening the capability index alone would fix the limit that does **not** bind. The one that did was
the OBJECT handle codec: an 8-bit `SlotPool::INDEX_BITS` topped every pool -- semaphores, mutexes,
endpoints, IRQ bindings -- out at **256 objects**, and `ThreadPool::INDEX_BITS` capped threads the
same way. A capability table of 65535 slots pointing into a universe of about 1280 objects is not
scale.

**That 256 was unused headroom, not a hard wall.** The object handle packs `(gen << INDEX_BITS) |
index` with `gen_` a `uint16_t`, so at 8 index bits it spent 8 + 16 = **24 of the 32 available bits
and wasted eight**. Both codecs sit at **16/16** with the rest of the fleet -- `SlotPool::INDEX_BITS`
and `ThreadPool::INDEX_BITS` are 16 -- so each pool addresses **65535 objects with the generation
untouched**, and the whole word is spent.

Three things that had to move with them, all cheap:

- **`cursor_`'s width.** It stores an index, so widening `INDEX_BITS` past it would truncate the
  resume point and alias two slots onto one. The assert is against `UINT16_MAX`, and the cost is one
  byte per pool.
- **The `handle < 0` guards.** With the whole word spent, a handle whose generation has aged into
  bit 31 **is** negative in `int`, so any sign test in `free()` or `resolve()` would silently reject
  live handles. Neither performs one (`kernel/include/kickos/slotpool.h` flags both sites "NO SIGN
  TEST"); what keeps `alloc()`'s `-1` out of range instead is the same capacity rule the capability
  codec uses -- the all-ones index is never seated. The value was already sign-agnostic where it is
  stored, since a `CAP_REPLY` `obj` is routinely negative.
- **The static arrays.** `used_[N]` and `gen_[N]` are statically sized, so a board pays for the `N`
  it **configures**, not for the ceiling. `microbit` configures **4** semaphores
  (`boards/microbit/configs/base/defconfig`); a large part configures thousands and pays
  for thousands. Raising the ceiling therefore costs a small board nothing.

Nothing has hit this because nothing configures anywhere near it. The
`kernel/include/kickos/config/system.h` **defaults** are 16 semaphores, 8 mutexes and 4 endpoints --
28 objects, about **2.7%** of four pools at 256 apiece, or 3.5% counting the 8 IRQ bindings. Those
are defaults and not what the fleet configures: four headers in the tree cut semaphores to 4, across
three boards. It becomes binding on a multi-core part with gigabytes of RAM, which is the stated
direction, so the two codecs are reworked together: they are the same problem twice, with the same
reasoning.

## 6. Provisioning: one number, four declarations, summed at configure

The per-task width cannot become one fleet-wide value. Demand varies in both directions, and the
evidence is measured: below is the per-board demand the sum replaces -- one row per board header that
named its own `KICKOS_MAX_HANDLES`, with the slab that width implied. **No board header declares a
width**; the table is kept for the spread in it, which is the whole argument:

| board | `KICKOS_MAX_THREADS` | `KICKOS_MAX_HANDLES` | slab |
| --- | --- | --- | --- |
| nrf51, stm32f302, stm32f103, bluepill-c8 | 2 | 7 | 224 B |
| xmc4800 | 8 | 12 | 960 B |
| mk64f | 16 | 12 | 1728 B |
| fleet default | 16 | 10 | 1440 B |

A single value breaks something either way. At 12 for all, `microbit` gains 160 B on a board
`TODO.md` measures at 0 to 7 bytes from the `mem_self_grant` cliff. At 7 for all, `xmc4800` and
`mk64f` regress: they need 12 because their SPI service holds a request-endpoint capability in
**root's** table for the life of the image.

**So the number stays, and stops being a board knob.** It becomes a configure-time **sum of four
declarations, each made by whoever knows the fact:**

- **the kernel's reserved indices** -- a constant, `KICKOS_CAP_FIRST_DYNAMIC`;
- **what the chosen service list retains for the life of the image**, declared by the service-list
  target. This is *not* a board property: `KICKOS_SERVICE_LIST` is a `CACHE STRING` with a per-board
  default (root `CMakeLists.txt`), so it is a per-image choice, and one board has several service
  lists that retain different amounts;
- **the app's peak concurrent capabilities**, declared by the app, via a macro mirroring the
  existing `KICKOS_APP_AUTHORITY` (`system/include/kickos/sys/init.h`);
- **the peak concurrent INBOUND reply capabilities** a task's table must hold, declared by whoever
  owns the protocol's fan-in: `INBOUND_REPLY_CAPS` on the service list, `CAPABILITIES_INBOUND_REPLY`
  on the app, combined as the widest. A client mints into the SERVER's table, so without this term
  the sum is not a bound on when a task's own mint can fail.

Beneath the sum sits a floor that is nobody's declaration: a table too narrow to seat a full spawn
grant list is unsound whatever any app asked for, since delegated cap *i* lands at child index
*i*+1 with no runtime check. The floor **raises** the width rather than refusing it, and it refuses
only when it exceeds the board's supply -- so no app is ever told to overstate a demand it does not
have, and `cap.h`'s `KICKOS_MAX_SPAWN_GRANTS < KICKOS_CAP_CHILD_WIDTH` backstop holds by
construction: M4.7.3 re-aimed it at the child default, which is that same floor.

**The summed width is what ROOT gets, and no other task gets it.** Every spawned child is seated at
`KICKOS_CAP_CHILD_WIDTH` -- that same grant-list floor -- and there is no way to ask for anything
else: no per-spawn field, no board knob, and no app declaration reaches it. The constant answers to
`KICKOS_MAX_SPAWN_GRANTS` and the reply term, never to an app's demand, so raising a demand widens
root's table and leaves every child exactly where it was.

**What that costs a child is concrete.** On the sim's selftest image the sum resolves to **10** --
2 reserved + 5 declared peak + 3 optional, nothing retained -- while the floor is **7**
(`KICKOS_MAX_SPAWN_GRANTS` 6 + 1, the reply term being 0). A child spends index 0 on stdout and,
taking two delegated grants, indices 1 and 2 on those (`KOS_SPAWN_DELEGATED_CAP0` is 1), so it
keeps **4** slots of its own where one fleet-wide width gave it **7**. An app whose children hold
more than that has nothing it can declare: the only lever is `KICKOS_MAX_SPAWN_GRANTS`, which
widens every child table in the image at once.

**The reply term is honest but PESSIMISTIC under one width, and that is a reason it stays 0 in
tree.** The four terms are peaks of *different tasks*: an app's peak belongs to the thread doing the
creates, while an inbound reply cap lands in whichever thread is serving. Summing them provisions
root's table for the worst of all roles at once. The in-tree suite's true concurrent inbound-reply
peak is **1**, and declaring it would push the supply-7 boards to 8 and stop them configuring, while
no in-tree server's own peak plus its reply actually exceeds the width it already has. The term is
therefore declared and defaulted to 0: it is the number a reserved reply sub-range has to be sized
from, and it is charged to the child floor as well as to the sum (`_floor = _grants + 1 + _reply`,
`cmake/cap_table.cmake`), so a nonzero declaration widens every table in the image and not only
root's.

**The board declares supply only** -- its arena, and which peripherals exist. It contributes no
demand figure at all.

**The old scheme failed structurally, not through carelessness.** Under it a board's supply figure
was justified by arithmetic of the form "2 reserved + 2 permanent selftest caps + 1 retained SPI ep
+ 6 concurrent in `t_mutex_deadlock`". That is a **board header summing an app's working set and a
chosen service list's retention**, two addends it cannot know. Change the app, change the service list, and
the number is silently stale with nothing to notice. (That comment is no longer in any defconfig;
`grep -rn KICKOS_CAP_TABLE_SUPPLY boards/` is the live list of boards still declaring a supply.)

What the sum buys:

- **Configure sums the demands, checks against the board's supply, and fails naming every term.** A
  shortage is reported at configure with each contributor spelled out, instead of surfacing at
  runtime as a mislabelled pool shortage.
- **A distinct errno.** Exhaustion is currently `-KOS_ENOMEM`, which cannot distinguish "this
  task's table is full" from "the thread pool is empty", so it surfaces as a mislabelled
  `SKIP pool too small`. A full table gets its own code, in the magnitude POSIX uses for EMFILE,
  added to the `KOS_E*` family; `-KOS_ENOMEM` keeps its real meaning. Prior art agrees this is the
  useful cut: seL4 distinguishes five conditions and returns the residual quantity, and Genode has
  a dedicated out-of-capabilities error distinct from out-of-memory.

**The price, stated plainly rather than discovered later:** the sum exposes that `microbit`'s 7
works today only because an arm **skips at runtime**. `mutex_deadlock` -- the case that holds 6
capabilities -- is in `microbit`'s hardcoded skip list: the `EXPECT_SKIPS` list set on the
`microbit_selftest` test (`user/apps/common/selftest/CMakeLists.txt`), so the board never pays
for it. Under a
configure-time sum that skip is not visible, and the two honest options are that the selftest
declares a reduced footprint for 16 KiB parts, or configure states plainly that the board cannot
host the full suite. Nothing here lets 7 satisfy a declared 6-plus-reserved demand.

`mps2` is the other declaration that must be reconciled, and it is a chip serving all four `qemu*`
boards rather than a board: it declares both a class mix **and** a
non-default spawn capacity, and its own comment calls it a deliberate runnable CI gate -- "because
it is a RUNNABLE gate: a mis-sized count fails loudly in CI instead of on somebody's bench". Both
declarations go with sections 3 and 7; the gate intent has to be re-expressed against the sum, not
dropped.

**Any new per-board knob this rework introduces uses the `#ifndef` board-config override seam**, the
way every knob in `kernel/include/kickos/config/system.h` already does, and never a kernel-side
table keyed on board name -- because an out-of-tree board must be able to set it. Recorded as a
separate finding and **out of scope here**: out-of-tree boards are not actually supported today.
`KICKOS_BOARDS_DIR` is hard-derived from the repository layout (`cmake/kickos.cmake`) and is not
overridable, and an installed package can only build the single board it was built for --
`kickos_load_board_descriptor`'s installed-package branch resolves only when the requested board
matches the one recorded at build time, else it is a `FATAL_ERROR` (`cmake/kickos.cmake`).

Related, and folded into the same errno change: `cap_install` used to return a bare `-1` on a full
table, hand-translated at each call site and shadowed by the fact that the raw value reads as
`-KOS_EPERM`. It returns `-KOS_EMFILE` itself now, and every caller
(`kernel/syscall/syscall_obj.cc`, `kernel/syscall/syscall_ipc.cc`, `kernel/irq/irq.cc`) propagates
that `rc` unchanged.

## 7. One law: segmented storage, fully reserved at spawn

The law is **reserve at spawn, never grow**, and it holds for every task on every board. It is NOT
one width fleet-wide: M4.7.3 gave root the summed width and every other task
`KICKOS_CAP_CHILD_WIDTH`, which is what makes the chunk directory below load-bearing rather than
isomorphic to a free list of whole runs.

**Either the worst case is reserved at spawn -- which requires a number -- or it is not, and then
allocation can fail at any point in a task's life.** That is not a conservation law and no claim
here should say it is: donation escapes it, and both seL4 and Genode take that exit, charging table
memory to whoever asked for the object. This design escapes it differently, by reserving
**segmented** storage at spawn: the number is required, and the storage behind it does not have to
be one contiguous block.

**There is one law, not a per-board posture. Growth is removed.** The reason is the reply mint: a
client drives `cap_install` on **another thread's** table (`cap_install_reply` in
`kernel/syscall/syscall_ipc.cc`, called from both `endpoint_call` and `endpoint_recv`), and
`cap_install`'s own contract comment in `kernel/include/kickos/cap.h` already records the property
that makes that safe -- "a client drives this on a SERVER's table, so a full run has to refuse
rather than reach for a chunk". A growing table converts that bounded, self-inflicted refusal into
an **unbounded arena drain charged to the
victim**, and an unrecoverable one: there is no `arch_ram_free` anywhere in `kernel/`, `arch/` or
`system/`, so the drained bytes never come back. Reserving bytes instead of slots does not help
either: reserved bytes are unavailable to everyone else, so the footprint is identical.

**Two refusals, and only one of them is vacuous.** They were conflated, and the second was retired
on the first's evidence:

- the **slab-level spill refusal** ("refuse, never spill" across size classes) is genuinely vacuous
  with one class, and section 3 deletes what it guarded;
- **`cap_install`'s per-task refusal** is the containment boundary above, and it is load-bearing on
  every board. It stays, and growth is what would destroy it.

The design:

- **Chunks of one fixed size**, free-listed through the dead block, which is the pattern
  `ThreadPool::stack_free_list` (`kernel/include/kickos/thread.h`) already uses and documents as
  unable to fragment. An 8-byte `CapEntry` is wide enough to hold that link on a 64-bit target
  (section 4).
- **A spawn takes `ceil(want / CHUNK)` chunks at spawn, or the spawn fails.** `want` is the
  configure-time sum of section 6, so the division is a configure-time computation.
- **`cap_install` never allocates.** That is the whole containment property, restated as an
  implementation rule.
- **A segmented index splits into `(chunk, slot)`**, which must be a shift and a mask for
  `cap_resolve` to stay O(1) -- so the chunk size is a power of two -- and index 0 lands in the
  chunk every task has. **The chunk directory must not make resolve O(chunks)**: a walked list of
  chunks would reintroduce exactly the chained representation section 4 rules out.
- **A chunk count of 1 keeps the flat run, behind `#if`.** This is genuinely two code paths and
  should be named as such rather than sold as one. It is worth the second path because a
  power-of-two chunk costs a 7-handle board a wasted slot, and `armv6m` has no hardware divide, so
  a non-power-of-two chunk cannot buy the exact fit back at runtime.

Mid-life failure -- the ninth `sem_create`, an hour in, refused because *another* task consumed the
arena -- is what all of this exists to BOUND. Three things make it undesirable at every point in the
fleet, not only on a small target: failure stops being a function of the task's own declared needs,
it surfaces inside application logic rather than at provisioning time, and it is unbounded in when
it can occur.

**"Impossible" would be the wrong word, and the reply mint is why.** `cap_install_reply` drives plain
`cap_install` on the SERVER's table, so inbound reply capabilities and that server's own creates draw
on one free list with no partition between them. A server whose window is full of parked callers gets
`-KOS_EMFILE` on its own next create, and the coupling runs between PEERS: one client's `kos_call` can
be refused because three others are mid-call, against a resource neither of them declared. Measured
against the three criteria above, that failure satisfies all of them.

So the claim this design can actually carry is narrower, and it is still worth having: **a bounded,
self-inflicted refusal instead of an unbounded arena drain charged to the victim.** Bounded, because
a full run refuses rather than reaching for a chunk, so the damage cannot leave the one table;
self-inflicted, because the traffic that fills a server's window is traffic that server chose to
accept. What section 6's fourth term buys is that the number configure computes is once again an
honest input to that bound. What it does not buy is a partition: sizing a table for reply traffic is
not the same as reserving part of it for reply traffic, and only a partition would stop client
traffic crowding out a server's own creates.

M4.7.3 supplied that partition, and it is a CAP rather than a reservation: `KICKOS_CAP_REPLY_MAX`
bounds the `CAP_REPLY` entries one task holds at once, enforced at `cap_can_take_reply`, which
replaced `cap_has_free_slot` at both `kos_call` probes. A cap reserves no index, so it neither
collides with delegation placement nor moves any board's width; it needs one counter where a
reservation would have needed a second free-list head plus a range boundary. The partition is
ONE-WAY by design: reply traffic can never crowd out a server's own creates, while the server's own
creates may still take a slot a reply wanted -- which is the residual named just above, and is
self-inflicted for the same reason. The counter is a `Thread` field on the segmented path, where the
chunk directory's second pointer leaves tail padding for it, and a scan bounded by
`KCAP_CHUNK_TARGET` on the flat path, where a field would cost 8 bytes per TCB.

**A cap alone does not deliver the one-way half, and the reason is worth stating because the first
implementation got it wrong.** A bound is only a partition if the width it is spent against was sized
for it. The reply term is summed into the width, but the summed width is ROOT's; a spawned child gets
`KICKOS_CAP_CHILD_WIDTH`, and **every server in the tree is a spawned child**. With the term charged
to root alone, a declared inbound-reply peak of 5 against a 7-slot child let three peers drive that
child's own-create budget to zero -- measured, not argued. So the term is charged to the child width
as well, and `cmake/cap_table.cmake` refuses at configure any combination that would leave a
default-width child no dynamic slot of its own once the bound is spent. The guarantee is a
configure-time property, not a runtime one.

## 8. SMP (M7) consequences

This section was written when multicore was M6; the 2026-08-21 resequencing made it M7, so an "M6"
met elsewhere in this section means the same milestone this heading now names.

Three assumptions in the current subsystem are uniprocessor and bear on this design now:

- `cap_teardown`'s chunked interrupt-masked window excludes nothing on the other cores.
- `Thread::authority` is documented as read without the lock, "so it must stay a single byte that no
  path writes concurrently". That argument needs redoing with explicit ordering.
- The slab free list's push and pop need **a plain spinlock**, not a lock-free construction. A bare
  compare-and-swap is not an option: the free list is a Treiber stack whose next pointer lives
  inside the freed block, which is the textbook ABA failure, and `armv6m` has no `ldrex`/`strex` at
  all (`docs/reference/architecture.md`, "How KickOS differs from its inspirations"). A spinlock is
  the right cost because attach and detach happen at spawn and reclaim -- they are not a hot path.

**READ THIS BEFORE ACTING ON ANY ENTRY BELOW.** This section enumerates what a SECOND CORE would
break. Three of its entries read as uniprocessor defects and are not. The claim-then-commit window, the non-atomic `uint8_t` refcounts and the probe/install
TOCTOU were each re-derived against the tree and each is UNREACHABLE on a uniprocessor, because the
test is not "is `IrqLock` held" but "is there a SWITCH POINT inside the window", and none of the
three windows contains one. Expanding every callee between claim and commit yields no scheduler
entry point; every refcount read-modify-write is a single statement with no call between load and
store, no ISR touches any of them, and a bumped generation closes the double-free a second time.

**The switch-point test is the useful part of that, and it is not uniform across backends.**
`arch_switch` PENDS on armv7m, rv32imac and rxv3, but the sim and LX6 swap INLINE, so a wake inside
a held lock really does hand the CPU to a peer there. An entry below is reachable on one core only
if its window contains such a point. Check that before believing any of them names a live bug.

**That list is incomplete, and the rest is an M6 catalogue this rework does not answer.** Recorded
here because they are all in the capability path and all invisible from a uniprocessor reading:

- **The cross-task reply mint has a probe/install TOCTOU.** `cap_install` on another thread's table
  (`cap_install_reply` in `kernel/syscall/syscall_ipc.cc`, both call sites) is preceded by a probe
  on the target, and the `KICKOS_ASSERT(minted == 0)` that follows is what stands in for a check.
  With another core minting between the probe and the install, that assert FIRES and the system
  panics: `KICKOS_ASSERT` has exactly two definitions in `kernel/include/kickos/kernel.h`, selected
  by `KICKOS_DIAG_TERSE`, and BOTH panic. `KICKOS_DEBUG_ASSERT` is the one that compiles out, and
  there is no `NDEBUG` arm. So the defect is a denial of service a peer can provoke.
  **On ONE core it is not reachable at all**: nothing separates the probe from the install but a
  queue pop, a clamp, a copy and a sequence bump, and the two `sched::wake` calls nearby sit in
  bounce branches that `continue` a loop which re-evaluates the probe. `cap_can_take_reply` and
  `cap_install_reply`'s own refusal are exact complements over the same state, which
  `tests/unit/capprobe` gates from both sides.
- **The `w->dying` check** in `endpoint_call` (`kernel/syscall/syscall_ipc.cc`). Its own comment
  already states the uniprocessor dependency: the sweep drops `IrqLock` between chunks and this
  check is what covers the gap.
- **`cap_reply_caller`** (`kernel/syscall/cap.cc`) is a **four-load, non-atomic** one-shot guard:
  thread generation, then `state`, then `call_state`, then the low byte of `call_seq`. Nothing keeps
  the four consistent with each other across cores.
- **Every refcount is a non-atomic `uint8_t` read-modify-write** -- `sem_refs`, `mutex_refs`,
  `endpoint_refs`, `irq_refs` (`kernel/include/kickos/instance.h`) -- so two concurrent drops
  double-free.
- **`SlotPool`'s `used_`, `gen_` and `cursor_`** (`kernel/include/kickos/slotpool.h`): a plain array
  scan with a shared resume cursor, unsynchronised.
- **`g_teardown_depth`** (`kernel/syscall/cap.cc`), a bare `unsigned`, and **`g_stdout_target`**, the
  published console endpoint every task's index 0 shadows.
- **The endpoint `server` back-pointer** (`kernel/include/kickos/endpoint.h`), re-set at every recv
  and required to be cleared on teardown.
- **The per-run free list, `Thread::cap_free_head`** (`kernel/include/kickos/thread.h`): an intrusive
  list whose links live in the run's own dead entries, and `cap_install` pops its head and
  `cap_install_at` unlinks through it **on a PEER's TCB** -- that is what a client does when it mints
  a reply capability into a server's table (`cap_install_reply`, both call sites in
  `kernel/syscall/syscall_ipc.cc`). Two clients calling one server splice the same list, and so does
  the server's own create.
- **The reply-bound counter, `Thread::cap_reply_live`** (`kernel/include/kickos/thread.h`): the
  same shape as `cap_free_head` above and a different failure, which is the point.
  `cap_install_reply` (`kernel/syscall/cap.cc`) INCREMENTS it on the parked receiver's TCB driven
  by the CLIENT, from both call sites in `kernel/syscall/syscall_ipc.cc`, while only the owner
  decrements it, through `cap_reply_released` at `kos_reply`, at `handle_close` and in the teardown
  sweep. A lost update corrupts no memory: if the decrement is what vanishes, the count never comes
  back down, and the server sits permanently at `KICKOS_CAP_REPLY_MAX` REFUSING every later caller
  with `-KOS_EMFILE` for the rest of its life.
- **The width itself, `Thread::cap_width`** (`kernel/include/kickos/thread.h`, the segmented path's
  stored capacity): written by the SPAWNER on a PEER's TCB -- `cap_slab_attach` seats it in the
  `ThreadAttr` and `thread_create` copies it into the child -- and read at every `cap_lookup`
  (`kernel/syscall/cap.cc`) through `thread_cap_capacity`, as **the bound that keeps `cap_slot`
  inside the task's own run**. Sharper than anything above, because it is not a counter: a torn or
  stale read there is an out-of-run memory access, a table indexed past the last chunk its
  directory holds.
- **`cap_reply_live()` SCANS a peer's whole table on the FLAT path** (`kernel/syscall/cap.cc`, the
  `KCAP_RUN_CHUNKS == 1` arm, where no counter is stored at all): a bounded read-only walk of
  another thread's entries, typed one at a time, with no snapshot and nothing holding them still
  as it goes. A client drives that walk through `cap_can_take_reply` and `cap_install_reply`, and
  `cap_teardown`'s `cap_reply_live(c) == 0` assert stands on it too.
- **The served chain, `Thread::served_head` and `Endpoint::next_served`**
  (`kernel/include/kickos/thread.h`, `kernel/include/kickos/endpoint.h`): `endpoint_server_set`
  splices it at every recv that moves an endpoint's server, and its `endpoint_server_clear` step
  writes the PRIOR server's `served_head` -- a different thread's TCB -- while
  `thread_effective_prio` may be walking that same chain (`kernel/sync/sync.cc`). The walk's own
  liveness argument, that a chain member is always a live slot because `server` is cleared before
  `recv_holders` can reach 0, is an ordering claim with no barrier behind it.

**M7 must answer these. This rework does not**, and the entry width is not what decides them: an
atomic 64-bit load buys nothing while three writers still write the fields separately, without
acquire/release ordering, and the real blocker underneath is object reclamation -- a pointer
`cap_resolve` has just resolved can be freed by another core, which is an RCU or hazard-pointer
problem and not a packing problem.

## 9. Prior art

Surveyed from primary sources. Recorded for the option space, not as a ranking; each choice buys
and pays for something different.

| system | authority from | rights in the reference | per-task bound | epoch |
| --- | --- | --- | --- | --- |
| seL4 | possession | fused, only 4 cap types carry any | none; field widths only, memory is user-donated | no |
| Zircon | possession | fused, immutable | none per task; a fixed global arena | yes, 12-bit |
| Genode | possession | **none at all** | quota, tradeable; base-hw grows a slab from donated memory | no |
| Zephyr | identity (per-object bitmap) | one bit | thread count fixed at build time | no |
| PikeOS | identity (static table) | no | a per-partition kernel-memory budget | not found |
| INTEGRITY-178B | identity (static matrix) | no | a per-partition memory quota | not documented |
| QNX | identity + ability bits | no | per-process resource limits | no |

Five things worth carrying:

- **Nobody puts a *count* in the kernel.** The two safety-certified separation kernels put a
  **memory quota** there and let object counts fall out of it. INTEGRITY-178B's argument is one
  sentence: fixed quotas give bounded worst-case execution time and memory. That is the shape of
  section 7's reservation.
- **Donation is the third exit**, and the one this design declines. seL4 and Genode both charge
  table memory to whoever asks for the object, which removes the per-task number entirely at the
  cost of a memory-accounting model this kernel does not have. Section 7 reserves instead, and
  segments the reservation so the number does not have to buy a contiguous block.
- **Genode carries no rights bits at all**, holding that a narrower right should be a capability to
  a narrower object. Not adopted: a counting semaphore's wait and post are two operations on one
  object, and "may post but not wait" is a real asymmetry that would otherwise cost two objects per
  semaphore.
- **A generation is rare.** Only Zircon has one; the others substitute a scrub, eager global
  revocation, or cooperative release. Section 4 keeps it.
- **Neither capability system narrows rights in transit.** Both require derive-then-send. Enforcing
  `CAP_TRANSFER` at the delegate site rather than in `cap_resolve` is therefore conventional, not
  an inconsistency.

## 10. What must land in the same commit

Each of these is a gate or a claim that goes wrong *silently* if the deletions land without it. A
coupling already satisfied is checked here, not re-done.

- **The selftest's exhaustion arm.** `t_cap_index0` (`user/apps/common/selftest/main.cc`) asserts
  the exhaustion errno on a full table, twice, and asserts the table recovers afterwards. It is
  unconditional -- it runs on every board -- so the new errno of section 6 breaks
  every gate in the fleet until this arm moves with it. Two neighbouring arms in the same file cover
  the storage geometry and the codec's cap-gen half: `t_cap_chunk_span` proves the SEGMENTED index
  decode by holding a live slot at or above the chunk granule (registered as a PARTIAL where the
  configured width compiles the flat decode instead and no such slot exists), and `t_cap_gen_reuse`
  fills the table, closes one slot so the free list has exactly one node, and proves the refill lands
  on that same index with a different cap-gen while the stale handle stops resolving.
- **Every gate expectation derived from a deleted configure variable.** CMake evaluates an undefined
  name as **false**, so a predicate of the form `if(NOT <deleted-variable> AND ...)` does not fail
  when the variable goes: its first clause becomes silently **always true**, and the expectation it
  guarded is then asserted on boards that no longer register the arm at all. Deleting a knob
  therefore means re-deriving every expectation keyed on it, never merely leaving the predicate.
- **The TAP arm counts.** `_tap_arms` and `_tap_arms_p1` .. `_tap_arms_p3`
  (`user/apps/common/selftest/CMakeLists.txt`) are hand-maintained and reconciled against
  each other with a `FATAL_ERROR` at configure time. Deleting an arm without decrementing both the
  whole-suite count and its part fails the configure, which is the correct behaviour and must
  simply be done.
- **The positional capacity parameter** on `spawn` and `spawn_caps` (`user/include/kickos/kos.h`),
  and every call site that passes it or the per-grant destination argument beyond it: a positional
  argument list shifts silently when a middle parameter is removed.
- **The claims the mix made false.** Four sat in the tree: that the sim keeps a fixed stress
  footprint so its CI gate is unchanged (`user/apps/common/stress/main.cc`), that the sim gate has
  deterministic virtual time and is therefore reliable
  (`user/apps/common/stress/CMakeLists.txt` -- it has none, which is why a load-dependent failure
  was assumed impossible), and two saying the mix was behaviour-identical and regressed no image.
  A claim of the last shape still stands in `TODO.md` and belongs in the same sweep. The rule they
  illustrate: a footprint claim about a gate that **sizes itself to a probed budget** cannot be
  written as a fixed number, because the gate reports PASS on either side of the regression.

## 11. Open, and the gate

Not settled by this document:

- Whether detaching the run at the end of `cap_teardown` is safe (section 4). Prove by mutation.
- The concurrent-teardown path is measured at **zero** hits across the whole suite, so
  `cap_teardown_active()` and the deferred console-death reclaim that depends on it are untested.
  A restructuring that removes the chunked window would delete the question instead of answering
  it.
- **`cap_console_publish` checks possession but not service.** Possession *is* checked, by its
  caller: the `KOS_SYS_CONSOLE_PUBLISH` case in `syscall_dispatch` (`kernel/syscall/syscall.cc`) does
  a `cap_lookup`, then re-checks that the entry is a `CAP_ENDPOINT` and that the object still
  resolves. Two things it does not check: **any rights suffice** ("Any rights: the publish is
  identity-only"), and **nothing compares the publisher
  against the endpoint's `server`**. So a holder of a send-only copy can publish an endpoint it does
  not serve, and every task's stdout then points at a rendezvous nobody receives on.
- **`KOS_CAP_CLOCK` is kept, and its reservation is currently aliased.** It is a deliberate provision
  for a userspace CPU governor, which is why index 1 stays reserved rather than being reclaimed as a
  dynamic slot. But `KOS_SPAWN_DELEGATED_CAP0` (`user/include/kickos/sys/abi.h`) places the first
  delegated capability on index 1 under default placement, so **default spawn delegation overwrites
  the reservation**. The aliasing has to be closed -- by moving the default delegation base or by
  refusing a defaulted grant onto a reserved index -- or the provision means nothing.
- **`handle_close` accepts a RESERVED index, and one such call costs a thread its console for good.**
  `cap_lookup` bounds on `thread_cap_capacity` and on nothing else, so `handle_close(c, 0)` resolves:
  slot 0 is seated, its cap-gen is 0, and the bare handle 0 therefore gen-matches. The close bumps
  that gen. Userspace names stdout as the bare constant `KOS_CAP_STDOUT`
  (`system/include/kickos/sys/cap_index.h`) and never as a minted handle, and `cap_seat_stdout`
  re-seats the slot WITHOUT resetting the gen, so no later publish makes handle 0 resolve in that
  thread again. LATENT: nothing in `user/`, `system/`, `tests/` or `examples/` closes a reserved
  index. The open question is whether `handle_close` should refuse below `KICKOS_CAP_FIRST_DYNAMIC`.
- **`KCAP_RUN_OFF_POOL` is 1, where the true peak of concurrently attached runs is
  `KICKOS_THREAD_SLOTS`.** `ThreadPool::alloc` detaches the reclaimed slot's run BEFORE
  `cap_slab_attach` takes the new one (`kernel/include/kickos/thread.h`), so the pool term and
  `thread_create_call`'s in-flight term never both count: the slot a spawn targets holds no run while
  the in-flight one is live. `0` would save one child-width run,
  `KCAP_CHILD_CHUNKS * KCAP_CHUNK_SLOTS * 8` bytes of `.bss`.
  Left alone deliberately: it spends the last
  margin on an allocation whose exhaustion is indistinguishable from a full thread pool, both being
  `-KOS_ENOMEM`.
- **`t_cap_chunk_span` cannot be mutation-proved**, so its evidence is coverage and not detection. A
  CONSISTENT bijective mis-decode in `cap_slot` merely relabels slots: every install/lookup pair
  still agrees and the arm passes. Every non-injective mutation also corrupts `cap_run_free_build`,
  which threads its links through `cap_slot`, and so breaks BOOT rather than that one arm.
  `t_cap_gen_reuse` has a clean kill by contrast, which is the shape to aim for.
- **The fourth provisioning term is declared by NOTHING in tree**, so the summing branch, the
  service-list property read and the diagnostic path that names the term all have zero exercise, and
  a typo in a property name would go unnoticed. The default must stay 0 (section 6), so closing this
  wants a fixture rather than an in-tree declaration.
- ~~**The crowding scenario has no test anywhere**~~ -- CLOSED by M4.7.3's `cap_reply_bound_fast`
  and `cap_reply_bound_slow`, which drive the two `-KOS_EMFILE` probes against the RECEIVER's table
  separately (the `endpoint_call` fastpath and the recv-side scan of parked callers). Both need
  three threads, so both SKIP on the two-slot boards, as predicted.

Before any of this lands: each deletion needs a mutation proof that the gate it claims to protect
actually fails when the property is broken, per the project rule that an unproved gate is not
evidence. The specific ones that matter:

- the sim's thread-slot recovery (16, not 7);
- a wrong per-task width producing the new errno rather than a pool-shortage message, and the
  configure-time sum failing with every term named;
- the `microbit` arena floor holding across the change, measured at 0 to 7 bytes of slack;
- **`microbit`'s skip set, checked by hand in the RUN direction.** "The floor holds" is not
  sufficient: freeing `.bss` can flip a currently-skipped arena arm from SKIP to RUN. But the gate
  will not tell you. `EXPECT_SKIPS` and `EXPECT_PARTIALS` are **permission sets, not budgets**
  (`tests/integration/check_tap_stream.sh` says so and implements it): an unlisted skip FAILS, while a listed
  name that did NOT skip is a `NOTE` and the gate still passes. So a LOSS of slack is caught
  automatically and a GAIN is not. An earlier revision of this list claimed a gain "breaks the gate
  exactly as a loss does"; that was false, and the practical consequence is that any change moving
  `microbit`'s `.bss` must have its skip set diffed by eye.
