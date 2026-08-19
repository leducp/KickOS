<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Adding a kernel object type: the additive recipe

> Chapter 8.1 taught how a task *names* a kernel object -- the per-task handle and the
> resolve chokepoint -- and noted that the capability table itself is frozen: adding a
> new object kind touches neither `CapEntry` nor any table operation. This chapter
> teaches the other half: the object-*side* plumbing behind resolve -- how a new
> object type is created, delegated, closed, and torn down, and how all of that is
> arranged so that adding a type is genuinely additive. It points into
> `../reference/architecture.md` ("Object model, capabilities & IPC") for the exact
> contract and `kernel/syscall/cap.cc` for the code.

## The goal: additivity that is real, not aspirational

A microkernel's value is a small, auditable trusted core. That is only sustainable if
extending the kernel with a new object -- a mutex, an endpoint, a memory grant -- does
not ripple through the existing machinery. The promise is: **one pool, one refcount
array, one resolve case, and one arm in each lifecycle step.** Nothing existing changes
shape; each step merely gains a case.

The capability table delivers half of this for free (Chapter 8.1): `CapEntry` is a
fixed 8 bytes, and `cap_lookup` / `cap_install` / `handle_close` are type-agnostic. But
a capability only *names* an object; something must own the object's storage and its
lifetime. That is the object side, and left naive it is exactly where additivity leaks.

## Why the naive shape does not stay additive

Start with a single object type -- a semaphore. The natural first cut hardcodes it:
the refcount-drop function asserts the type is `CAP_SEM` and has one sem arm; the
delegation path bumps the sem refcount with an `if (type == CAP_SEM)`. It works,
and it even predicts its own limit ("a future type reaching here without its own arm
must trap").

Add a second type and the seams show. There are three distinct places the object side
does something *per type*, and none of them is the resolve chokepoint:

1. **Reference up** -- when a capability is delegated to a child, the object it names
   gains a holder and its refcount must rise.
2. **Reference down** -- when a capability is closed or torn down, the refcount must
   fall, and the object must be freed at zero.
3. **The close-time protocol** -- some object types must *do something* when a
   capability to them goes away, before the refcount drops. A semaphore does not; a
   mutex must refuse closing a lock you own, and force-unlock if the owner died; an
   endpoint must wake blocked senders with a broken-pipe error when the last receiver
   leaves.

Left as scattered `if (type == ...)` tests, each new type edits each site, and the
"additive" claim quietly becomes false.

## The options

**Hardcode per site.** Simple for one type, and it is where you start. Does not scale:
every new type edits every lifecycle function, and a forgotten site is a silent
refcount bug.

**Templated / generic over the type.** Metaprogram the lifecycle over a type list.
This scales mechanically but pulls template machinery into the trusted core, and it
fights the pool design: KickOS pools are deliberately monomorphic (one concrete
`SlotPool<T, N>` per type -- see the template-discipline the project keeps to). A
generic lifecycle would either template the pools too or bridge them with type
erasure, both of which trade auditability for cleverness in exactly the code that must
stay auditable.

**Three explicit switches over the type enum.** One `switch` per lifecycle concern,
each with one arm per object type and a `default` that traps an unknown type in debug.
No templates; the pools stay monomorphic; the trusted core stays a flat, greppable set
of cases.

## KickOS's choice: three switches

KickOS localizes all per-type object plumbing into three functions, each a switch over
the small `CapType` enum:

```
// Reference up. Called from the delegation site for ANY type -- no per-type test
// at the call site anymore. (Carries the delegated rights, which most types
// ignore; a type that gates state on a right, e.g. an endpoint counting its
// receivers, reads it here.) FALSE refuses: a counter is at its ceiling and
// NOTHING moved, which the caller reports as -KOS_EOVERFLOW.
[[nodiscard]] bool obj_ref_inc(CapType type, int obj, uint8_t rights);

// Reference down. Called from handle_close and from cap_teardown.
void obj_ref_drop(CapEntry const& e, bool teardown);

// The type-specific close/exit protocol, run BEFORE the ref drop at both call
// sites. Returns 0, or a negative -KOS_E* to refuse the close (voluntary close
// only), in the same taxonomy every syscall answers in, so the refusal travels
// out unchanged.
int  obj_close_protocol(Thread* closer, CapEntry const& e, bool teardown);
```

Each is a switch whose arms are the object types, with a debug-trapping `default`:

```
void obj_ref_drop(CapEntry const& e, bool teardown)
{
    switch (static_cast<CapType>(e.type))
    {
    case CapType::CAP_SEM:      sem_ref_drop(e.obj, teardown);      return;
    case CapType::CAP_MUTEX:    mutex_ref_drop(e.obj, teardown);    return;
    case CapType::CAP_ENDPOINT: endpoint_ref_drop(e.obj, teardown); return;
    default:
        KICKOS_ASSERT(false);   // unknown type: loud in debug, safe leak in release
        return;
    }
}
```

The debug trap matters: an object type that reaches a lifecycle switch without an arm
is a programming error, and it should fail loudly in development. In release the
`default` simply returns, which *leaks* the object rather than freeing the wrong one --
the same "rather miss than corrupt" instinct the ISR fail-safe follows in Chapter 8.1.

With the switches in place, the delegation site loses its per-type test entirely: it
calls `obj_ref_inc(type, obj, rights)` unconditionally, and delegation is type-agnostic
forever.

## The close protocol runs before the ref drop

`obj_close_protocol` is the extension point most easily overlooked, and its *ordering*
is a contract. At both call sites the protocol runs first, and only then does the
reference drop:

```
handle_close(c, cap):
    e = lookup(c, cap)
    rc = obj_close_protocol(c, *e, /*teardown=*/false)
    if rc != 0:
        return rc                 // -KOS_EBUSY: closing a mutex you own
    detach the cap entry          // bump cap-gen, empty the slot
    obj_ref_drop(detached, false)

cap_teardown(c):                  // called from exit_current, closes every held cap
    for each entry e:
        obj_close_protocol(c, e, /*teardown=*/true)
        detach
        obj_ref_drop(detached, true)
```

The `teardown` flag distinguishes a *voluntary* close (a running thread closing one of
its handles) from *exit teardown* (a dying thread's handles being reclaimed). Two
behaviors depend on it:

- Voluntary close may be **refused** (`return -1`); teardown never can -- a dying
  thread's handles must all go.
- The protocol's action differs. For a mutex, a voluntary close of a lock you own is
  refused; at teardown the same owned mutex is force-unlocked and its waiter woken with
  the owner-died status (Chapter 2.3). Same hook, both cases, one place.

A semaphore's arm is simply `return 0` -- no protocol. That empty arm is the proof the
hook costs nothing where it is not needed: a type that has no close-time work pays one
`return`.

## The refcount discipline: leak, never strand

The reference count is a **global** property of the object slot (Chapter 8.1:
possession is per-task, liveness is global). Its discipline:

- **create** sets refs to 1 and installs a full-rights capability into the creator's
  table;
- **delegate** increments (a child gains a named reference);
- **close / teardown** decrements;
- the object is freed when refs reaches **0** -- the last holder's close.

The one dangerous case is freeing an object while a thread is still *parked* on it (on
its wait queue). Freeing the slot out from under a blocked thread would leave that
thread linked to reclaimed memory -- a strand. The rule is **leak, don't strand**:

```
X_ref_drop(obj, teardown):
    decrement X_refs[obj]      (floored at 0)
    if X_refs[obj] == 0:
        if any thread is still parked on this object:
            KICKOS_ASSERT(teardown)   // must be unreachable on the close path
            X_refs[obj] = 1           // floor the count: LEAK rather than strand
            return
        free the pool slot
```

The assertion encodes a real theorem, not a hope. On the **voluntary-close** path the
strand branch is *unreachable*: a parked thread is `BLOCKED` and cannot be running a
syscall, so it cannot be the one calling close -- and its own capability to the object
keeps the refcount at least 1. So refs can only reach 0 when no thread is parked. The
branch exists only for the **teardown** path (where the argument can, in principle, be
circumvented by unusual object topologies), and there it chooses a bounded leak of one
pool slot over a memory-corrupting strand. This unreachability argument transfers to
*any* object type whose blocked holders cannot run a close -- which is every blocking
object KickOS has.

## The recipe, in full

Given the three switches, adding a new capability type `CAP_X` is exactly:

1. **A pool and a refcount array** (in the kernel instance): a monomorphic
   `SlotPool<X, KICKOS_MAX_X>` plus a parallel `uint8_t x_refs[KICKOS_MAX_X]`.
2. **A config knob** `KICKOS_MAX_X`, board-overridable so tiny targets can shrink it.
3. **An index helper and three arms** in `cap.cc`: `x_index_of`, plus one arm on each
   of the three lifecycle concerns (the close protocol's may be an empty `return 0`).
   The reference-up arm is not written inside `obj_ref_inc` itself: it goes in the small
   locator that arm-per-type *finds the counters* a capability of that type moves, and
   both the bump and its undo are then written once over whatever the locator returned.
   That is the same additive shape with one extra guarantee, that an increment and the
   unwind of a partly-taken batch cannot drift apart, or move one of an endpoint's two
   counters without the other.
4. **One resolve case** in `cap_resolve` (Chapter 8.1): `if (want == CAP_X) return
   pool.resolve(...)`.

Nothing existing changes shape. This is the mutex (Chapter 2.3) and the endpoint
(Chapter 8.3) each landing as one instance of the same recipe.

Two guardrails are worth writing down because they bite silently otherwise:

- **The refcount is `uint8_t`, and the bound is enforced at RUNTIME, not by an assert.**
  The most references an object can accrue is one per possible holder, so the naive bound
  is `KICKOS_MAX_THREADS * KICKOS_MAX_HANDLES`. Do **not** weld that product into a
  `static_assert`: it is a worst case nothing approaches, it omits the boot TCBs and the
  kernel's own console reference, and stating it correctly ties two provisioning knobs
  together tighter than the runtime needs. Instead the ONE increment site refuses:
  `obj_ref_inc` returns false at `UINT8_MAX` and the caller reports `-KOS_EOVERFLOW`,
  so a board that raises a knob gets a refusal it can diagnose rather than a build it
  cannot ship.
- **`obj_ref_inc` carries the rights** even though most arms ignore them. A type whose
  object holds a count of holders-with-a-particular-right (an endpoint tracking its
  receivers -- Chapter 8.3) reads the rights here; the delegation site already has them
  in hand, so it is a signature detail, not a new lookup.

## The degenerate case: a capability with no object

All four steps assume the type *has* an object. Two of KickOS's types do not, in
different degrees, and what the recipe does with them says more about its shape than
the ordinary cases do.

**A type whose `obj` names something that is not a pool slot.** The reply capability
(Chapter 8.5) packs a *parked caller* into its `obj` word as a generational thread
handle. There is no reply pool and no reply refcount array, so steps 1, 2 and 4 do not
apply -- but its close-time work is very real, because a reply capability that goes
away without being used must wake the caller it named with a broken-pipe error. Its
three arms are therefore: reference up, nothing; reference down, nothing; close
protocol, the full stale-resolve-and-wake.

**A type with no object at all** is the limit of that same direction, and arriving there
is a signal rather than a milestone. If `obj` is unused, nothing is allocated, nothing
can be freed, there are no waiters and no close could need to refuse anything, then
steps 1, 2 and 4 vanish and all three of step 3's arms are a bare `return` -- at which
point the entry is a rights byte and a type inside an eight-byte struct, and the honest
question is what it is doing in the table at all. A capability table earns its cost by
naming things whose liveness a holder could otherwise get wrong. State that names
nothing is just state, and belongs on the thread; axis-3 authority is where that
argument is made in full (Chapter 7.4,
*[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*).

Two details make the reply capability a variant of the recipe rather than an exception
to it.

**An empty arm must be written out, not left to the `default`.** The `default` asserts,
and that assertion is load-bearing: it is what catches a type somebody added to the enum
and forgot to wire up. A poolless type falling through to it would trip a debug
assertion on every close of a perfectly correct capability. So "there is nothing to do
here" and "nobody wired this up" have to be *different* answers, even though they behave
identically in release. That is the general rule for any trapping `default`: **a
legitimately empty case has to state its emptiness, or the trap stops meaning
anything.**

**A poolless type must not go through the object-resolving chokepoint.** That chokepoint
exists to turn a validated handle into a pool object; handed a capability with no pool
behind it, it would look up `obj`, find nothing, and correctly return nothing -- so the
type would be permanently unusable through the very path that makes every other type
work. A poolless type is looked up type-agnostically instead, with an **explicit type
test** at the use site. That test is the whole safety argument, and it is not a
formality: the reply capability's `obj` is a packed thread handle, so a reader that
skipped the test and treated it as a pool index would not fail -- it would name an
unrelated live object. The type test is what makes reinterpreting the field sound.

So the recipe generalises by one word: *at most* one pool, one refcount array, and one
resolve case. The three switches are the invariant; the pool is the common case.

## Where to go next

- The naming side this completes -- the handle, resolve chokepoint, and boundary vs
  detector: Chapter 8.1, *Naming a kernel object*.
- Why axis-3 authority is not in the capability table at all, and what test to apply
  before putting anything in it: Chapter 7.4,
  *[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*.
- The first two instances of the recipe: Chapter 2.3, *Priority inheritance* (the
  mutex's close protocol and force-unlock), and Chapter 8.3, *Endpoints* (the
  broken-pipe close protocol).
- The exact refcount, delegation, and teardown contract:
  `../reference/architecture.md` ("Object model, capabilities & IPC") and
  `kernel/syscall/cap.cc`.
