<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS TODO

**M1 VALIDATION COMPLETE (2026-07-14)** -- 10 boards on silicon (5 ISAs) + 3 emulator gates
green; every board boots, has a console, runs the selftest, panics visibly, and runs at its
true (or safely-degraded) clock. Full record in `docs/archive/M1_state.md`. The items still
open below are either optional perf, deferred to M2, or non-gating HW-unverified notes --
none block M1.

Living checklist for **M1** (uniformity / bring-up). Check items off as they land -- this file,
not memory, is the source of truth for "where are we". M2 (MPU enforcement) and M3
(capabilities + clock-select) items are parked at the bottom so they aren't lost.

This file is the **granular, actionable** status. The milestone-level plan (the general idea
per milestone) is `roadmap.md`; validated end-state + per-board detail is
`docs/archive/M1_state.md`; the board/console readiness matrix is `docs/m2-readiness.md`.

## M6.5

- [ ] **`tests/static/trap_redzone_indirect.txt` binds call sites by exact `file:line:column`.**
      Every comment edit, every inserted line and every reflow in a bound file shifts the
      bindings, so an unrelated change breaks the gate and the fix is a hand edit of a table.
      Two files had to be reverted from a comment sweep for this alone. Bind on something the
      edit does not move: the enclosing symbol plus the callee, or a marker in the source.
      Re-derived by hand-driven tooling twice now. Two approaches that do NOT work, so the
      next attempt does not spend them again: `#line` directives to restore numbering, which
      never resync for active code after one lands inside an `#if`-disabled region; and a
      per-instruction "only the immediate changed" check, since on Thumb the line number's
      VALUE selects its encoding and so moves widths, branch offsets and instruction counts.
      What does work is a padded control, every comment hunk restored to its original length
      so each code line keeps its physical number.

## Retired from the M4.7.2 review backlog (triaged 2026-08-06)

M4.7.2, .3, .5 and .6 closed most of the review backlog without the entries being updated. What
survives is below; everything else was re-verified fixed against tree `82fa51f`.

- [ ] **`handle_close` does not refuse a RESERVED index, and one such call costs a thread its
      console for good.** `cap_lookup` bounds on `thread_cap_capacity` and nothing else
      (`kernel/syscall/cap.cc:485`), so `handle_close(c, 0)` resolves -- slot 0 is seated, its
      cap-gen is 0, and the bare handle 0 gen-matches -- and the close bumps that gen
      (`cap.cc:786`). Userspace names stdout as the bare constant `KOS_CAP_STDOUT`
      (`system/include/kickos/sys/cap_index.h:39`) and `cap_seat_stdout` re-seats the slot without
      resetting the gen (`cap.cc:882-904`), so no later publish makes handle 0 resolve in that
      thread again. LATENT: nothing in `user/`, `system/`, `tests/` or `examples/` closes a
      reserved index. Fix is a refusal below `KICKOS_CAP_FIRST_DYNAMIC` plus a selftest arm
      proving it. Also stated in `docs/design-capability-table.md` section 11.
- [ ] **`KICKOS_CAP_RUN_OFF_POOL` reserves one run more than the true peak.** It is 1
      (`cmake/cap_geometry.cmake:26`), the in-flight spawn run, where the peak of concurrently
      ATTACHED runs is `KICKOS_THREAD_SLOTS`: `ThreadPool::alloc` returns the reclaimed slot's run
      before the spawn's `cap_slab_attach` (`kernel/syscall/syscall_thread.cc`), and the slot a
      spawn targets holds none either way, so the in-flight run REPLACES a pool one rather than
      adding to it. Cutting to 0 saves one child-width run of `.bss` and must move
      `cmake/cap_table.cmake`'s footprint arithmetic with it. Deliberately not folded in: it
      spends the last margin on an allocation whose exhaustion is indistinguishable from a full
      thread pool, both `-KOS_ENOMEM`, so it wants its own measurement. M4.7.7 moved the number
      from 2 to 1 by seating root in the pool, which did not touch this margin: the total
      `KCAP_RUN_COUNT` is unchanged.
- [ ] **The out-of-tree capability WARNING has no gate.** `kickos_declare_app_capabilities` warns
      when a declaration cannot be honoured (`cmake/cap_table.cmake:137-144`), but neither
      `examples/oot-app/CMakeLists.txt` nor `examples/oot-mcu-app/CMakeLists.txt` passes any
      capability keyword, so `tests/integration/check_oot_export.sh` and `check_oot_export_mcu.sh` never
      invoke it. Nothing would catch a regression in its text, in the `KICKOS_IN_TREE` detection,
      or in `_kickos_cap_installed_width`. Wants a small OOT app that declares `CAPABILITIES` and
      greps stderr. Proportionate: the warning is a diagnostic, so a regression costs a missing
      warning rather than corruption, and the two OOT gates are delicate.
- [ ] **`grant_reserved` has three `tap::partial` exits the bench cannot tell apart.**
      `user/apps/common/selftest/main.cc:2014` partials at `:2037` (granule alloc failed), `:2064`
      (board reserves nothing) and `:2190` (board mints no DEV window), while
      `selftest/CMakeLists.txt:116` matches `KICKOS_EXPECT_PARTIALS` on the test NAME alone, so a
      partial from an unexpected cause reads as the expected one.
- [ ] **`docs/reference/architecture.md` has never had a full-document correctness audit.** The
      M4.7.x edits corrected only the rows a grep surfaced; the rest is unreviewed. It is the one
      reference doc no reviewer covered in full -- the agent assigned to it died without
      reporting -- and the 2026-07-29 codebase sweep that used to back it up has been deleted
      from the tree, so nothing banked covers this file at all.

## Found during the M4.7.5 configuration-mechanism work (triaged 2026-08-06)

The whole fleet is on Kconfig now, so anything that once read "scoped to a crossed board" applies
to all 20.

- [ ] **Nothing checks that the generated fragment and the `-D` translation carry the same knob
      set.** `tools/kconfig/genconfig.py:35-70` owns 23 fragment variables (8 string, 10 int, 5
      bool); `CMakeLists.txt:92-146` translates a bare `-D` into a `CONFIG_*` request over its own
      lists; nothing compares the two. A knob in the fragment but not the translation is one the
      fragment SILENTLY OVERWRITES -- that shape has now bitten three times (the posture, the five
      booleans, and `KICKOS_SERVICE_LIST`/`KICKOS_BOARD_PINMAP`, whose omission reddened four sim
      gates). `tests/static/check_kconfig_gen.sh:51` drives `genconfig.py` DIRECTLY, so the CMake
      translation never executes under any gate; its only round-trip leg is `KICKOS_SERVICE_LIST`
      (`:149-152`), there is none for `KICKOS_BOARD_PINMAP`, and no leg tests a provisioning
      integer accepted as an override or a boolean forced to `n` against a defconfig that sets it
      `y`. The only real exercise of the translation is `-DKICKOS_SERVICE_LIST=` in the four sim
      gates.
- [ ] **Five booleans reach C from CMake, not from the generated header.** `KICKOS_DEBUG`,
      `KICKOS_ENABLE_SELFTEST`, `KICKOS_BENCH`, `KICKOS_SHUTDOWN_TO_BOOTLOADER`
      (`CMakeLists.txt:245,253,278,295`) and `KICKOS_SCHED_PERIODIC_TICK`
      (`kernel/CMakeLists.txt:93`) arrive by `add_compile_definitions`, because
      `tools/kconfig/genconfig.py:30-31` emits only `INT`/`HEX` symbols. The fragment is therefore
      load-bearing for them and `option()` must defer via CMP0077. Converting the emitter to
      `#if`-style booleans retires the fragment lines, the deference and the
      `check_kconfig_gen.sh:101-106` presence assert together.
- [ ] **A board's provisioning is repeated once per variant and nothing compares the copies.** The
      corpus is `git ls-files '*/configs/*/defconfig'`, 52 defconfigs over 20 boards at the time of
      writing (20 `base`, 14 `flat`, 13 `st`, 3 `bench`, 2 `telem`), each a
      COMPLETE statement rather than a delta on `base` -- which is the Kconfig model and what
      `savedefconfig` writes back. A board with `base`, `st` and `flat` states
      `KICKOS_MAX_THREADS` three times and an edit to one is silent in the other two.
      `check_kconfig_gen.sh` leg 6 iterates every one of them but only asserts each RESOLVES; it never
      diffs a variant against its base, and `savedefconfig` regenerates one variant from the live
      `.config`, so it cannot catch a divergence either. Cheap first cut: a gate asserting every
      variant agrees with its board's `base` outside a per-variant allowlist of the symbols that
      variant exists to change. The rule behind it is the open question -- nothing declares which
      axis a variant owns. Expressing a variant as `base` plus a fragment was considered and
      rejected: it breaks the `savedefconfig` round trip.
- [ ] **The "is this a knob?" rule has never been applied once over the whole symbol set.** The
      rule is in the design -- a hardware fact earns a Kconfig declaration only if some option's
      availability or default depends on it -- but it was only ever applied to the two symbols the
      maintainer asked about (`KICKOS_MAX_IRQ`, `KICKOS_RX_INTB_ENTRIES`), both of which left
      Kconfig for `chip_limits.h`. `KICKOS_CONSOLE` is the named open case: an unconditional
      prompted `choice` at `Kconfig:183-202`, a real choice on a board with two transports and a
      fact on a board with one, with nothing distinguishing them.
- [ ] **A wrong `arch_mpu_region_pow2` / `arch_mpu_min_region` literal is caught by nothing
      in-tree.** `cmake/boot_arena.cmake:53` (`_kickos_seam_int_in_file`, driven from `:140-141`)
      regex-scrapes the return literal out of the same backend TU the link resolves, and also
      reimplements the linker's archive-member selection rule. It is the one value the ownership
      rule cannot place: there is no configuration behind it, so nothing resolves it and there is
      nowhere for it to come FROM. `rx72m` silicon is the only check for the RX MPU. Already
      recorded in `STATE.md`; filed here so it survives the next STATE.md rewrite. Cited without
      a line number ON PURPOSE: it has already moved once, which is the whole reason for the copy.
- [ ] **Decide whether the per-app build stamp should be reproducible.** It folds `__DATE__` and
      `__TIME__` in an app TU (`user/include/kickos/app.h`), so its CODE size varied between two
      builds of an identical tree (measured at 0x8c, 0x90 and 0x94) and every later address shifted
      with it. R2.2 turned the app's half into DATA (`kickos_app_build_raw`) and moved the
      reformatting into `kmain`; the flash-fix pass turned that half into a `const`
      `kickos_app_build_time` and deleted the reformat, so the app's half is now a `.rodata`
      string and no code at all, and the code-size variation is gone. The timestamp is not. It
      exists to answer "did the APP change or was the image relinked", which a content hash would
      answer without perturbing code size. While it stands, **"byte-identical
      image" is not a claim this tree can make** -- and `docs/reference/boards.md:2199` makes it,
      correct in intent ("Tree identity is the test, not hash identity") but wrong in wording.
- [ ] **The C surface is gated on the INSTALLED package only, not per board.**
      `tests/static/check_public_headers.sh` now carries a C11 arm beside the c++17 one. Its
      corpus is DERIVED, not listed: a header whose code guards `extern "C"` with
      `__cplusplus`, plus that header's include closure (24 of 63 headers on a sim package).
      No exclusion list to rot -- `sys/bytes.h`, `sys/spi_service.h` and `kos.h` carry no
      guard at all, so they never claimed C and the rule drops them by itself. `arch/include/kickos/arch/arch.h`
      now states C++-only with an UNGUARDED `extern "C"`, which also takes the per-arch
      `context.h` (`alignas`) out of the corpus with it.
      What is left: the one caller (`check_oot_export.sh`) installs ONE board's package and
      uses the host `gcc`, so the per-arch `context.h` of the other five arches and every chip
      header are unchecked as C. A sibling branch's `check_c_headers.sh` walks the SOURCE tree
      per board with that board's own cross compiler; the two are complementary, and the
      shared derivation should end up in one place instead of two.
- [ ] **`abi.h`'s p-state enum is C++11/C23, not C11.** `user/include/kickos/sys/abi.h`
      declares `typedef enum kos_pstate_e : uint32_t`, and a fixed underlying type is
      something C did not adopt until C23. GCC accepts it as an extension and says nothing
      without `-pedantic`, which `KICKOS_WARN_FLAGS` does not set, so it is invisible to both
      of the things that now compile C: `hello_c` (the tree's only `.c`) and the C11 arm
      above. It breaks the day anyone adds `-pedantic` to a C app, or a stricter C compiler
      reads the header. Recorded, not fixed, on purpose: nothing is broken today. The fixed
      32-bit width IS the stable ABI here, so the fix keeps it -- an unnamed `enum` for the
      three values plus a plain `typedef uint32_t kos_pstate_t`, not a narrower enum.

## M4.7.8 -- the timed wait and the reaper init

An abortable and timed call, a thread join, wait-until-last, and an init that reaps before it shuts
the system down. `roadmap.md`'s ledger carries the number.

**THE TIMED-WAIT HALF SHIPPED IN M4.7.8 (PR #17); ONLY THE REAPER BELOW IS STILL OPEN.** This
section read as unstarted work until 2026-08-16 -- "everything here is settled, what is left is
execution" over seventeen unticked boxes -- while `KOS_SYS_CALL_TIMED` (46) through
`KOS_SYS_SEND_TIMED` (50) were defined in `user/include/kickos/sys/abi.h` and dispatched in
`kernel/syscall/syscall.cc`, `KOS_ETIMEDOUT` was in the errno set, and `kos_call_lens_pack` was
carrying the two-scalars-in-one-slot packing the first item calls new ground. A stale checkbox is
worse than a missing one: it invites the work to be done twice.

**THE ITEMS BELOW ARE KEPT AS THE DESIGN RECORD, NOT AS A TASK LIST.** They stay because the design
spike is gitignored and never entered history, so this is the ONLY place the reasoning exists -- the
rejected alternatives especially. Read them as why the ABI looks the way it does.

- [x] **Two new syscall numbers, a timed call and a join, and NOT a flag on `kos_call`.** The trap
      frame carries the syscall number plus FOUR argument slots (`arch_syscall`, `syscall_dispatch`),
      and `kos_call` already spends all four on `ep`, `buf`, `send_len` and `recv_cap`
      (`user/src/syscall_stubs.cc`), so there is no slot left for a deadline. Freeing one means the
      stub packing `send_len` and `recv_cap` into a single word: both are bounded by `KOS_EP_MSG_MAX`
      (256), so each fits nine bits, and `endpoint_call` already refuses a `send_len` above the bound
      and clamps `recv_cap`. No stub in the tree packs two scalars into one slot today, so this is
      new ground: `kos_sleep_ns` does the OPPOSITE, splitting one 64-bit value across two slots with
      `kos_u64_lo` / `kos_u64_hi`, and it is not a precedent for the move.
- [x] **The timeout is one relative `uint32_t` of MICROSECONDS, with `UINT32_MAX` meaning no
      deadline.** Only one slot is available, and 32 bits of nanoseconds spans 4.3 s, which is too
      short to be a timeout; microseconds give about 71 minutes and give up no resolution that exists,
      because `KICKOS_TIMER_MIN_DELTA_NS` is 20 us (`kernel/include/kickos/config/board.h`) and no
      deadline in the system can be finer. That constant is one fleet-wide `constexpr` with no
      override hook, so the floor is the same on every board today.
- [x] **One deadline spans both call phases for free.** The send-wait to reply-wait transition moves
      the caller through `link` only, from `wq_block` on the endpoint's `send_waiters` to
      `reply_donor_park` on the server's `reply_waiters`, and never touches `tnext`, which is the
      timer delta list's own field and is documented as SEPARATE from `link` exactly so a timed wait
      can be on the timer list and a wait queue at once (`Thread::tnext`). Nothing needs re-arming
      across the handoff.
- [x] **The untimed path must stay free**: no timer arm, no clock read, no sleep-queue touch, all of
      it behind one comparison against the no-deadline sentinel. The M4.7.7 payload sweep puts the
      fixed cost of a round trip at about 35 us on `xmc4800-relax` and about 52 us on `frdmk64f` (the
      zero-length intercept of the sweep; the 8 B points measured 36.0 and 53.6 us), against a payload
      copy of 62.5 and 75.0 ns per byte per copy. The rendezvous is already the expensive part by two
      orders of magnitude and must not grow. Those absolute figures are only in the gitignored bench
      logs (`.session/logs/m477-xmc4800-relax-bench.log`,
      `.session/logs/m477-frdmk64f-bench.log`); what is tracked is the per-byte pair, in the
      `esp32-wroom` clock item below, and `user/apps/common/bench/main.cc`'s note that the sweep's
      slope is twice the per-byte copy.
- [x] **The existing ABA guard already suffices for a timed-out caller, so `cap_reply_caller` needs
      no change.** It rejects on four independent grounds before the reply cap is consumed (index out
      of pool range, thread-slot generation mismatch, a thread no longer parked in
      `CALL_REPLY_WAIT`, and a rolled `call_seq`), and the server sees `-KOS_ESRCH` through the
      `endpoint_reply` branch that is already written and carries the marking "Unreachable today."
      **Correction to the design note: only ONE test exercises the guard, not three.**
      `tests/unit/capreply/capreply_packing.cc` covers the CODEC (`cap_reply_seq_seat`, `cap_reply_seq`,
      `cap_reply_handle`) over all 256 sequence values, and nothing exercises the runtime resolve or
      the `-KOS_ESRCH` outcome, precisely because that outcome is unreachable today. The selftest
      double-reply arm is stopped earlier, by the per-slot cap-gen guard in `cap_lookup`
      (`-KOS_EBADF`), and `tests/unit/slotpool/slotpool_policy.cc` is the generic `SlotPool` wrap
      distance, which `ThreadPool` explicitly is not. A timed call is what makes the branch
      reachable, so it is also what first needs an arm on it.
- [x] **`docs/reference/ipc-call-reply.md` justifies that unreachability with "there is no
      `thread_kill`", which is already stale.** `thread_kill` exists; it is a cooperative cancel flag
      honoured at the target's next `irq_wait` and does not unpark a `CALL_REPLY_WAIT` caller, so the
      code comment still holds while the Reference's REASON does not. A timed call falsifies the
      sentence outright, so fix it in this milestone.
- [x] **One residue the timeout side cannot fix: the reply capability pins a slot in the SERVER's
      table** until the server replies or closes it. `cap_install_reply` mints into the receiver's own
      run and `cap_reply_released` accounts every release, so a caller that times out leaves a live
      inbound reply cap behind, and reclaiming it would mean reaching across a containment boundary.
      It is bounded, by `KICKOS_CAP_REPLY_MAX` against `Thread::cap_reply_live`, and that bound is
      the whole of the answer.
- [x] **A new errno, in the style of the existing set** (`system/include/kickos/sys/errno.h`, 14
      codes today; the magnitude is the contract and the value is always returned negated). An
      `ENOTSUP` code is being added concurrently on `M4.8.1-driver-class` as 95, so the two must not
      collide.
- [x] **Two primitives, not one, and the timeout question resolves per primitive.** There is no join
      of any kind today (`user/include/kickos/sys.h`: "There is no join, so a caller that must
      observe the thread is gone"). A deadline belongs only where the caller knows the bound, so
      `join` by handle takes one: a user joining its own worker knows what it is waiting for.
      **Wait-until-last takes NO deadline.** It is not a wait for an event, it IS the shutdown
      condition, and it is already expressible as the live-thread count reaching 1
      (`sched::live_count`; the kernel's own test is `kernel().live` at 0 after the exiting thread's
      own decrement, in `sched::exit_current`). Root can only name threads it spawned, so `main`'s
      grandchildren are unnameable from root and an aggregate is the only correct shutdown primitive.
- [x] **The reaper init CALLS `main`; it does not spawn it.** "main returned" already IS the join,
      and there is no correct timeout for a join on `main`: a legitimate `main` may run for hours or
      park forever by design, so a supervisor cannot tell wedged from working. Calling it also costs
      no pool slot, no arena stack, no narrower capability table for `main` and no stdout delegation,
      and it keeps working the four in-tree apps that grant a DEV window from `main`. What is given
      up: a wedged `main` wedges the supervisor, and the answer to that is a watchdog rather than a
      supervisor.
- [x] **The send arm is the cheapest of the three, and it closes a PRE-EXISTING wedge.** A plain
      sender parks with `call_state` set to `CALL_NONE` (`endpoint_send`), so it satisfies neither
      call-state-gated term of `thread_effective_prio`, has no priority donation to revert, and is
      handed `KCAP_INVALID` instead of a reply capability; the unwind is unlink, write the result,
      wake. `console_handover_finish` (`user/include/kickos/sys/driver_service.h`) issues a plain
      zero-length `kos_send` on `KOS_CAP_STDOUT` from the bring-up path that runs IN root, on every
      board with a userspace console driver, and the header already states the wedge: a driver that
      hangs in bring-up instead of dying parks that send indefinitely. That send is byte-identical
      to `master`'s (where it lives in the retired `driver_bringup.h`), so the wedge is independent
      of M4.7.7, which only added a second door to it. A timed send plus a deadline at that call site is the item, and it is one line of
      consumer code once the syscall exists.
- [x] **A timed recv is symmetric with the send arm** and closes the practical half of the item
      "`kos_cap_narrow` narrows authority but not endpoint rights, so there is no driver-death story":
      a parked receiver has no last-sender wake at all, and `recv_waiters` are woken by nothing in
      the tree.
- [x] **Root's own kill tag is an enabler, but a weaker one than it reads.** Root now holds a real
      tag (`ThreadPool::ROOT_INDEX`, `ThreadPool::is_root`) instead of sharing `KILL_TAG_BOOT` with
      idle, and that is what makes root nameable by a handle and by a reply capability. It is NOT
      what makes killing a child possible: `thread_create_call` seats `attr.spawner_tag` from
      `kill_tag_of(spawner)` and a BOOT-tagged root already matched its BOOT-tagged children on
      `master`. The load-bearing fact for reaping is the other one: root's children can never be
      orphaned, because orphaning happens only in `ThreadPool::alloc`'s sweep over a reclaimed
      `EXITED` slot and root never reaches `EXITED`, which is what M4.7.7's exit redirect of
      `KOS_SYS_EXIT` into `kickos_terminate` secures.
- [x] **`KICKOS_CAP_CHILD_WIDTH` is NOT a fixed ceiling**, recorded here so the wrong version is not
      repeated. `cmake/cap_table.cmake` computes it as `KICKOS_MAX_SPAWN_GRANTS` plus 1 plus the
      widest declared inbound reply caps, and hands that floor to every child verbatim; the summed
      demand widens root only. `KICKOS_MAX_SPAWN_GRANTS` is a Kconfig knob with `range 2 16` and
      `default 6`, no board in the tree overrides it, and no service list or app declares an inbound
      reply cap, so the width is 7 on every board today. Its Kconfig help states the cost is caller
      stack and not `.bss`, because `thread_create_call` stages the grant list in arrays on the calling
      thread's own stack. The residual constraint worth stating is only that ONE width serves every
      child, so whatever `main` needs sets the floor for every worker in the image.
- [x] **`ktime_on_timer` cannot be reused as the timeout unwind.** It does a generic `sleepq_remove`
      plus `sched::wake` (`kernel/time/time.cc`) and knows nothing else about the thread, while a
      thread parked in IPC is still linked through `link`, which the ready lists, the wait queues and
      a server's `reply_waiters` all reuse one at a time (`kernel/include/kickos/list.h`). The wake
      would leave it on `send_waiters` and then `on_ready` would overwrite the same node, and no
      `wait_result` would be written, so the woken caller would return a stale byte count.
      `sched::block_current` already states the rule: safe for the timer path only, because the sleep
      queue uses the separate `tnext`. What the unwind needs is a TOTAL dispatch keyed on the
      thread's own state.
- [x] **Every unpark path must cancel the pending deadline, or the singly-linked `tnext` chain
      corrupts.** Six are the endpoint rendezvous in `kernel/syscall/syscall_ipc.cc`:
      `endpoint_send`'s receiver wake, `endpoint_recv`'s two `CALL_SEND_WAIT` bounces and its
      plain-sender wake, `endpoint_call`'s fastpath receiver wake, and `endpoint_reply`'s caller
      wake. Eight counting the two `obj_close_protocol` arms that EPIPE-wake IPC waiters (the
      endpoint arm at `recv_holders` zero, and the `CAP_REPLY` arm). Twelve counting the other wait
      parks a deadline could sit under: `sem_post`, `mutex_unlock`, `mutex_force_unlock` and
      `thread_kill`'s `irq_wait` cancel, which is the one path that hand-unlinks from
      `Thread::wait_queue`.
- [x] **Do not derive the timeout's four-way case from the existing fields.** Adding send and recv
      arms turns the unwind into a case over the wait queue and the call state, both already present,
      but a mis-tagged park unwinds the WRONG list silently. `Thread::blocked_on` exists and is a
      `Mutex*` only, the priority-inheritance chain edge; what the kernel really uses is
      `Thread::wait_queue` plus `Thread::call_state`, and a `CALL_REPLY_WAIT` caller is queue-less
      with no back-pointer to its server. `irq_thread_parked` already back-computes an owning object
      from `wait_queue` by `offsetof`, which is the workaround proving there is no tag. The
      generalized tagged `blocked_on` edge probably wants to land first.

### The reaper init is BLOCKED, and not on effort (2026-08-06)

- [ ] **`kos_wait_last()` answers "am I the last thread in the SYSTEM"; a reaper init needs
      "has the APP finished". Those coincide only in an image with no services, and the init is
      never in one.** `sched::add` increments `kernel().live` for every non-idle thread with no
      exclusion for a driver or a daemon (`kernel/sched/sched.cc`), and the init itself spawns the
      service threads one step earlier in `kickos_service_list_run`. So an init that called
      `kos_wait_last()` after `main` would park forever on threads it created. This is not a
      per-app regression to absorb: it is EVERY app whose `main` returns, on `frdmk64f` and
      `xmc4800-relax` at their default enforcing posture and on every `uartirq` / `usbcdc`
      configuration, which is the posture the six-board M4.7.7 fleet capture ran in. Two ctest
      gates would HANG rather than fail (`sim_published_console`, `sim_uartloop`). There is no
      `stop()` hook on `kos_service_bringup`, so "tear the services down, then reap" is not
      available either.
      **The rest of M4.7.8 does not depend on it**: `kos_wait_last()` is app-callable and correct
      as built, and only the app knows which live threads are its own work.
      **The way forward, and it is core-path work needing its own number and an explicit go:**
      classify a thread as app or infrastructure, with ONE privileged call from the init
      immediately after `kickos_service_list_run` returns, marking everything then live as
      infrastructure. That instant is the only one where the set is exactly right, it needs no
      spawn parameter and no per-driver edit, and it gives the existing `kernel().live == 0`
      terminate edge the same meaning, which is the reaper stated properly at the kernel level.
      Residual risk to design against: a driver that respawns after death comes back
      unclassified and would then be reaped on. Rejected alternative, recorded so it is not
      re-derived: a live-count watermark taken after bring-up, which fails because a count is not
      an identity and a driver death or respawn moves the floor.
      **Four apps return from `main` with a child that never exits** and would hang under any
      reaping init, so they need `exit()` said explicitly whenever this lands: `initdemo`
      (`console_sink` parks in recv), `tele_pingpong` (five daemons), `drvdeath`
      (`nest_grandchild` parks on a semaphore nothing posts, and `thread_kill` is cooperative so
      it does not wake that park), and `rootfault` on its no-enforcement fall-through.

## Found auditing the panic and fault scope (2026-08-06)

Two audits are in flight on this, both gitignored spikes in the main checkout
(`.session/spikes/audit-panic-scope-syscall.md`, `.session/spikes/audit-panic-scope-faults.md`).
The items below are the parts that belong in tracked history whatever those audits conclude.

- [ ] **A terminating image without `KOS_AUTH_SYSTEM` cannot exit cleanly: a successful termination
      reports as a crash.** Both routes panic when the shutdown is refused. A returning `main`
      reaches `kos_panic("root: shutdown refused")` in `root_entry` (`kernel/init/kmain.cc`), and
      M4.7.7's root-exit path reaches `kpanic("root: exit shutdown refused")` in the `KOS_SYS_EXIT`
      arm (`kernel/syscall/syscall.cc`); both end in `kfault_terminate`, which exits with the fault
      status 132. `system/include/kickos/sys/init.h` already requires the authority for a returning
      `main` and `docs/reference/invariants.md`'s `init-return-is-shutdown` repeats it, so the rule
      is documented and still only discoverable at runtime; the single gate is
      `user/apps/common/rootauth`, which passes the panic text to `tests/integration/check_app_arms.sh` as a
      must-NOT-appear marker. Proposed shape: refuse the combination at CONFIGURE time, where the
      tree already prefers failing loud and early (the HAS_MPU-without-`mpu.cmake` refusal in the
      root `CMakeLists.txt` is the model). The obstacle to price first is that nothing declares "this
      image terminates" and the authority mask is a C symbol in the app's own translation unit
      (`KICKOS_APP_AUTHORITY`) that no build file reads, so the gate needs a declaration that does
      not exist yet.
- [ ] **The fault path is thread-scoped on FIVE backends (armv6m, armv7m, rxv3, rv32imac, sim)
      and still system-terminal on ONE (xtensa); on that one, isolation still buys only
      detection, attribution and prevention of cross-domain corruption, and NOT availability.**
      `arch/arm/armv7m/arch_armv7m.cc`'s `kickos_armv7m_fault_report` now opens with
      `if (kickos_fault_kill_thread(frame)) { return; }`, which redirects the stacked PC to
      `kickos_thread_fault_exit`; `kfault_terminate()` is the FALLBACK, not every path.
      `kickos_fault_kill_thread` exists on armv6m, armv7m, rv32imac and the sim (plus its shared
      body in `kernel/init/fault.cc`), gated by `user/apps/common/faultsurvive` and
      `user/apps/common/drvdeath`. armv8-m boards run the armv7m reporter (`KICKOS_ARCH` is
      `armv7m` on `qemu-m33` and `pizero2350`) so they inherit the same early return. xtensa and
      rxv3 have no such function: xtensa's `kickos_lx6_fault_report` and rxv3's fault handler both
      still reach `kfault_terminate()` on every path (rxv3 after `kickos_isr_fault` names the
      task), so the original claim stands for those two only. **Correction to carry: `EXC_RETURN`
      bit 2 does NOT distinguish a user-context fault from one taken mid-syscall.** It selects MSP
      against PSP, and a syscall runs in privileged thread mode on the calling thread's own PSP,
      because `SVC_Handler` rewrites the stacked PC to `svc_trampoline` and clears
      `CONTROL.nPRIV`; the reporter reads that bit only to print the stack's name. The sound
      discriminators already in the tree are the stacked `xPSR` IPSR field (printed but never
      decoded), a `CONTROL.nPRIV` read in the handler, and `ctx.resting_npriv`. What a
      thread-scoped death already leans on: `cap_teardown` plus `sched::exit_current` tear down a
      dying thread's capabilities, its domain reference, its held mutexes, its served endpoints
      (EPIPE-waking every parked sender) and its owned IRQ lines. Two real gaps still open: there
      is no grant capability type, so spawn-time windows go only transitively with
      `domain_release`, and a `kos_mem_self_grant` window in `Thread::regions` is never cleared at
      exit at all; the stack, the capability slab run and the orphaning of children are all
      deferred to `ThreadPool::alloc`'s reclaim sweep. `kernel/include/kickos/domain.h` already
      documents a supervisor respawning after a death, requiring that a supervisor learning of it
      by watchdog, timeout or a future join must join before respawning, and no fault path can
      reach that today.
- [ ] **Standard C `exit()` does not link on the ARM ports.** It pulls `__libc_fini_array`, whose
      `_fini` no linker script in this tree defines: every `.ld` carves a `.kickos_app_fini_array`
      output section and then `ASSERT`s it empty, and not one defines or provides `_fini`. The
      failure is "dangerous relocation: unsupported relocation", measured on mps2-an386 (preset
      `qemu`) and banked nowhere; the mechanism is recorded in exactly one place,
      `user/apps/common/sched_exit/main.cc`. The live routes into `_exit` are therefore `abort()` and
      a failed `assert`, both newlib's own, reaching this tree's `_exit` in
      `user/src/newlib_stubs.cc` (force-linked fleet-wide through `-Wl,-u,_exit`). Against this
      project's consumer-API surface a plain C `main` should be able to call `exit()`, so the item is
      to satisfy `__libc_fini_array` in the linker scripts rather than to leave the standard call a
      link error.

## Found by the M4.7.9 ten-angle review (2026-08-07)

Both items were measured on `23b9abb` and neither is a defect this milestone introduced. Both are
scheduler and console core-path work, so neither rides M4.7.9. The capture behind both is
`docs/archive/M4.7.9_teardown_latency_meas.md`.

- [x] **FIXED in M4.8.2. `sched::wake()` suppressed rescheduling for EVERY thread woken while the
      current thread was dying, whatever its priority, which read as starvation rather than as
      safety.** The guard is now three clauses (`kernel/sched/sched.cc`): a null `current`, an
      `EXITED` current, then `dying and t->prio <= current->prio`. Gated by `sched_wake`
      (`tests/unit/schedwake/`) on the new K-seam host fixture, twelve mutants killed. The record is
      `docs/design-m4.8.2-host-unit-tests.md` sections 8.2 to 8.4: the wake-site enumeration this
      item asked for, the four things the original entry got wrong (a priority comparison alone
      strands `exit_current`'s own waiters; all three in-sweep wake sites can preempt; the
      `endpoint_wait_timeout` deflate and the narrowing multiply, which also makes M4.7.9's rejected
      priority deflate partly live already per `docs/design-m4.7.9-fault-isolation.md` section 5.1;
      and the two bounds nobody had stated on the already-live `tick_rr` set), and the mutation table.
      The measurements the entry carried stand and are not re-run: RR at q=20us preempted the sweep
      on 8/8 deaths; with `KCAP_TEARDOWN_CHUNK` forced to 1, 48/48 deaths took up to 8
      (xmc4800-relax) / 13 (frdmk64f) preemptions inside one sweep; nothing broke across 1080
      silicon plus 488 sim deaths.
- [ ] **STILL OWED by the fix above, and it is silicon: narrowing the guard puts MORE traffic
      through the preemptible window between a fault redirect and its stub**, so the fault-record
      race gets more likely, not less. `fault-record-is-printed-only-by-its-owner` is the invariant
      that carries the weight now, and no host gate can discharge it. Wants an enforcing board with
      a fault arm under teardown pressure.

- [ ] **The M4.7.9 fault-path console-pressure figures are stale and want a re-take.** Measured
      when `console_tx_write`'s overflow branch still drained a full ring under `IrqLock`: the
      preemptible THREAD FAULT dump grew from 105us to 290us (xmc4800-relax) and from 132us to
      876us (frdmk64f) under synthetic console pressure. That branch no longer bit-bangs under the
      lock -- `console_tx_write` queues in ring-sized chunks and waits UNMASKED between them, so the
      masked window is one ring copy (at most 128 bytes on every CRLF board), independent of baud,
      at a sub-1% duty cycle, with a bounded synchronous burst only when nothing drains at all.
      `tests/unit/consoletx/` scores the old code at 4096 masked pushes and the new at 1. Re-take
      the two figures under the same synthetic pressure. The drop-on-overflow the old comment
      rejected is moot: nothing is dropped and nothing stalls.

- [ ] **`console_tx_flush_sync` still drains a full ring under `IrqLock`** (up to about 44ms).
      Unreachable from an unprivileged thread -- panic and deinit only -- so it is not the DoS the
      producer path was, but it is now the largest masked window left in the tree.

## Left out of the M4.7.9 diagnostic catalogue, on purpose (2026-08-07)

Both were considered while `include/kickos/diag.h` was built and both were declined for a reason,
not deferred for lack of time. Recorded so the next reader does not re-derive the reasoning and
"finish the job".

- [ ] **Driver bring-up prose is NOT in the catalogue, and a second table would be a different
      mechanism wearing the same name.** `system/` carries 76 `kos::print`, 5 `emit`, 7 `win_puts`
      and 7 `wire_puts` sites, worth 755 bytes on xmc4800-relax hello_c per
      `docs/archive/M4.7.9_footprint_meas.md`. It earns **zero** on both boards that select the
      short column: neither bluepill-c8 nor f302nucleo links a driver archive at all. It also goes
      through the userspace `kos::print` rather than `kputs`, and lives in `kickos_system` rather
      than the kernel, so it cannot share the kernel catalogue's include or its emit path.
      Becomes worth doing only if a driver-carrying board ever selects `KICKOS_DIAG_TERSE`; until
      then the saving is hypothetical and the second table is pure surface.

- [ ] **`KICKOS_DEBUG_ASSERT` still stringifies its whole condition** (`kernel/include/kickos/debug.h`,
      9 sites), so it is the one diagnostic the short column does not reach. Left alone because
      `KICKOS_DEBUG` is `n` on every board and in every preset: wiring a short arm now would add an
      `#if` branch that NOTHING compiles, which is the exact shape that let the bounded-waits knob's
      off posture rot unbuilt until M4.7.9 deleted it (that symbol is not spelled here, because it
      no longer exists in the tree). If `KICKOS_DEBUG` ever becomes selectable by a
      preset, this needs the same treatment `KICKOS_ASSERT` got: drop the condition text and pass
      `__FILE_NAME__` and `__LINE__` as SEPARATE arguments (measured 4x better than one joined
      `"file:line"` literal, see the capture).

## M4.7.4 -- delete the legacy management (nothing is released before the ABI freeze)

KickOS is unreleased and will not ship before the ABI-freeze milestone (M8, the last one), so
**there is no backward compatibility to manage**. Anything that exists only because something else
USED to exist is cost: it must be kept in sync, it reads as a supported path, and it makes a deleted
thing look alive. `roadmap.md`'s ledger
carries the number and the class definition.

**OWED AT THE FREEZE ITSELF, and nothing else in the tree records it: a full doc and comment
sync pass.** The instruments that keep prose honest are deliberately partial and stay that way.
`check_doc_names.sh` validates a PATH and an UPPERCASE-prefixed IDENTIFIER, so every lowercase
seam and function name a design page cites is outside its corpus -- which is how
`kickos_rx_dev_pending_line` stayed valid in `docs/design-driver-era-scope.md` long after it
stopped existing anywhere in the tree. Widening it was considered and REFUSED: it is a helper for
keeping docs roughly in step, not a proof, and a checker that cries wolf gets disabled. The
answer is a deliberate pass at the freeze, when the ABI stops moving and prose written against a
moving target can be reconciled once against a fixed one, rather than a gate that tries to hold
the line continuously against a target that is still moving.

**The sweep has been RUN.** M4.7.3 audited all four surfaces and banked the inventory at
`.session/spikes/legacy-audit.md` (gitignored, main checkout only). M4.7.4 is execution against the
rows below, not discovery. Every row was re-verified at `6f2eb55`.

**Class 1 (tombstones) and class 2 (fallbacks reachable only from a broken build) are EMPTY.** The
one instance of each was `KICKOS_MAX_HANDLES`, and both died with M4.7.3's generated header; the
supply assert that commit orphaned was deleted in the same milestone. An enumeration of every
`if(DEFINED)` (11), `message(FATAL_ERROR)` (50) and `#error`/`#warning` (17) site found no other
instance -- each names something that still exists.

**`.github/workflows/*.yml` is CLEAN end to end** -- the one surface of the four with no residue.
Every symbol, ctest case name, doc path and preset name it references resolves.

- [x] **Class 4, the Reference tier contradicting itself.** `docs/reference/boards.md:670-673` says a **DONE:** it now says the name has zero hits in every code and build file, matching `invariants.md`.
      a root-privilege-knob `FATAL_ERROR` survives in `cmake/KickOSConfig.cmake.in`. No such
      refusal exists and the name greps to zero in every build file;
      `docs/reference/invariants.md`'s `root-unprivileged-idle-alone-privileged` says correctly that
      it was deleted and a stale `-D` is now silently ignored. Two Reference docs disagree and
      `boards.md` is the stale one.
- [x] **Class 4, `architecture.md`'s live repo-layout tree.** `:320` lists `libcxx/  # __cxa_* stubs, **DONE:** the layout names the real `lib/` tree, and the C++ section states what actually exists (`__dso_handle` only, `operator new` a link error).
      guards, operator new/delete`; `lib/` has no such directory and none of those symbols exists
      tree-wide -- `CMakeLists.txt:743` says "a stray operator new stays a link error". `:319`
      annotates `libc/` with a heap and an assert it does not contain. `:803` promises
      `__cxa_pure_virtual` and `__cxa_atexit`; only `__dso_handle` exists.
- [x] **Class 4, a deleted TAP case named in the present tense.** `boards.md:605` and `:2217` say the **DONE for `:2217`**, which named it in the present tense. `:605` was LEFT deliberately: it is a tip-stamped capture at `9a00e73`, and a measurement is never renamed.
      one partial on every row is `cap_capacity`. That case died in `4ad39a8`; the successor is
      `cap_chunk_span`, which `STATE.md` already uses. `boards.md:2290` is a DATED capture row and
      stays as written -- a measurement is never renamed.
- [x] **Class 4, a driver path that no longer exists.** Two source/CMake comments still place drivers **DONE:** `usic.h` names `system/driver/xmc4800/xmcssc` and the two real app paths; the CMake comment names `system/init/<board>/`.
      under a `user/` subtree that was moved to `system/driver/<chip>/<name>/`
      (`CMakeLists.txt:827`, `arch/arm/chip/xmc4800/regs/usic.h:141`, the latter also naming two wrong
      app paths). Zero hits in tracked markdown -- exactly the surface `doc_names` cannot see.
- [x] **Class 4, `roadmap.md` calling landed work open.** `:70` names `sys_cpu_clock_hz()`, which does **DONE:** both marked LANDED with their design records, and the symbol corrected to `kos_cpu_clock_hz`.
      not exist (`kos_cpu_clock_hz()`); `:63,66,71` list the console device handover and the
      clock-select write side as open, and both landed with a LANDED design record each.
- [x] **Class 4, low value but still false.** `kcap_smallest_class_slots()` /
      `kcap_largest_class_slots()` said they pin chunk 4 **now**; neither symbol exists, and both went
      with the capability-class mix in M4.7.1 -- said so in place rather than rewriting two completed
      records. `kickos_armv6m_mpu_commit` now reads as what it was then against the arch-neutral
      `kickos_arch_mpu_commit` it became. `boards.md` said `arch_sim`; the target is
      `kickos_arch_sim`.
      **One of the four claims was itself false and is withdrawn: `qemu_reboot_declined` IS
      registered.** `user/apps/common/rebootdemo/CMakeLists.txt` builds the name from
      `KICKOS_QEMU_MPS2_TAG`, which is `KICKOS_BOARD` with dashes swapped for underscores, so on the
      `qemu` preset it resolves and `ctest -N` lists it as Test #17. Verified by running it, not by
      grepping for the literal -- the reason the audit missed it is that the name never appears as a
      string anywhere.
- [x] **Class 3, and the test for it is NOT "does the MPU gate this chip's peripherals".** It is
      **does this spawn's grantee call a `kos_periph_*` syscall**, because `kernel/syscall/syscall_mem.cc`
      makes MMIO possession *the sole authorisation* for `arch_periph_enable`. By that test only
      `user/apps/common/gpioblink`, `user/apps/frdmk64f/k64console` and `user/apps/frdmk64f/k64drv`
      hold a genuinely inert window; `system/driver/mk64f/{k64dspi,k64uart,k64uartirq}` and
      `user/apps/rx72m/rxdrv` all call `kos_periph_enable`, so their grants are load-bearing and must
      NOT be swept. `k64uart.cc` had already drifted into calling its own live grant inert -- deleting
      it on that comment's word would have killed the K64F console. Corrected in M4.7.3.
      **DONE:** collapsed to one canonical statement, `docs/reference/boards.md` -> "When an MMIO
      grant is INERT, and the one test that decides it", carrying the register-level argument and a
      table of all nine grants by verdict. The seven longhand copies became back-references, and each
      LOAD-BEARING site now says so in its own first line, so the k64uart drift cannot recur silently.
      Two sites that looked like part of this duplication were left alone deliberately: `xmcspi` and
      `f411spi` make a DIFFERENT claim (a vacuous isolation test when enforcement is off), and
      `rxdrv`'s reference to `k64drv` was already correct.
- [x] **Class 4, the largest single instance: `irq_register`, a function that does not exist**, named **DONE:** all ten now name `irq_claim`, the driver-facing call that arms a line.
      in ten comments across `arch/` and `kernel/irq/irq.cc`. The API is `irq_attach` / `irq_claim`.
- [x] **Class 3, `kos_service_cfg.cs_policy` / `.cs_index` ARE legacy after all.** Corrected during the
      audit: a reader comparing `cfg->cs_policy` against the SPI hardware-CS enumerator was added in
      `9ae301f` and **deleted in `dde73ca`**, so a reader really did exist.
      **DONE:** both fields deleted, along with the service-side CS enum that nothing else used, and
      **14** initializers (not 13, across 12 files) rewritten. The `sizeof` assert needed no
      rebalancing after all: `rsv` widened from 2 to 4 bytes keeps the fixed part at exactly 16, so
      the layout is **byte-identical** on both data models -- 32 B on LP64 and 24 B on ILP32, verified
      by compiling the old and new shapes side by side. The identically named `kos_bus_cfg` fields
      were NOT touched and remain live (`k64dspi.cc`, `xmcssc.cc`).
      Ten presets across five ISAs build clean. **`doc_names` caught this row itself** naming the
      deleted enumerator, which is the gate doing its job on a claim written the same day.
- [ ] **The rule to apply:** delete it if its only justification is history. Keep a guard only when it
      catches a mistake somebody can still make today.

**Two things deliberately NOT filed as legacy, with the check that settled each:**

- `KICKOS_MIN_STACK_SIZE`'s `#ifndef` default in `config/system.h` survives: it is reachable when
  `KICKOS_ARCH` matches none of the six ladder arms, which is a NEW PORT and a mistake somebody can
  still make today.

**One maintainer decision, not an evidence gap -- DECIDED 2026-08-05: DELETE.** The gate on the
root-MMIO service list was an empty list plus a `FATAL_ERROR` that could not fire on any
configure in the tree. Its comment argued it should stay for the next board whose bring-up writes
MMIO from root. **The maintainer's ruling: that class of misconfiguration cannot arise under the
M4.7.5 mechanism unless a user genuinely asks for it, and the tree will not carry hand-rolled
catchers for every misuse when a proper framework is coming.** 22 lines deleted; four enforcing
configurations still build, including `frdmk64f` on its full service list, which is the board the
gate used to police.

**A gate blind spot found while doing it -- FIXED 2026-08-13 in M4.8.4.** The gate built its
valid-identifier set from every tracked NON-markdown file, and the audit ledger then tracked as
the audit ledger then tracked under `docs/` recorded this name, so **any name it recorded stayed valid**
even after the build dropped it -- a DOCUMENT behaving as a source, which is wider than the stale
source comment the behaviour was designed around. The fix excludes `docs/` from the identifier scan
only; the corpus being CHECKED is unchanged at every tracked `*.md`. It turned 39 markdown references
to this knob and to the privileged-root one RED, and the doc audit repaired all 39 by rewriting the
claim without spelling the dead name -- fencing the one that is a verbatim capture line.

## How to read this file

The sections below are the granular ledger, newest strata first; older strata are kept for the
measurements in them, not for their framing. `STATE.md` holds the one-screen version.

**Older commit hashes cited in this file resolve against**
`backup/m4.5.1-pre-squash`, `backup/m4.5.1-pre-msg-trim` and the `refs/backup/integration-*` refs.
**These refs are local-only**; pushing them is what makes the citations resolvable for anyone else.
The hashes that reach no branch at all, and why they still stand as witnesses, are in `STATE.md`
under *History that must not be garbage-collected*.

### Decisions taken (do not re-open without new information)

- **`kos_reboot` shares `kos_shutdown`'s authority** (`AUTH_SYSTEM`), rather than taking a capability
  at a reserved index. The reserved spare therefore **stayed free**, which is worth more than bit
  granularity: spending the last well-known index forces the next one to raise
  `KICKOS_CAP_FIRST_DYNAMIC` and costs a dynamic slot on every board. (The spare has since been
  deleted outright and `KICKOS_CAP_FIRST_DYNAMIC` lowered to 2, which only strengthens this.) **LANDED** as
  `KOS_SYS_REBOOT`; recorded in full in the `kos_reboot` section below and in
  `docs/design-unprivileged-root.md` section 9. The stage-4 re-cut renamed that shared bit from
  its old device-authority spelling without splitting reboot from shutdown.
- **`kos_ram_alloc` gets an explicit self-grant**, not an implicit one at alloc: `AUTH_MEMORY`-gated,
  bounded by `KICKOS_MPU_MAX_REGIONS`, failing loud with `-KOS_ENOMEM`. **LANDED** as
  `KOS_SYS_MEM_SELF_GRANT`.
- **The selftest gate asserts a named, posture-dependent expected-skip list**, not a skip budget.
  **LANDED** as `EXPECT_SKIPS`.
- **The service capability index is retired.** The ABI is not stable yet, so a superseded spelling
  is deleted rather than deprecated. **LANDED.**
- **clang-format is decided against** as a gate -- see the CI-hygiene section.
- **The record cites closing commits by SUBJECT, not by hash.** Hashes move under rebase; this
  branch proved it (the A/B witness hash `a463ab9` had to be re-resolved to `22e1c5a`).
  **SUPERSEDED** by the squash: subjects do not survive one either. The record names a single
  resolution target instead, `backup/m4.5.1-pre-squash`.
- **The canvas is mirrored into the repo** so git carries its history, the Cursor path staying the
  live file. **SUPERSEDED TWICE**: the record became an audit ledger edited in place, with no live
  copy outside the tree and no mirror, and that ledger has since been deleted
  (recoverable from git at `b27a409e`). The `.canvas.tsx` survives only on
  `backup/m4.5.1-pre-squash`.
- **Non-goals are appended to the existing `## North star` section of
  `docs/reference/architecture.md`**, not given a new document, because that section already states
  all three goals. **LANDED** as `### Non-goals -- seL4 machinery deliberately NOT adopted`
  (`m4.5.1: state the seL4 machinery KickOS does not adopt, and the arithmetic refuting it`,
  79b7a37): all four, each with its arithmetic.
- **Sequencing: M4 driver breadth and M6 SMP wait behind goal 1** (fleet flip,
  `arch_periph_enable`, `kos_cap_narrow`). Both multiply capability and memory complexity on a
  fleet that at the time still defaulted to privileged root, so doing them first would have widened
  the surface that goal 1 then has to confine. **LANDED** in `roadmap.md` under `## Next`
  (`m4.5.1: put M4 driver breadth and M5 SMP behind goal 1`, a5fc422). The premise is now DISCHARGED
  rather than pending: M4.5.6 deleted the privileged-root posture knob, so goal 1 holds
  unconditionally and
  M4.6's driver work is what comes next.
- **Selftest tiering (core tier + optional tiers) was to be considered for the two 64 KiB boards --
  but MEASURE FIRST, and the measurement says it is not needed today.** See below.

### The tiering measurement (this is the "measure first" result)

The premise was that the fleet defaults to `Debug` with no optimisation, inflating flash ~26%. It
WAS handled by a two-board `-Os` block applied under `KICKOS_ENABLE_SELFTEST`, since DELETED and
subsumed by the fleet `MinSizeRel` default (see the closed item below). No `-Os` appears in the root
`CMakeLists.txt` any more. At branch tip (`text + data`, against 64 KiB of flash):

| Board | flash used | free | headroom |
| --- | --- | --- | --- |
| `bluepill-c8-st` | 51,540 B | 13,996 B | 21.4% |
| `f302nucleo-st` | 51,716 B | 13,820 B | 21.1% |

**Tiering is unnecessary: both boards carry the fleet-uniform suite with ~14 KiB spare.** The
revisit trigger ("only if headroom actually erodes") has **partly fired** -- 1,636 bytes went on
`bluepill-c8-st` and 1,644 on `f302nucleo-st` against the M4.5.1 merge baseline, attributed
symbol-by-symbol in `docs/archive/M4.5_footprint_meas.md` section 9: two new selftest cases
(`t_bus_device_slots` + helpers, `t_reboot_denied`), a real +88 in `domain_for` from the
one-holder-per-MMIO-window check, and `MAX_TESTS` 64->128, whose 512 bytes land on RAM rather than
flash. 21% is still ample, so tiering stays unbuilt.

**The narrower N16 question now has a measured answer: per-preset build types**
(`docs/archive/M4.5_footprint_meas.md` section 10). `CMAKE_BUILD_TYPE=MinSizeRel` is
byte-identical in `.text` to an explicit `-Os`, and its `-DNDEBUG` is inert in this tree -- there is no `NDEBUG`
reference anywhere and no `assert()` outside the `KICKOS_DEBUG` guards -- so its only cost is
losing `-g`, recoverable with `-DCMAKE_C_FLAGS_MINSIZEREL="-Os -DNDEBUG -g"`. It also **inverts the
block's stated cost**: `CMakeLists.txt:139-140` gives that cost as "the `-st` kernel is no longer
codegen-identical to the same board's non-st build", and measured kernel-side on `bluepill-c8`
`hello` the two differ by **13,505 bytes as shipped** and by **114 at `-Os`** (the flag's genuine
content). The unoptimised default is what creates the divergence; widening `-Os` nearly closes it,
which matters because silicon witnesses are taken on `-st` images. **LANDED**: the four MCU base
presets and the sim preset build `MinSizeRel`, `-g` is re-added under that config, and the RP2040 /
RP2350 optimised-build defect that blocked it is fixed. The consequence for the existing witnesses
is the fleet re-witness pass under M4.5.5.

### One new finding: guards that exist but assert almost nothing

Filed as one item because the **pattern** is the point -- a check that is present, green, and
carrying almost no information is worse than an absent one, because it consumes the attention that
would have gone to writing a real check.

- [ ] **Replace the `.bss`-emptiness linker assert and the vacuous `kernel_ctor_placement` with one
      post-link ELF check.**
      - `ASSERT(_ebss > _sbss)` in the linker scripts only fires when kernel `.bss` is **entirely
        empty**, which needs all four archive selectors to fail at once. It misses the far likelier
        **partial** case: one KickOS archive renamed, or a new one added and not listed in all
        **eleven** scripts. That library's writable state then sits silently inside the app's
        granted window -- an isolation hole that the assert reports as fine.
      - `kernel_ctor_placement` passes **vacuously fleet-wide** (same class: green, asserting
        nothing).
      - **Proposed fix, in an idiom the project already uses** (`check_kernel_ctor_placement.sh`,
        `check_oot_export.sh`): a post-link ELF check asserting that **no symbol from a
        kernel-owned object lands inside `[__kickos_appdata_start, __kickos_appdata_end)`**. That
        catches the partial case the linker script structurally cannot.
      - **Why not fix it in the linker script:** GNU ld cannot be asked whether an input selector
        matched anything, so the in-script version can only ever approximate. This is a limitation
        of the tool, not of the attempt -- worth recording so the next person does not retry it.

### Remaining queue, in dependency order

Ordered so each step's input exists when it starts. Items 1-2 are cheap and unblock judgement;
the sweeps go last because they touch everything and would conflict with any of the above. Closed
items keep their number and are struck through rather than removed, because they are cited by
number: the record and the XMC entry under Blockers below both point at **item 5**.

1. ~~**Measure `-Os` on the tight boards**~~ -- DONE above. Outcome: **tiering not needed.** The
   narrower question of how `-Os` should be expressed is answered above too, and landed:
   per-preset build types (`MinSizeRel`).
2. **Selftest tiering** -- **do not build it.** Headroom has since eroded by ~1.6 KiB to 21.4%,
   which is a partial fire of that trigger and still ample. Kept in the queue only so the next
   reader sees it was considered and refuted by measurement, not forgotten.
3. ~~**Non-goals into `docs/reference/architecture.md`, appended to the existing `## North star`
   section.**~~ -- DONE (79b7a37). All four landed with the arithmetic that refuted them (no untyped
   memory / `Retype`; no CNodes; no derivation tree; no per-instance capabilities), under
   `### Non-goals -- seL4 machinery deliberately NOT adopted`, and the common thread is stated: the
   16-slot ceiling with the tiny boards' tables under it. **That section's `read`/`open`/`socket` sentence
   stays alone** -- the maintainer reads it as a design rule, not a status claim. It was not
   touched, and should not be by a later pass.
4. ~~**Record the sequencing note** (M4 driver breadth and M6 SMP behind goal 1) in `roadmap.md`.~~
   -- DONE (a5fc422), as a block quote under `## Next`.
5. ~~**Move the XMC USIC bring-up into the granted driver thread, and add the privileged configure
   seam it needs for FDR/BRG/CCR.**~~ -- DONE in M4.5.6 (`KOS_SYS_PERIPH_REG_WRITE = 42` /
   `arch_periph_reg_write`, an exact `(base, offset)` allowlist, possession-gated), both parts:
   `xmc_spi0_start` now contains zero register access. The three PV-write-only stores were a measured
   hardware refusal and the seam is witnessed discriminating against them on silicon -- see the entry
   under Blockers below. It closes the consequence too: `xmcssc` AS A SERVICE is witnessed at
   `commit 270b6fa` and `kickos_services_xmc4800relax` came off the root-MMIO
   refusal list, which was emptied then and has since been deleted outright.
6. **`stm32f103` `arch_mpu_min_region()` override.**
7. **Re-point `kernel_ctor_placement` at the `cxxtest` ELF** (it is vacuous where it is now; see
   the finding above -- these two are the same problem and can land together).
8. **CI hygiene set, minus clang-format** (which is decided against).
9. **Branch-wide comment sweep** -- last, because it touches everything.
10. **Commit-message reword as a SEPARATE step after the sweep**, not folded into it.
11. **Record citation pass** -- after the record edits, so it runs over the final text.

## M4.5.1 -- kernel audit follow-ups (2026-07-26) -- COMPLETE except S4 (2026-07-27)

Findings from a code audit of the kernel, all rated **Medium or below** -- none is a live
escalation or a fleet blocker. They are bound-the-unbounded / be-honest-about-the-error
hardening, roughly ordered by exposure. Six of seven landed; the seventh turned out not to
be implementable where it was filed, and says so in code.

Verified on the host sim and QEMU only, each with a gate checked to FAIL first. **These commits
have not yet been through CI** -- but the branch under them has: the maintainer reports **CI green
at `16d89a0`**, the tip before this batch, covering the branch's first 33 commits. Every commit
that touches `.github/workflows/` is at or before it, so the fleet-wide `-Werror` and the rest of
the pipeline are now observed on CI's pinned 15.2.rel1 rather than argued from a local 15.3.rel1.
Uncovered: everything after `16d89a0` -- stage 0/1 of unprivileged-root, this batch, the book
work, the stage-2 flip and its silicon record, and the record passes over all of it. That is
**49 commits as of this commit**, derived as `git rev-list --count 16d89a0..HEAD` on a branch that
stands 82 commits from `master`, with `16d89a0` at position 33 by the same count against
`master..16d89a0`. **Take the command over the number**: it grows with every commit until CI runs
again. The figure this replaces (21) was correct at `3e8ed10` and went stale when the two transport
branches were replayed in, which is the whole reason the method is written down here. CI status
itself is the maintainer's report, not checked from here (`gh` unauthenticated, and `ci.yml`
triggers `push` only on `master`).

- [x] **Bound the semaphore `count`** -- `m4.5.1: bound the semaphore count` (66280c1). The hole
      was **sharper than filed**: `sem_create` validated nothing,
      so the overflow was not "enough posts" but *one* -- `kos_sem_create(INT_MAX)` then a
      single post. Now `sem_create` refuses an initial outside `[0, KOS_SEM_COUNT_MAX]`
      (-KOS_EINVAL) and `sem_post` refuses at the ceiling (-KOS_EOVERFLOW, a new code); the two
      ISR posters ignore the refusal, matching their coalescing contract. Gate: the two arms of
      `sem_destroy`, each checked to fail on its own.
- [x] **Read `console_chip_writers()` under `IrqLock`** -- `m4.5.1: read the console writer
      count under IrqLock` (127dae2). One load under the lock; no livelock,
      since the drain yields between polls rather than spinning inside a critical section.
      NOT gated: reproducing a torn read needs a race the suite cannot schedule -- the change
      is an argument, not a demonstration, and that is worth saying.
- [x] **Split `domain_for`'s refusal** -- `m4.5.1: split domain_for's refusal reasons`
      (296e030). An out-parameter errno (EPERM inadmissible / EINVAL malformed /
      ENOMEM exhausted), forwarded verbatim by `thread_create_call`. The spawn-boundary pre-check
      that existed only to recover the errno the chokepoint could not express is gone, so the
      duplication went with the fix. Gate: `grant_reserved`, checked to fail on ENOMEM.
- [x] **Debug asserts on the intrusive list** -- `m4.5.1: assert list membership on push_back
      and unlink` (0022c82). New `KICKOS_DEBUG` knob, default OFF (board
      images byte-identical); **the sim preset turns it ON**, so the guards run against the
      whole suite on every sim run instead of rotting unbuilt. Both checked to fire.
- [x] **Bound the spin in `wq_confirm_resume`** -- `m4.5.1: bound the resume spin` (6961989).
      Measured while gating it: the loop takes **zero
      iterations on the sim AND on qemu armv7m**, so the barrier the comments describe at
      length has never been observed to spin even once -- see the new finding below. Proven by
      holding the epoch so the switch never lands: an infinite hang becomes a panic on both.
- [ ] **Implement `__malloc_lock`/`__malloc_unlock`** -- **NOT DONE, and it cannot be done
      here.** `m4.5.1: document why the malloc lock stays a no-op` (291815c)
      replaces the vague FOOTGUN comment with three measured facts: newlib takes this lock
      RECURSIVELY (in the linked `cxxtest` image `_free_r` holds it and calls
      `_malloc_trim_r`, which takes it again), so a non-recursive lock self-deadlocks and a
      re-entry detector fires on a legitimate free; a recursive lock needs thread identity and
      userspace has none; and capabilities are per-task, so there is no lock object two threads
      can name and no reserved index left to seat one at. "An IrqLock-equivalent" is not
      available -- this file is userspace, which is the whole point of the surrounding
      milestone. Real fix: the per-thread libc state / TLS item under "Later -- not M1", or a
      kernel-held lock behind a syscall (a designed change).
- [x] **Guard the `uint16_t` domain refcount** -- `m4.5.1: bound the domain refcount at the
      thread pool` (b72e9e5). The count is live threads and nothing else, so
      a `static_assert` proves the wrap unreachable, as the object-refcount arrays already do.
      Bound against the thread HANDLE INDEX ceiling, not `KICKOS_MAX_THREADS`: a board sets the
      latter to 2..16, so an assert on it could never fire. Checked live by widening
      `INDEX_BITS`. Plus a `KICKOS_DEBUG` assert for the way the bound would break first.

## M4.5.1 -- found during the CI / out-of-tree hardening work (2026-07-26)

- [x] **Split `_sbrk` into its own TU.** -- `m4.5.1: move _sbrk out of the force-linked TU`
      (c539d1c). **The split alone is necessary but not sufficient, which
      the plan did not anticipate**: the g++ driver appends libc and libstdc++ AFTER everything
      CMake emits, so an on-demand `_sbrk` member sits behind the linker by the time `_sbrk_r`
      asks -- measured as `undefined reference to _sbrk` on EVERY allocating image fleet-wide.
      The toolchain runtime therefore joins the rescan group, which moves the group from
      `kickos_core` onto the two posture leaves (the freestanding leaf must NOT name libstdc++,
      and CMake forbids one target's closure carrying a library in two groups); `kickos_cxx_rt`
      names its include providers directly as a result.
      Fail-loud PROVEN by construction, with both heap bounds deleted from `mps2.ld`: an app
      calling `malloc` fails the link naming `_kickos_heap_start`, an app using `new` fails the
      same way on the full-C++ leaf, a non-allocating app still links (the property that was
      impossible before -- the force-linked reference broke those too), and a real board is
      unaffected. Both out-of-tree export gates pass, so the exported package still links.
      Follow-on now unblocked: nrf51 can drop the zero-length `.userheap` it only kept for this.
- [x] ~~**Override `arch_mpu_min_region()` to 0 in `chip_stm32f103.cc`.**~~ DONE (`6d49e14`),
      and `chip_stm32f302.cc` with it: both parts have no MPU, so they were paying the v7-M pow2
      alignment tax for regions nothing programs.
- [ ] **Re-point `kernel_ctor_placement` at the `cxxtest` ELF.** The gate passes fleet-wide, but
      vacuously: every app it inspects links an empty `.kickos_app_init_array` window, so the
      script takes its early-out without ever dereferencing a pointer. `cxxtest` is the one image
      with real app ctors -- point the gate at it so it actually asserts something.
- [ ] **Console bytes lost on shutdown.** On a service-list board, root returning while the
      userspace driver still holds queued bytes loses them: `console_tx_flush_sync()` is a no-op
      (the ring was disarmed by `console_tx_deinit`) and `arch_shutdown` then spins forever.
      Shutdown has to drain through the owning driver, not the retired kernel ring.
- [x] **Give `thread_create_call`'s two READ checks the same static-data fallback the write side just
      got.** -- `m4.5.1: use user_readable_ok for thread_create_call's two read checks`
      (fe68c72). One word at each site; byte-identical on an enforcing backend, where the
      fallback arm returns false. Gate: the `authority_cap` worker's params struct and grant
      array became GLOBALS -- exactly the shape that failed -- and the comment recording the bug
      as a local workaround is gone with it. A **fourth** spawn probe was needed for the array:
      the other three are all refused by an authority check that runs before the delegation
      loop, so none of them read `caps[]` at all and the array site would have shipped
      unexercised. Each of the two sites checked to fail on its own.
- [x] **The fleet-uniform selftest image no longer fits the smallest flash. DECIDED, TWICE, and
      this box outlived both rulings**: the fleet moved to `MinSizeRel` and `Debug` was ruled
      unsupported on the 64 KiB boards. Kept for the measurement below, not as an open question.** Re-measured 2026-07-27 at `7eb9592`: the headroom
      was **104 bytes** on `f302nucleo-st` and **292** on `bluepill-c8-st`, not the 96/284
      recorded here (measure from the program headers, not `size`'s text+data). The seven
      kernel-hardening items then cost **184 bytes** on f302nucleo, so the board stopped linking
      on KERNEL code, before a single new test -- which is not the shape this item predicted.
      **The measurement that should decide it:** the same f302nucleo image is **48,848 bytes at
      `-Os`** against 65,720 at the fleet default -- and the fleet default is `CMAKE_BUILD_TYPE=
      Debug`, i.e. `-g` with **no `-O` at all**. So the ceiling is ~74% real code and ~26%
      unoptimised codegen, and the choice this item poses (shed coverage / accept build-only)
      has a third answer nobody had costed: 16.7 KiB comes back for free.
      **Holding measure landed** (`m4.5.1: build the two 64 KiB boards at -Os`, e946003): `-Os`
      across the selftest tree for these two boards only, `-g`
      kept, no test dropped, one block to revert. What it costs, stated plainly: the `-st`
      image's kernel is no longer codegen-identical to the same board's non-st build, which the
      selftest `-Os` block deliberately preserved when it optimised the app's own TUs only. Both
      boards are build-only for the suite (no bench unit, no QEMU model), so what they provide
      is a link check and this keeps them providing one. **Widening `-Os` to the fleet, or
      accepting either board as suite-exempt, is the maintainer's call.**
      Unchanged: `TAP_CHECK` embeds `__FILE__` plus its stringified condition, so assertion
      count is a flash cost; CI does not catch any of this, because its ARM matrix builds the
      PLAIN board presets, not the `-st` ones. Same class as `tests/tap/tap.cc`'s `MAX_TESTS`
      ceiling, which is a per-board RAM cost the smallest board pays for the whole fleet. It is no
      longer a SILENT ceiling: registrations dropped past it are counted and `tap::run_all` emits a
      failing `tap_registry_overflow` line.
## M4.5.1 -- found during the kernel-audit batch (2026-07-27)

- [ ] **The resume barrier has never been observed to spin.** Bounding
      `wq_confirm_resume` (6961989) needed a gate, and calibrating one measured that the loop
      takes **zero iterations on the host sim AND on qemu armv7m** across the whole suite: a cap
      of one iteration does not fire. The sim is expected (its switch is synchronous inside
      `wq_block`), but ARM is not -- the long comment at `sync.h` explains that `arch_switch`
      only PENDS PendSV and `arch_irq_restore` has no ISB, so 1-2 instructions retire on the
      not-yet-switched thread. Either the switch always lands before the caller reaches the
      barrier (a call and a return later), or the window is narrower than the comment implies.
      Worth settling, because the barrier is on the mutex/endpoint wake path on every board and
      is currently justified by an argument nothing exercises. Silicon witness still owed --
      timing is exactly what an emulator does not reproduce.
- [x] **The whole fleet builds unoptimised: CLOSED.** The four MCU base presets and the sim preset
      set `CMAKE_BUILD_TYPE=MinSizeRel` and every board inherits it; `-g` is re-added under that
      config (`CMakeLists.txt:139`), because an image with no debug info cannot be witnessed on
      silicon. The two-board `-Os` holding block is deleted, subsumed by the fleet default. The gap
      it closed, measured on f302nucleo's selftest image: 64,408 bytes unoptimised against 47,120 at
      `-Os`, a 26.8% reduction with no source change (`docs/archive/M4.5_footprint_meas.md`
      section 3, which
      measures all fourteen boards both ways). One caveat stands: the switch's I-cache footprint
      plausibly moved too, but **no instruction-cache or cycle measurement was ever taken**, so that
      is not to be cited as measured (`docs/archive/M4.5_footprint_meas.md` section 10).
      Consequence: every
      published bench figure measured unoptimised code, and every silicon witness except the
      2026-07-29 `f302nucleo` captures, which are the first taken on `-Os` -- see the fleet
      re-witness pass under M4.5.5 below. No bench has been run optimised.
- [x] **`picopi` and `pizero2350` cannot build an optimised image: FIXED**, which is what unblocked
      the fleet build type above.
      `arch/arm/chip/rp2040/chip_rp2040.cc:80-81` and `arch/arm/chip/rp2350/chip_rp2350.cc:96-97`
      -- the bootrom-header `r8`/`r16` accessors, `*reinterpret_cast<volatile uint8_t*>(addr)` --
      fail `-Werror=array-bounds` ("array subscript 0 is outside array bounds of
      `volatile uint8_t [0]`") at `-O2` and at `-Os`, and compile clean at `-O0` and `-O1`.
      Reproduced both by compiling each TU directly and as a `pizero2350-st` build at
      `-DCMAKE_BUILD_TYPE=MinSizeRel`; the diagnostic count varies with toolchain version, the
      pass/fail split by optimisation level does not. The accessors exist only under
      `KICKOS_ENABLE_SELFTEST` (they serve `arch_reboot`), which is why the default build and the
      flag-off `-Os` build both pass -- flag-gated code is exactly the code no default build
      compiles. **Cause, confirmed by flag bisection:** GCC treats the first
      `--param=min-pagesize` bytes of the address space as unmapped, so a constant-address
      dereference down there is an out-of-bounds access to it -- and on both chips the bootrom
      the accessors read *is* at address 0. `--param=min-pagesize=0` silences all four
      diagnostics on the rp2350 TU with nothing else changed. The remedy is a
      `#pragma GCC diagnostic ignored "-Warray-bounds"` scoped to the two accessor bodies
      (`chip_rp2040.cc:80-82`, `chip_rp2350.cc:96-98`), which compiles clean at `-Os` and `-O2` on
      both TUs; a global `--param` would blind the whole tree to a real diagnostic class. The
      optimised accessors are **disassembly-verified only, never executed** -- folded into the
      M4.5.5 re-witness pass below.
- [ ] **LTO does not link, on any board.** `-flto` fails every app with
      `(.isr_vector+0x4): undefined reference to Reset_Handler`. The handler is defined in a C++ TU
      and referenced **only** from the vector table in an assembly object, so the LTO plugin sees
      no reason to keep the definition (measured on `bluepill-c8`; the defect record is
      `docs/design-flash-footprint.md`, *LTO does not link*). So LTO is not an available footprint
      recovery today. It is not on the `-Os` path
      N16 needs, so it gates nothing -- filed so it is not rediscovered. **Record only: no fix
      attempted.**
- [ ] **`kickos_core` no longer carries the archive group.** c539d1c moved the RESCAN group onto
      the `kickos` / `kickos_cxx` leaves, because the two postures need different toolchain
      runtimes in it and CMake forbids one target's closure carrying a library in two groups. A
      consumer linking `kickos_core` DIRECTLY now gets usage requirements but no archives. The
      documented contract already says consumers link a leaf and never core, and both
      out-of-tree export gates pass -- but core is still in the export set, so the contract is
      now load-bearing where it used to be advice. Either state it in the exported package or
      make linking core alone a configure-time error.
- [x] **The record cites commit hashes a rebase has rewritten: CLOSED BY CONVENTION (2026-07-28).**
      Found while re-resolving the audit record: of the 56 hashes it cited, 37 named commits
      unreachable from `HEAD` after the message-trim rebase. 32 were remapped (patch-id, or a
      unique exact subject); 16 squash casualties stayed flagged. The M4.5.1 squash to eight
      theme commits then made per-citation conversion a losing game -- a subject survives a
      reword but not a squash -- so the convention is a resolution TARGET instead: every hash
      and subject this branch's records cite resolves against `backup/m4.5.1-pre-squash`
      (tree-identical full history), stated once in the audit page's header. The standing
      practice: create the backup ref BEFORE any history edit, and say in the record which ref
      citations resolve against. This file's own six M3-era hashes resolve against
      `backup/m3-pre-squash` the same way.
- [ ] **Reclaim `arch_ram_alloc`'s alignment run-up** (allocator work). The bytes skipped
      ahead of each allocation to satisfy its alignment are dropped on the floor -- which is why
      boot-stack allocation *order* is load-bearing today (idle must be allocated before root).
      Folding the run-up back into the free space removes that ordering constraint. Subsumed by
      the general freeing allocator under "Later -- not M1".

## M4.5.1 -- CI hygiene (2026-07-26)

- [x] **Reduce `--repeat until-pass:4` to 2 on the four QEMU gates** (M5.2.1): it is 2, and it is
      scoped to the five gates that actually poll a wall-clock output window rather than to whole
      suites. The RISC-V jobs carry no retry at all, their clock not being the coarse one the
      retry was for.
- [x] **Tighten the sim `mpu_fault` failure regex** (M4.5.8): the sim registration now runs
      `tests/integration/check_mpu_fault.sh`, the same script the QEMU boards use, which also pins the reported
      fault address to the one the app announces.
- [ ] **Add link-only CI jobs for `f302nucleo-st` and `bluepill-c8-st`** (maintainer-confirmed
      2026-07-27). CI builds only the plain presets, so a selftest image that overflows 64 KiB of
      flash goes unnoticed until someone builds the `-st` preset by hand. These two boards are
      build-only for the suite anyway -- a link check is exactly and only what they provide, so a
      link-only job is the whole value at none of the runtime cost. Both link today with ~14 KiB
      spare (measured in the session record above, and shrinking), and the job is what keeps that
      true. Note for whoever adds it: the base presets build `MinSizeRel` fleet-wide, so the job
      still has to configure the `-st` VARIANT -- only the selftest image risks overflow. The
      per-board `-Os` block this note used to name is gone. A local sweep of all
      thirteen `-st` presets found one real link break that the seven emulator gates could not
      (`esp32-wroom-st`, Xtensa, missing `kickos_arch_mpu_commit`), which is the argument for
      widening this beyond the two tight boards later.
- [x] ~~**Pin a clang-format version, reformat, add a gate.**~~ **DECIDED AGAINST 2026-07-27
      (maintainer).** Not wanted as a CI gate. The checked-in `.clang-format` is a **per-file
      starting point** -- something to run on a file you are already editing if you want it -- and
      **not a target state the tree is supposed to converge to**. So the measurement that prompted
      this item (144 of 289 tracked C++ files diverge under clang-format 21.1.8) is not evidence of
      drift; it is the expected state of a config used that way, and re-measuring it will not change
      the answer.
      Recorded so it is not re-filed. The reformat would also not have been mechanical: the config
      says `IndentExternBlock: NoIndent`, `user/src/syscall_stubs.cc` obeys it and
      `kernel/init/kmain.cc` does the opposite, so gating would have restyled every `extern "C"`
      block in the kernel as a side effect of a formatting decision nobody made deliberately.
      Record finding **T9** closes with the same reasoning.
- [ ] **Add a licence-header gate.** The premise that it could wait -- "coverage is 100%, so this
      is cheap to hold" -- turned out to be false: re-measuring on 2026-07-27 found
      `docs/design-rp2350-mpu-armv8m.md` carrying no SPDX identifier while all 26 sibling design
      records did. Header added, so coverage is 534 of 536 tracked non-binary files (the rest are
      `.gitignore` and the six JSON presets, none of which can carry a comment). The drift the gate
      exists to catch had already happened unnoticed, which is the argument for adding it now.
- [ ] **Pin GitHub Actions to commit SHAs**, not moving tags -- a tag is a supply-chain seam
      controlled by someone else.
- [ ] **Wire the telemetry runtime gates into CI.** The `sim-telem` / `qemu-telem` presets exist
      and work, but no job runs them, so telemetry can rot without anything going red.
- [x] **Move `.claude/` from `.git/info/exclude` into `.gitignore`** -- DONE, `**/.claude/`.

## `kos_reboot` (reboot-to-bootloader) -- BUILT (2026-07-28)

`KOS_SYS_REBOOT = 38`, `AUTH_SYSTEM`-gated, behind an `arch_reboot` seam whose fallback
(`arch/common/arch_reboot_default.cc`) answers `-KOS_ENOSYS`. Case, fallback TU, wrapper and app are
all inside `KICKOS_ENABLE_SELFTEST`, so a
production image carries none of it. The decision to share shutdown's bit, and its counter-argument,
are recorded in `docs/design-unprivileged-root.md` section 9.

Backends: rp2040 `'UB'` -> `_reset_to_usb_boot(0, 0)`; rp2350 `'RB'` -> `reboot` with
`BOOTSEL | NO_RETURN_ON_SUCCESS`; imxrt1062 `bkpt #251` -> the MKL02 presents HalfKay. Every
other chip declines through the fallback. The earlier instruction to read `*(uint8_t*)0x13` and
branch on it is **refuted**: both datasheets forbid using that ROM build byte to locate
functions, and the three magic bytes at `0x10` are the whole validity test they give.

Witnessed: the refusal path -- selftest `reboot_priv` (an unprivileged caller gets `-KOS_EPERM`)
plus `sim_reboot_declined` / `qemu_reboot_declined` on `apps/rebootdemo`. The reboot itself is
**witnessed on RP2350** (pizero2350, see `docs/reference/boards.md`); the RP2040 and imxrt1062
backends are still bench debt (see the bench item below).

## Unprivileged ctors and `main` -- start unprivileged, holding capabilities (2026-07-27)

**Reasoning, blockers and the boards this does not work on are now filed as
`docs/design-unprivileged-root.md` (ACTIVE).** This section stays the actionable checklist.

A design investigation superseded stages 2-8 of the old plan: root should **start unprivileged
holding capabilities** rather than start privileged and demote, so there is no demotion to build.
`thread_regions_recompose`, the drop-privilege syscall, its per-arch backends and Xtensa-last are
**deleted, not deferred** -- the region set is composed once in `thread_create`
(`kernel/thread/thread.cc:94-134`) from a privilege that never changes, and the rest existed only
to manage a transition this design does not have. The reason, recorded once: **every ISA with a
ring split already encodes thread privilege in the fabricated first frame and restores it on the
first switch-in** -- armv7m `ctx.npriv` (`arch/arm/armv7m/arch_armv7m.cc:111-119`), armv6m
(`arch_armv6m.cc:102-108`), rv32imac `MSTATUS_MPP` (`arch_rv32imac.cc:143-158`), rxv3
`PSW_THREAD_USER` (`arch_rxv3.cc:279-289`). The two ports without a ring split store nothing:
xtensa (`arch_xtensa.cc:271-273`) and the sim, whose `arch_context_init` takes `privileged` and
**discards** it (`sim.cc:758-761`) -- privilege there is the thread's region set plus a
per-context mid-syscall raise counter, so the sim has no CPU-mode axis at all. Starting root
unprivileged therefore needs **zero new assembly on any port**, and Xtensa comes along free
rather than last. `drop_priv` survives only as a **contingent, much smaller** item: it is the
only mechanism giving "privileged bring-up then self-confinement for life", which is what the
blocked bring-up bodies below want -- in scope only if the `arch_periph_enable` seam (stage 3)
proves insufficient.

The old stage 1 (arena-allocated boot stacks) LANDED on the M4.5.1 branch -- see
`m4.5.1: take the root and idle stacks from the arena, not .bss`. The new stages, in dependency
order:

**Stage 0 -- independent prerequisites, no behaviour change. COMPLETE** (all three were real bugs;
each landed with its own gate, and the whole stage costs 276 B of flash and 8 B of `.bss` on
frdmk64f+blink -- the 8 B being the argv struct itself).
- [x] **Move the argv handoff out of kernel-stack storage** -- see `m4.5.1: move the init argv
      handoff off the boot stack`. `root_entry` read `argc`/`argv` from a `kmain` frame local on the
      boot stack, *outside* the arena, so an unprivileged root would fault on its first statement
      after the ctor walk on every enforcing board -- and **the sim cannot reproduce it** (`kmain`'s
      frame is host stack). Now `kickos_init_args` in `libkickos_user.a`, which every enforcement
      linker script routes into the `.appdata`/`.appbss` grant. NOT the init provider's archive: a
      build naming its own `KICKOS_INIT_PROVIDER` must not be able to remove the definition.
      Placement verified by symbol address on all five enforcement images, not assumed.
- [x] **Add `KOS_SYS_SHUTDOWN(status)`** -- see `m4.5.1: end the system through a syscall, not a
      direct kernel call`. Syscall **36**; privileged-only for now, which is exactly who can end the
      system today, and stage 1 widens it to an authority bit (`AUTH_SYSTEM` after the stage-4
      re-cut). Still the natural home for the "console bytes lost on shutdown" item above, which
      now has one owner instead of two call sites. Gate: selftest `shutdown_priv`, checked to FAIL
      (run truncates mid-suite) with the gate removed.
- [x] **Add a writable arm to `user_writable_ok`** -- see `m4.5.1: give user_writable_ok the
      static-data arm its read twin has`. New `arch_user_data_writable` seam. **The hole was wider
      than recorded here:** it is not just the five chips with no MPU *backend* (stm32f103,
      stm32f302, nrf51, sam3x8e, esp32 -- and `sam3x8e` **has** an MPU on silicon, a Cortex-M3
      revision 2.0 unit; what is missing is the `mpu.cmake` port, and the `due` unit is retired so
      it cannot be witnessed either way) -- **the host sim has it too**, despite building
      `KICKOS_HAVE_MPU=1`, because its globals live in the host image rather than the mprotect'd
      arena. So the fix could not key
      on `KICKOS_HAVE_MPU` alone and the sim carries its own arm. Gate: selftest `writable_global`,
      confirmed failing on both broken postures beforehand. Also note the suite had already
      *worked around* this bug in `ep_recv_worker`'s comment without it being filed.

**Stage 1 -- the authority capability, root still privileged. COMPLETE** (see
`m4.5.1: gate the eight authority syscalls on a capability, not only on privilege`; +374 B flash
on frdmk64f+blink, no `.bss`).
- [x] **Added a dedicated authority capability type**, seated at the already-reserved index 2 and
      carrying the **five unused bits of
      `CapEntry.rights`**: `AUTH_MEMORY` (ram_alloc + MMIO grant), `AUTH_PINMUX`, a clock bit,
      `AUTH_IRQ`, and a device bit (console publish, shutdown, and later reboot -- **not** periph
      enable, which is possession-gated; see stage 3). The stage-4 re-cut below replaced those last
      two spellings with `AUTH_PSTATE`, `AUTH_SYSTEM` and `AUTH_CONSOLE`. Zero dynamic slots on
      every board. Poolless, so it resolves by reading the reserved slot and never via
      `cap_resolve_e`; `obj_ref_inc`/`obj_ref_drop`/`obj_close_protocol` each gained an explicit
      no-op arm rather than relying on a `default:` that asserts.
      **Seated WITHOUT `CAP_TRANSFER`**, which makes it non-delegable rather than merely
      undelegated -- so index 2 has exactly one writer, the kernel, and the delegation-packing
      collision below is unreachable instead of unlikely.
      **SUPERSEDED in mechanism**: the cap type was later deleted and the word moved to
      `Thread::authority`, a byte in existing TCB padding, because it named no pool object, held
      no refcount and bumped no generation. Index 2 went back to the dynamic range and
      `KICKOS_CAP_FIRST_DYNAMIC` fell 4 -> 2. The bit cut, the gate chokepoint and the
      narrow-only rule all survive unchanged; see `reference/invariants.md`
      (`authority-word-narrows-only`).
- [x] **Converted the gates to `cap_check_authority(caller, AUTH_*)`** -- that call and nothing
      else, with the privileged-implies-everything arm inside the function, so the rule is stated
      once instead of at every site. Behaviour-neutral: root is privileged, so every
      gate takes the same arm as before. **Eight sites converted, and there turned out to be
      nine authority decisions**: `grant_region_admissible`'s DEV arm was missed here and found by
      stage 2 below. Enumerate the decisions, not the call sites -- a count that is right the day
      it is written is what lets the next one hide. `kos_thread_params` gained an `authority`
      byte (in the padding after `cap_count`, so the struct does not grow) that narrows only --
      refused together with `cap_count >= 2`, since a second delegated cap would land on the
      authority slot.
- [x] **Gate: selftest `authority_cap`.** An unprivileged child holding `AUTH_PINMUX` and nothing
      else gets PAST the pinmux gate (which then answers for itself) while being refused at a
      gate it holds no bit for -- so the bits are shown independent, not one lump. Confirmed to
      FAIL with the grant removed, which is what makes it cover the arm that would otherwise ship
      unexercised until stage 2 (the `kernel_ctor_placement` vacuity trap).

**Stage 2 -- flip per board**, behind a build-enforced posture knob (default ON,
**NOT a weak symbol**: opting out of the boundary had to be visible in the board's build, not
silently satisfied by a link-time override).
**The knob NO LONGER EXISTS -- M4.5.6 deleted it and root is now unconditionally
unprivileged.** Everything in this stage record is dated history, and no item under it describes a
posture that is still selectable.
- [x] **The knob** -- see the `m4.5.1` commit that added it and seated root's authority cap when it
      was off. **SUPERSEDED by M4.5.6, which deleted all of it**; recorded as built.
      CMake option, always emitted as `0`/`1` (so `#if` was `-Wundef`-clean), printed at
      configure time and carried to out-of-tree consumers as a usage requirement of `kickos_core`.
      OFF created root unprivileged and seated `CAP_AUTH_ALL` at the reserved authority index after
      `thread_create` (which zeroes the TCB) and before `sched::start` -- that seat is unconditional
      now. The banner reported the posture on the `mpu` line as a *concatenated literal*, so the
      default posture added no string and no runtime branch there; with one posture the literal
      became a constant and M4.5.6 removed it. Cost to the no-MPU tight boards, measured against `ed78926`
      rather than assumed: f302nucleo+selftest **-4 B** of text, bluepill-c8+selftest **+8 B**. Not
      byte-identical, as first claimed here: the `+8` is `syscall_thread.cc` calling
      `cap_check_authority` where it read `Thread::privileged`, and the `-4` is one store dropped
      from the selftest. Both still link.
- [x] **A ninth authority gate that stage 1 missed, and it blocks the first board** -- see
      `m4.5.1: gate the MMIO grant on AUTH_MEMORY, not on the caller's privilege`.
      `grant_region_admissible`'s DEV arm (`kernel/grant/grant.cc`, Choice 5A) read the caller's raw
      `Thread::privileged`. It is not in the design doc's list of surviving privilege reads, and it
      sits directly on the console-handover path: an unprivileged root holding `AUTH_MEMORY` clears
      the `syscall_thread.cc` gate and is then refused here, so the board goes dark. Now
      `caller_authorized`, resolved at both call sites as `cap_check_authority(current, AUTH_MEMORY)`.
- [x] **`xmc4800-relax` FLIPPED and witnessed on silicon.** Console-only service list added
      (`kickos_services_xmc4800relax_console`), selected automatically for the flipped posture;
      the root-MMIO refusal list makes the combined list a **configure-time `FATAL_ERROR`** in
      that posture rather than a dark board. (M4.5.6 lifted that listing: the combined list is the
      default again and the board is witnessed running it. The list itself was emptied, and deleted
      outright on 2026-08-05.)
      Evidence in `docs/reference/boards.md`. The A/B was
      **re-captured post-rebase at `22e1c5a`**, so it witnesses the rebased combination of stage 2
      with the kernel-audit batch, not just the pre-rebase branch.
- [x] **Re-witness the tip on silicon: DONE 2026-07-28 at `75227d4`.** The XMC A/B re-run plus
      the `frdmk64f` SYSMPU regression, six flash-and-capture runs, all signatures matched. The
      two enforcement-path commits the boards left the bench before (`af696e6`, `3c772b9`) plus
      the alignment-gate repair are witnessed by `mem_self_grant` and `mem_self_grant_nonpow2`
      running `ok` under PMSAv7 in both postures and under SYSMPU. Updated boundary table in
      `docs/reference/boards.md`; captures under `.session/n33-rewitness/` (machine-local).
- [x] **`esp32c6-wroom`, `pizero2350` and `rx72m` FLIPPED and witnessed on silicon**, which puts
      the flip across all four enforcement backends (PMSAv7, RISC-V PMP, PMSAv8, RXv3). Evidence
      per board in `docs/reference/boards.md`. `rx72m` needed two prerequisites first: `rxdrv`
      moved onto the fleet driver pattern, and an `arch_pinmux_set` backend covering `PmnPFS` plus
      `PORTm.PMR`.
- [x] **`f411disco` FLIPPED and witnessed on silicon 2026-07-29 at `6646c8e`**, which closes the
      declared stage-2 set. What blocked it was a **pre-existing bench debt rather than flip work**:
      PMSAv7 had never been witnessed on that board at all, so a flip would have had no enforcing
      baseline to discriminate against. Done in two passes for that reason -- enforcement first in
      the default posture (selftest 62/62 with 0 skips, plus an `mpu_fault` cross-domain MemManage
      denial, `CFSR=0x82`, `MMFAR=0x2000b000`), then the A/B. The backend is the shared `stm32f411`
      one, so this also closes the MPU HW debt for the chip and for `blackpill`; `docs/m2-readiness.md`
      no longer carries an unwitnessed enforcement backend. Evidence in `docs/reference/boards.md`.
      Found on the way, NOT fixed: `f411spi` does its SPI1 bring-up (RCC/GPIOA/GPIOE) from `main`,
      so under the flip it faults MemManage on the first store (`RCC_AHB1ENR` @ `0x40023830`,
      witnessed). It is a diagnostic app and not on the gate, so it is stage-3 follow-up
      (`arch_periph_enable`), the same treatment `c6blink` and `rxdrv` needed.
- [x] **`frdmk64f` FLIPPED and witnessed on silicon at `127efb5`, on stage 3 rather than stage 2.**
      Its `k64uart` and `k64dspi` PACR writers (`AIPS0_PACRN` at `0x4000_0064`, `AIPS0_PACRF` at
      `0x4000_0044`) both fall inside `arch_reserved_blocks`'s AIPS0 entry
      `[0x4000_0000, 0x4000_1000)`, so no grant can ever reach them and `arch_periph_enable` was the
      only way in. **The first board to run its FULL service list under the flip** (console
      `k64uart` + SPI `k64dspi`), where every other flipped board is console-only or serviceless:
      `selftest` `1..65` `# all tests passed (2 skipped)`, and `rootfault` denies root's cross-domain
      write (`SYSMPU ISOLATION FAULT: port=3 addr=0x2001a000 master=0 W EDR=0x80000003`,
      `CFSR=0x400`, `HFSR=0x40000000`). Skips were `mpu_privileged_guard` (posture) and
      `mutex_deadlock # SKIP pool too small` (pre-existing, `docs/reference/boards.md`). Root writes
      no MMIO at all on this board now. Evidence in `docs/reference/boards.md`.
      **The skip count is now 1, not 2**: M4.5.6 retired `mpu_privileged_guard`, re-witnessed on this
      same board and same full service list on 2026-07-30 (`# skipped: 1`).
- [x] **Per-board gate, and what it actually cost.** Both halves met on `xmc4800-relax` silicon
      under PMSAv7. But the gate as worded is not reachable by the *unmodified* suite, and the
      reasons are worth keeping:
      - The cross-domain half needed a **new** app. `apps/mpu_fault` confines a spawned CHILD and
        says nothing about root; nothing covered root, the thread that runs the ctors, the bring-up
        and `main`. `apps/rootfault` does, and it was discriminating in both postures (privileged root
        completed the write and said so), so the fault is evidence rather than a symptom. With one
        posture left the discriminating pair is gone, but the app is now stronger where it counts: it
        is an ALWAYS-BUILT gate on six images as of M4.5.6, having previously registered in none.
      - The selftest half cost **2 skips** when this was written, named on the wire
        (`irq_as_event`, `mpu_privileged_guard`), so a flipped image did **not** satisfy the
        selftest gate. Both halves are now closed, and the split is worth keeping because the two
        skips had different causes. `irq_as_event` was a missing capability, not a posture cost:
        `kos_mem_self_grant` lets root ask for the page it allocated, so it **runs** in both
        postures. Only `mpu_privileged_guard` was genuinely posture-driven -- its subject was the
        privileged posture. The gate now takes an expected-skip list **by name**
        (`EXPECT_SKIPS`) instead of a count, since a budget of 2 would have admitted any 2 skips
        rather than these 2. Measured after both: flipped `sim` skipped exactly
        `mpu_privileged_guard`, 59/60 run.
        **`mpu_privileged_guard` is RETIRED as of M4.5.6**, so neither skip is posture-driven any
        more and the `EXPECT_SKIPS` list has no posture-dependent entry left. Nothing above changes
        as a record; it just no longer describes a live cost.
- [x] **`kos_ram_alloc` grants its caller nothing, which left it near-useless to an unprivileged
      root.** Allocation and grant are separate acts: a region becomes reachable by being handed to
      a spawn, so an `AUTH_MEMORY` holder could allocate a page and then not touch it. A privileged
      root never noticed. This was the root cause of BOTH selftest skips and of the `mpu_fault`
      restructure below -- a gap in the capability story rather than three test defects.
      **Decided: an explicit `kos_mem_self_grant` (37), not an implicit grant at alloc.** Alloc must
      stay usable for the allocate-then-hand-off pattern that spends no region, and the caller's
      region table is a hard, small budget (`KICKOS_MPU_MAX_REGIONS`), so spending a slot has to be
      something the caller *asks* for. It is gated on `AUTH_MEMORY`, bounded by that budget, and
      fails loudly with `-KOS_ENOMEM` rather than silently not enforcing -- proved both ways in
      `t_selfgrant` (returning `-KOS_EPERM` fails the test; accepting silently faults the worker).

**Stage 3 -- the blocked bring-up bodies. COMPLETE** (2026-07-29, `127efb5`).
- [x] **Added `arch_periph_enable(uintptr_t base)`**, `-KOS_ENOSYS` fallback (then in
      `kernel/time/clock_select.cc`, now `arch/common/arch_periph_enable_default.cc`), covering "ungate the clock and drop the bus-side
      supervisor-protect for the block at this base". Syscall `KOS_SYS_PERIPH_ENABLE = 39`, wrapper
      `kos_periph_enable`. Backends: mk64f (UART0, DSPI0) and stm32f411 (SPI1, clock gate only --
      that bus exposes no privilege-classification register in this tree). ESP32-C6 and RX72M
      deliberately have none: their windows need nothing from the seam, the C6's APM open being a
      boot-time act in `arch_init`. Each backend is a hand-curated table keyed on the **exact** block
      base, never a range, and both writes are DERIVED from `base`, so no caller can name a shared
      block's register or the bit inside it. Retires `k64uart` and `k64dspi`'s root MMIO entirely, so
      `kickos_services_frdmk64f` came off the root-MMIO refusal list.
- [x] **NOT gated on a device authority bit, which is what this checklist previously said. The
      gate is possession.** `caller_holds_mmio_block(base)` (`kernel/syscall/syscall_mem.cc`)
      requires a live `ARCH_MPU_DEV` region whose base matches exactly; privileged callers bypass,
      as in `cap_check_authority`. Rationale in full, including why not `user_range_ok`, why exact
      base rather than containment, and what an authority bit would have handed every unprivileged
      bus driver: `docs/design-unprivileged-root.md` sections 7 and 9.
- [x] **The call site is the DRIVER, not root**, which is what makes the seam's bound a fact rather
      than an aspiration: root holds no DEV region on any board (`ARCH_MPU_DEV` is attached only in
      `domain_for`, reached with MMIO only from `thread_create_call`, and `KOS_SYS_MEM_SELF_GRANT`
      hardcodes `R|W`), so a holder of one window can only ever ask the kernel to configure that
      window's device.
- [x] **No PIT entry on K64F, refused by design.** One AIPS `PACR` slot covers a whole 4 KiB block,
      so opening slot 55 for the legitimately granted PIT ch2 window (`0x40037120`) would also expose
      the chained ch0+ch1 pair carrying `arch_clock_now` -- the registers `arch_reserved_blocks`
      protects by address (`{PIT_BASE, 0x120}`). That base answers `-KOS_EINVAL`. An entry exists
      only where the bus gate's granularity is CONTAINED by the block the window covers. The
      system-wide reach of an opened slot is the already-documented hardware ceiling
      (`docs/book/peripheral-isolation-and-the-hardware-ceiling.md`,
      `docs/reference/architecture.md`, `docs/design-m3-console-handover-stageii.md`), not new.
- [x] **New `arch/arm/chip/mk64f/regs/aips.h`** gives the three formerly open-coded slot -> (`PACR`
      register, `SP` bit) derivations one home, with `static_assert`s pinning slots 106 (UART0), 44
      (DSPI0) and 55 (PIT, derivation coverage only, no entry). It caught that `PACR` offsets are
      **not** contiguous: groups 0..3 at `0x20`..`0x2C`, `0x30`..`0x3C` reserved, groups 4..15 at
      `0x40`..`0x6C`, so a naive `0x20 + group * 4` names the wrong register for slot 44.
- [x] **`stm32f411` pinmux gained an encoding field.** `func` bit 8 `PINMUX_OUT_HIGH` presets an
      output high; a non-output mode carrying the bit is refused `-KOS_EINVAL`. `BSRR` is written
      BEFORE `MODER` (proven in disassembly: `str [r3,#24]` precedes `str [r3,#0]`), because a `BSRR`
      set on a still-input pin is inert while the reverse order asserts the `ODR` reset level first.
      `f411spi` needs it to hold the onboard gyro's `PE3` chip-select deasserted so the gyro's SDO
      stays tri-stated.
- [x] **Gate: selftest `periph_enable_unheld`** (`ok 47`), the negative arm. The POSITIVE arm has no
      in-env carrier at all -- the host sim can never hold a DEV region, `arch_mpu_region_encodable`
      returning false unconditionally (`arch/sim/sim.cc`) -- so it is witnessed by driver bring-up on
      silicon instead, and by the two-arm possession probes in `c6blink` (ESP32-C6, PMP NAPOT) and
      `rxdrv` (RX72M, RXv3), each negative in `main` and positive as the driver's first act, both
      printing rc and want. **Both were run on silicon in M4.5.6** -- see the `c6blink` and
      `rxdrv` items below in this file.

**Stage 4 -- the app story. COMPLETE** (three commits: the delegation type guard, the re-cut plus
`kos_cap_narrow`, then the narrow site plus the per-app declarations).
- [x] **Re-cut the authority set into SIX bits** -- `AUTH_MEMORY`, `AUTH_PINMUX`, `AUTH_PSTATE`,
      `AUTH_IRQ`, `AUTH_SYSTEM`, `AUTH_CONSOLE` -- **and delete the old device and clock bits as
      names.** Why each bit is separate, and why `AUTH_CONSOLE` cannot be possession-gated the way
      `arch_periph_enable` is: `docs/design-unprivileged-root.md` section 5. The ungated
      `KOS_SYS_ENDPOINT_CREATE` and the missing `cap_console_publish` owner check that force that
      last point are filed below under the M4.5.3 findings.
- [x] **Funded the sixth bit by moving the authority word out of `CapEntry.rights` into the poolless
      `obj` field.** `CapEntry` stays 8 bytes; `rights` is 0 on such an entry. The two families now
      have separate enums (`CapRights` bits 0..2, `CapAuthority` bits 0..5) and separate numbering,
      which costs one thing worth recording: an object right is no longer a *distinguishable* wrong
      value in an authority mask, so the "non-authority bits" spawn refusal catches only bits above
      the six, and the selftest's bad-bits probe moved to `1 << 6`.
- [x] **The delegation type guard landed FIRST, as its own commit.**
      `kernel/syscall/syscall_thread.cc` copies `se->obj` *and* `se->type` verbatim, so with the word
      in `obj` a delegable authority cap would forge a seat at child index `ci+1` -- index 2 whenever
      `cap_count >= 2`. Refused by TYPE, deliberately not by rights, so it does not rest on the byte
      the same change repurposes. Behaviour-neutral on landing (`cap_seat_authority` masks to the
      authority bits, so such a cap never carried `CAP_TRANSFER`).
- [x] **Added `kos_cap_narrow(cap, mask)`** -- `KOS_SYS_CAP_NARROW = 40`, ungated (an authority
      needed to drop authorities could never be given up), refuses any handle that does not name
      the authority word with
      `-KOS_EINVAL` because narrowing an endpoint cap's `CAP_WAIT` needs `obj_close_protocol`'s
      `recv_holders` accounting. Narrowing to 0 leaves nothing to give up; a second narrow is
      `-KOS_EBADF`.
- [x] **`kickos_default_init_run` narrows root after bring-up**, so the pin map and the console
      publish still have their bits. It lives in the RUN BODY, not the entry, because `init.h`
      advertises that body as the delegation reuse point: a custom `KICKOS_INIT_PROVIDER` composing
      pinmux + service list + run body would otherwise have run the app with root's full authority,
      silently. Found by the review pass, not by a test. The mask comes from
      `kickos_app_authority()` (default `AUTH_MEMORY | AUTH_SYSTEM`), overridden per app by
      `KICKOS_APP_AUTHORITY` in the app's own TU. **Per app, not per build tree**: one tree links
      every app against one kernel, so no CMake variable can express it. **NO weak symbol** -- the
      fallback is alone in `system/init/common/app_authority_default.cc`, so an app that defines the symbol
      resolves it locally and that member is never extracted. Weak was tried first and rejected: GCC
      carries a weak attribute from a declaration onto the definition in the same TU, so every app's
      override compiled `W` and link order decided it (`nm` confirmed). The macro emits the
      `extern "C"` itself, because a bare definition in a C++ app TU would mangle and leave the app
      silently on the default mask. Declared: `selftest` (five bits, not `AUTH_PSTATE`),
      `initdemo` (`CONSOLE`), `clockretune` (`PSTATE`), `c6blink`/`rxdrv`/`f411spi` (`PINMUX`).
      `AUTH_SYSTEM` is **not** forced back on: `kmain`'s refused shutdown panics
      `"root: shutdown refused"`, so the mistake is already legible, and forcing it would deny a
      never-returning app the ability to hold nothing.
- [x] **`stress` is NOT privileged-root: its three spawns were the same leftover.** Nothing in
      `ping`/`pong`/`churner` touches a privileged or authority-gated path (semaphore ops and
      `kos_sleep_ns` are ungated; the globals are ordinary `.appdata`), none of them spawns anything,
      and `sleeper` -- unprivileged in that same app, same round -- already does a superset of
      `churner`. Same origin commit as `selftest`'s two. Now unprivileged, and this **fixed a real
      failure**: with root UNPRIVILEGED the old flags were refused `-KOS_EPERM`, so
      `sim_stress` FAILED there on master and passes now.
- [x] **Gate: the selftest `authority_cap` narrow arms** -- the worker drops its only authority and
      the gate that had just answered for it returns `-KOS_EPERM`, plus the non-authority-cap
      refusal. In-env on every target, sim included. The ROOT narrow has no in-env carrier, so it was
      witnessed by deleting `AUTH_CONSOLE` from `initdemo`'s declaration: `console_publish` then failed
      from root on `qemu` with root UNPRIVILEGED, and the identical source passed with root
      privileged.
      That pair is also what proves the per-app override actually overrides. **Not re-runnable as
      described**: M4.5.6 deleted the knob, so the `ON` half of that A/B no longer builds; `rootauth`
      below is the carrier that replaced it.

Opened by stage 4:
- [x] **The ROOT narrow has an automated test: `user/apps/common/rootauth`** (M4.5.5). A diagnostic
      app declaring `AUTH_MEMORY | AUTH_SYSTEM | AUTH_PINMUX`. As written for M4.5.5 it registered in
      BOTH postures and discriminated on identical source via an `#if` on the posture knob -- four arms
      flipped, three privileged. **M4.5.6 deleted that condition with the knob**: there is one posture,
      so the app is now five arms on one path, and it gained a `microbit` gate.
      The arm that matters is still the first -- it asserts `pinmux_set` is NOT `-KOS_EPERM`,
      and `AUTH_PINMUX` is a bit the fallback mask does **not** carry, so **a silently-ignored
      `KICKOS_APP_AUTHORITY` override now fails a gate**. Asserting some MISSING bit is refused would
      not have done it: the fallback lacks those too, so that arm passes with the declaration
      ignored. Verified by mutation -- neutralising the declaration turns arm 1 red while the other
      arms stay green. Gate set as of M4.5.6, with the posture condition gone: the sim, the QEMU MPS2
      boards, `qemu-riscv`, and the new `microbit_rootauth`, which is the first no-ring carrier.
      `PASS` on `frdmk64f` silicon 2026-07-30.
- [x] **CLOSED BY DELETING THE MECHANISM: the authority delegation refusal had zero test coverage
      and could not easily get any.** `syscall_thread.cc` refused the authority cap type ahead of the
      `CAP_TRANSFER` check, but such a cap always carried `rights == 0`, so the older check refused
      the same delegation with the same `-KOS_EPERM`. The two were indistinguishable from userspace,
      which is why no black-box test could pin the guard, and why it was exactly the kind of line a
      later reader finds redundant and deletes. Moving the authority word into `Thread::authority`
      removed the entry the delegation loop would have copied, so non-delegability is now structural:
      there is nothing to refuse and nothing to test. **The general lesson worth keeping**: an
      untestable defense-in-depth guard is a signal to look for a shape in which the hazard cannot
      arise, not a prompt to build a kernel-side unit hook to pin the guard.
- [x] **The sim selftest gate runs an unprivileged root** (M4.5.5). The TAP verdict logic moved out
      of `tests/integration/check_qemu_selftest.sh` into `tests/integration/check_tap_stream.sh` (stdin), with
      `check_sim_selftest.sh` as the sim runner, so the sim permits a skip BY NAME through the same
      `EXPECT_SKIPS` list rather than by a count regex. `KICKOS_HAVE_MPU` is 1 by arch on the sim, so
      the existing `KICKOS_EXPECT_SKIPS` condition already covered the flipped sim -- only the
      plumbing was missing. Measured: `sim` at `ROOT_PRIVILEGED=OFF` was 12/13, now 14/14
      (`rootauth` and `rootfault` both register there). The checker's arms were verified individually
      against crafted streams, including the anti-vacuity count/name cross-check.
- [x] **DECIDED: `Debug` is not a supported configuration on the 64 KiB boards** (M4.5.5). It is not
      an `f302nucleo` regression and not stage 4's -- it is the whole 64 KiB class. Re-measured at
      `c5d9b0d`: `f302nucleo-st` overflows FLASH by **7,920 B** and `bluepill-c8-st` by **7,692 B**,
      `selftest` being the ONLY image that fails to link on either board, while the same
      `f302nucleo-st` image at `-Os` has **10,592 B free** (54,944 B of 65,536). Those figures read
      5,120 / 4,884 / 12.9 KiB in M4.5.5 and move with every milestone that grows the suite, so
      re-measure rather than quoting them.
      Debuggability is not what is being given up: `MinSizeRel` carries `-g` (`CMakeLists.txt`), so
      those boards keep full symbols and lose only `-O0`. **No gate is added** -- a gate for an
      unsupported configuration is noise, and the existing failure is already loud and names the
      overflow in bytes at link. Recorded in `docs/reference/porting.md`.
      This is also why four comments in `user/apps/common/selftest/main.cc` claiming "96 bytes free" /
      "at the f302nucleo ceiling" were removed rather than updated in 4.5.4: they were measured under
      the superseded `-O0` default and justified keeping `.rodata` down in a configuration with ample
      room.
- [ ] **A per-service authority declaration in `kos_service_cfg`.** The struct has `rsv[4]`, so a
      byte fits with no layout change, and the runner could then narrow *between* entries -- hold
      `AUTH_CONSOLE` only while the `KOS_SVC_CONSOLE` entry runs. Deliberately NOT done in stage 4:
      root holds `CAP_AUTH_ALL` for the whole list run either way, so the only window it closes is
      between one bring-up entry and the next, with no app code running, and the app-level narrow
      already strips the bit before `main`. It becomes worth doing when service bring-up moves off
      the root thread -- i.e. with the item directly below.

Carried over from the old plan, untouched by this design:
- [ ] **Move app bring-up into the service lists**, so an app is started the way a driver is.

Blockers and limits:
- **One service bring-up body used to poke MMIO directly from root -- CLOSED by M4.5.6's
  `arch_periph_reg_write` seam, and `xmc_spi0_start` now contains zero register access.** The
  consequence is discharged too: `kickos_services_xmc4800relax` came OFF the root-MMIO refusal
  list (emptied then, deleted outright since), `xmc4800-relax` defaults to its full service
  list under enforcement, and `xmcssc` AS A SERVICE is WITNESSED -- both driver banners on the wire
  at `commit 270b6fa`, no dark board. Recorded in full under M4.5.6. The analysis below is kept
  because it is what the seam had to satisfy.
  `system/driver/xmc4800/xmcssc/spi_usic.cc` (USIC kernel clock, baud, protocol) -- on
  `xmc4800-relax`, the enforcement flagship. The two K64F bodies were **retired by stage 3**:
  `system/driver/mk64f/k64uart/k64uart.cc` (AIPS PACR) and
  `system/driver/mk64f/k64dspi/k64dspi.cc` (clock gates, pin mux, GPIO, DSPI config) each call
  `arch_periph_enable` from the driver thread that holds the window. Stage 3 does **not** cover the
  XMC, which needs USIC-specific FDR/BRG/CCR programming rather than
  "ungate a clock, drop supervisor-protect", so `xmc4800-relax` stays console-only under the flip.
  **The XMC blocker is hardware, measured on silicon
  2026-07-28** by `user/apps/xmc4800-relax/pvprobe`: an unprivileged thread holding the MPU grant
  for the U0C1 window (`0x4003_0200`) has its writes to FDR/BRG/CCR **silently discarded** (no
  fault, read-back unchanged), while `SCTR` (`U,PV`) in the same window in the same run lands
  exactly and an ungranted SCU poke MemManages. So the window is grantable and the *transfer* path
  works unprivileged -- `xmcssc` already proves that -- but `xmc_spi0_start`'s three PV-write-only
  stores need a privileged executor, and the flip needs a seam for them. Given that seam the
  bring-up moves **wholesale** into the granted driver and must, because no path exists by which a
  post-flip root holds a DEV region: `ARCH_MPU_DEV` is attached only in `domain_for`, reached with
  MMIO only from `thread_create_call`, and `KOS_SYS_MEM_SELF_GRANT` hardcodes `ARCH_MPU_R | ARCH_MPU_W`.
  So the driver is the only possible caller of the seam. The
  earlier entry here said the opposite ("contradicted, not untested", from `consoledemo`'s scrambler
  garbling the UART); that was **invalid inference** -- the scrambler also writes SCTR/TCSR/PCR (`U,PV`) and
  gates `KSCFG`, any one of which garbles the UART on its own. Also corrected: Table 18-20 marks
  exactly three registers `Write = PV` (FDR, BRG, CCR); `INPR` is `U,PV` and its earlier inclusion
  was a transcription slip (untested here). It was **enforced
  at configure time** (the root-MMIO service-list refusal) rather than left to fail on the hardware,
  because the runtime failure is a fault mid-bring-up *after* the console has been relinquished, i.e.
  a silent dark board. **The condition is now `KICKOS_HAVE_MPU`, not the posture** -- M4.5.6 deleted
  the posture, and the substitution is the right one on its own merits: with the MPU off an
  unprivileged root DOES reach MMIO (measured), so the gate's subject is enforcement. The XMC listing
  is lifted, so the enforcement image links AND calls `xmcssc`; the empty list and its gate remain for
  the next board whose bring-up writes MMIO from root.
- **A published console drops `kos_print`, and there is a real dark window** -- narrower than this
  entry used to claim, and the narrowing is the point. What drops is the KERNEL bring-up path:
  `kos_print` / `kos_kconsole_write` hand bytes to the kernel console, and `console_emit` drops all of
  them once the UART is `USER_OWNED` (RTT still carries them where it is built in). **`printf` and
  `std::cout` do NOT drop.** THREE publish-aware writers exist and are kept in step
  (`user/include/kickos/sys/emit.h:11-12`): `kickos::emit()`, `tests/tap/tap.cc`'s `emit()`, and libc
  `_write` (`user/src/newlib_stubs.cc:19-59`, which tries `kos_send` on cap 0 and falls back for the
  remainder). So a user app on the standard APIs reaches a published console driver, and this is NOT a
  reason to hold USB CDC or anything else. Corroborated on silicon 2026-07-30: `frdmk64f` on its
  published full service list carries `# tap route: stdout endpoint -> console driver (service list
  published)` and still delivers its whole selftest, against `pizero2350`'s
  `# tap route: kernel debug console (stdout not published)`.
  Two real things remain. A freestanding app that uses `kos_print` instead of the publish-aware writer
  loses its output -- found on silicon: the first `rootfault` capture held a fault dump with nothing to
  check it against, and `mpu_fault`'s captures had been marker-only on every service-list board, both
  fixed via `kickos::emit`. The open question of which other worker-printing diagnostics share it is
  **answered: `pvprobe` and `inprstorm` do** -- filed below. And there is a genuine DARK WINDOW between
  the publish and the driver actually serving cap 0 (`k64uart.cc:209`, `xmcuart.cc:179`), which is
  ordering rather than a writer choice and is owned by M4.6.1.
- **The panic-path UART reclaim clips bytes in flight.** `kpanic_enter` takes the UART back from the
  userspace driver so the report always reaches the wire, which works, but on `xmc4800-relax` it
  reproducibly garbles roughly the last 8 bytes the driver had queued (the polled TX word pending in
  `TBUF0`). Cosmetic for a terminal report, but it eats the tail of the line preceding the dump.
- **`bluepill-c8` and `f302nucleo` were held by the absence of a RING-ARM witness, not by RAM or
  handles -- and `f302nucleo` has now TAKEN it.** `ringpriv` reports `PASS (5 arms)` there on real
  no-MPU armv7m silicon (M4.5.6, `commit 270b6fa-dirty`); `hello` and `stress` also pass at 2 KiB of
  heap (`docs/reference/boards.md`). `bluepill-c8` remains unwitnessed on the ring arm, but the prober
  now exists, so what it needs is a board and not code.
  Both are armv7m, so the flip's mechanism (`ctx.npriv` in the fabricated first frame) is present;
  neither part has an MPU (`stm32f103` none, and `f302nucleo` is the R8 `x8` line, which has none
  either), so stage 2's gate -- selftest green *under enforcement* plus a cross-domain `rootfault`
  -- cannot be met on either. The tiny boards' cap-table provisioning costs the flip nothing: the
  authority word is TCB state and spends no slot at all. The arena is heap policy, not the part:
  measured 6,560 B (`bluepill-c8`, production image), 2,592 B (its selftest image), 14,752 B with
  the heap carve at zero; 8,512 and 4,512 on `f302nucleo` since its carve went to 2 K
  (`docs/archive/M4.5_footprint_meas.md` section 7, `docs/reference/porting.md`
  minimum-requirement). The "barely 3 KiB" reading is a selftest-image figure.
- **`Thread::privileged` survives**, with narrowed meaning: it selects the memory posture (kernel
  domain + permissive background), it is the confused-deputy bypass at `syscall_mem.cc:37`, and it
  stays the home for "may spawn a privileged child" -- which should NOT be a capability, since
  holding it is equivalent to holding everything forever. Consequence: on a root-unprivileged
  board, **no privileged thread can come into existence after boot**.
- **`idle` stays privileged and holds no capabilities** -- it runs no app code, and RXv3 `WAIT` is a
  privileged instruction while RISC-V U-mode `WFI` is optional per spec.
- **The reserved cap index range was to be full after this** (0 stdout, 1 clock, 2 authority,
  3 spare -- reboot shares shutdown's bit, so index 3 stays free). **It did not end there**: the
  authority cap type was deleted and the word moved to `Thread::authority`, so the reserved range
  is `KOS_CAP_STDOUT = 0` and `KOS_CAP_CLOCK = 1` only, with `KICKOS_CAP_FIRST_DYNAMIC` at 2
  (`system/include/kickos/sys/cap_index.h`, `cmake/cap_geometry.cmake`); see the `kos_reboot`
  bullet above and the SUPERSEDED-in-mechanism note below. **The five-bit authority ceiling is
  gone**: the width is bounded by `kos_thread_params::authority` (a `uint8_t` in padding) rather
  than by a capability entry. Two more authorities cost nothing; a ninth needs that params field
  widened.
- **Delegation packing collides with reserved names** -- spawn delegation puts cap *i* at child
  index *i+1*, so a delegated cap lands at index 1 (`KOS_CAP_CLOCK`) and a second at index 2. The
  authority cap can no longer be the one that collides (refused by type at the delegation site), but
  the `KOS_CAP_CLOCK` aliasing still blocks the narrowed hand-off to a driver manager until the
  deferred explicit-destination-index work lands.
- **Cap-gen is a `uint16_t`** with no object generation behind a poolless cap, so 65536
  close/re-seat cycles wrap it. Unreachable in-tree; same unbounded-counter class as the
  domain-refcount item above.

## M4.5.5 -- MPU region-encoding classes

Ordered after stage 4 (`kos_cap_narrow`) and before the posture-knob deletion, but
**not a blocker for it** -- the knob went away on the strength of the flip, not of region shaping,
and it is gone as of M4.5.6. One general fleet re-witness pass closes the step; it comes due with
the rest of the bench debt under M4.6.3..N.

- [x] **DONE: the alloc/MPU seam has a third region-encoding mode.** `arch_ram_region_size`
      (`arch/include/kickos/arch/arch.h:263`) and `arch_ram_region_align` (`:302`) now branch on a
      new `arch_mpu_region_pow2()` seam (1 = pow2 size + natural alignment, 2 = any granule
      multiple), with the `arch_mpu_min_region()` floor applied BEFORE the mode branch so a
      pow2 backend is bit-for-bit what it was. `pow2()` is 0 on PMSAv8, SYSMPU and RX RXv3;
      1 on PMSAv7 and PMP NAPOT; unread where the granule is 0.
      **Sizes do not move fleet-wide** -- every board's boot stacks are already powers of two, so
      the entire recovery is alignment run-up: `idle 2048/2048 -> 2048/32` and
      `root 8192/8192 -> 8192/32` on `frdmk64f`, `pizero2350` and `qemu-m33`, `/16` on `rx72m`.
      Two things the original plan below got wrong, both worth keeping:
      **(a) the kernel could NOT just call `arch_mpu_region_encodable`.** That is the MMIO test and
      the sim returns false unconditionally (mprotect governs only the arena), so pointing it at RAM
      refuses every RAM grant, stack grant and self-grant there -- measured, 3 sim tests red. RAM got
      its own `arch_ram_region_admissible` derived from the two seam values; it reproduces every
      backend's `encodable` except the sim, where it is the one that is right for RAM.
      **(b) the RAM path never checked `size >= granule`**, only alignment, while the DEV path did via
      `encodable`. All three production callers pass a pre-rounded size so no production caller could
      reach it, but the predicate now closes it.
      Also: `cmake/boot_arena.cmake` mirrors the seam in CMake by SCRAPING the `return <int>;`
      literal, and its `GLOB` attributed `arch/arm/common/arch_arm_pmsav8.cc` to the arch family
      though the build compiles it into the CHIP library -- which is why that file's deliberate
      non-override of `arch_mpu_min_region` was load-bearing. It now reads the `SOURCES` property of
      `kickos_arch_*`/`kickos_chip_*`. The old regex also read `return 32 + 0;` as `32`; a trailing
      `;` is now required, and the match is anchored on a non-identifier boundary.
      Witness: selftest `region_mode` reports the observed shaping per board (96 B reserved for a
      3-granule request on PMSAv8, 12288 on the sim, 32 on PMP NAPOT). Gates as counted at the time:
      sim 13/13, sim flipped 14/14, `qemu` 11/11, `qemu-m3` 9/9, `qemu-m7` 10/10, `qemu-m33` 10/10
      (12/12 under enforce), `qemu-riscv` 8/8, `microbit` 5/5, plus the flipped arms `qemu` 14/14 and
      `qemu-m33` 13/13. Those flipped arms no longer exist (M4.5.6 deleted them); for the current
      fleet tally see the M4.5.6 gate count below.
      **What `region_mode` does and does NOT pin.** Its first shape asserted only
      `step == 3g or step == 4g`, with both the allocator and the expectation derived from the same
      `arch_mpu_region_pow2()` call -- a tautology that passed under either shaping. It now compares
      the observed step against `KICKOS_MPU_MIN_REGION_CFG` / `KICKOS_MPU_REGION_POW2_CFG`, the two
      literals `cmake/boot_arena.cmake` scraped at configure time, so it catches the allocator's
      shaping diverging from the declared mode, and scrape-vs-link resolution divergence (a
      definition behind an `#if` the textual scrape cannot see; the weak/strong precedence the scrape
      used to reimplement is gone with M4.5.7). Demonstrated to
      bite: disabling the granular branch turns it `not ok`, where the disjunction stayed green.
      It **cannot** catch a wrong literal in a backend: CMake scrapes the same `.cc` the link
      resolves, so flipping `chip_mk64f.cc`'s `return 0;` moves the macro and the runtime together.
      Nothing in-tree can catch that class -- only silicon can. The macros are unset on the sim (no
      `KICKOS_CHIP`, no linker script), where the weaker self-consistency check remains.
      **SILICON: two of the three moved boards, 2026-07-30.** `frdmk64f` (SYSMPU) and `pizero2350`
      (PMSAv8) both report `GRANULE-MULTIPLE (granule 32, ... reserved 96)` with `selftest` 66/66,
      against `xmc4800-relax` as the PMSAv7 control still at `POWER-OF-TWO ... 128`. The fault pairs
      are the part emulation cannot give: a granular base is only granule-aligned, so the enforced
      boundary lands off a non-round address, and both hit it exactly -- SYSMPU
      `addr=0x2001a140`, PMSAv8 a PRECISE `CFSR=0x82` / `MMFAR=0x20026020`. Full record in
      `docs/reference/boards.md`, *M4.5.5*. **`rx72m` (RX MPU, 16-byte granule) is the third moved
      board and was not available** -- it owes ONE visit covering this and the stage-4 witness.
      The original analysis, kept because it names the three hardware classes:
      `min == 0` gives 16-byte granularity, and any nonzero `min` gives a power-of-two size with
      the base NATURALLY ALIGNED to that size. The hardware has THREE classes, so one of them has
      no representation:
      - **Power-of-two REQUIRED.** PMSAv7 (`stm32f411`, `xmc4800`, `mps2`), and RISC-V PMP NAPOT
        (`arch/riscv/rv32imac/arch_rv32imac.cc:332` returns 8, "RISC-V PMP NAPOT minimum region
        size"). NAPOT folds the size into the trailing address bits, so pow2 there is the
        encoding itself, not a convention.
      - **Granular at N, power-of-two NOT required.** NXP SYSMPU 32 B (`mk64f`), RX MPU 16 B
        (`rx72m`), and ARM PMSAv8 32 B (`rp2350`, and `mps2-an505` via `qemu-m33`), which is
        base/limit rather than base+size. **This is the unrepresentable class**, and it
        over-aligns today on `frdmk64f`, `rx72m` and `pizero2350`.
      - **No MPU.** `arch/arm/chip/nrf51/chip_nrf51.cc:110` overrides to 0. `stm32f103` and
        `stm32f302` override to 0 as well (`6d49e14`), so this item covers only the class that
        remains: a granule that is right while the MODE is wrong.
      **For PMSAv8 the `min_region` VALUE of 32 is correct; the MODE is wrong.** 32 is the PMSAv8
      granule, which is why `arch/arm/common/arch_arm_pmsav8.cc` deliberately leaves
      `arch_mpu_min_region` to its fallback's 32 and defines encodability alone -- that member being
      anchored by the `kickos_arm_pmsav8_init` call from `chip_rp2350.cc`. `mk64f` is the same shape:
      it defines
      `arch_mpu_region_encodable` (`arch/arm/chip/mk64f/chip_mk64f.cc:559`) and nothing on the
      size/align path, so it still gets power-of-two shaping. That distinction is what makes this
      ONE seam change rather than a set of per-chip patches.
      **This is a known trade made explicit, not a newly found bug.**
      `arch/rx/rxv3/arch_rxv3.cc:653` already records the pow2 shaping as "a describable superset,
      not a requirement", and `arch_ram_region_size` already carries a `SEAM (MMU era)` marker
      naming itself the SINGLE point that couples allocation size to MPU descriptor geometry. The
      third mode belongs at that marker.
      **Scope and risk.** The change alters the region descriptors actually programmed on
      `frdmk64f`, `rx72m` and `pizero2350`. Two of those (`rx72m`, `pizero2350`) are flipped to
      unprivileged root AND silicon-witnessed, so it requires a silicon re-witness. Review it
      against `arch_mpu_region_encodable` and the real descriptor programming -- PMSAv8
      `MPU_RBAR`/`MPU_RLAR`, SYSMPU `RGD`, RX `RSPAGEn`/`REPAGEn` -- not only the allocator.
      `qemu-m33` (PMSAv8) is the one in-env gate the change moves.
      **The motivation is not bytes.** All three affected boards have RAM to spare (262 K on
      `frdmk64f`, 512 K on each of `rx72m` and `pizero2350`), so the expected recovery is small in
      absolute terms. What it buys is correct hardware modelling, and the parts ahead --
      Cortex-M23/M33/M55/M85 are all PMSAv8.
- [ ] **One general fleet re-witness pass, and it closes the step.** Every silicon witness in
      `docs/reference/boards.md` was captured from an UNOPTIMISED binary: the MCU presets built
      `CMAKE_BUILD_TYPE=Debug`, which is `-g` with no `-O` flag at all, and the fleet now builds
      `MinSizeRel` (`-Os -g`), which moves each image by roughly 17 to 23 KB. Per-board figures in
      `docs/archive/M4.5_footprint_meas.md`.
      **What does not transfer, and must be re-captured:** fault addresses, disassembly offsets,
      symbol sizes, stack-depth observations, and every timing figure -- the bench numbers, and
      `inprstorm`'s measured ~37,700 ISR invocations/second
      (`user/apps/xmc4800-relax/inprstorm/main.cc:25`). **That figure is distrusted for a SECOND,
      independent reason**, recorded under M4.5.6's `inprstorm` item: it was one operating point, and
      the FIFO-storm profiles replaced it with a structural bound (`min(fill, drain)`), which no
      re-measurement can move. So the toolchain caveat here and the operating-point caveat there are
      separate; the M4.5.6 captures were already taken `MinSizeRel`, which settles this one for that
      app but not for the rest of the list. Least transferable of all: `pvprobe`'s
      privileged-write measurements, whose whole subject is whether an individual store lands.
      **What stands:** `bluepill-c8-st` and `f302nucleo-st`. The deleted two-board holding measure
      already built those two `-Os`, and their `.text` is byte-identical under `MinSizeRel`.
      **What the move buys, which changes what a witness is worth:** a board's `-st` kernel and its
      non-`-st` kernel differed by **13,505 bytes** as shipped, so an `-st` witness never
      transferred to the shipped image at all. Both now build `-Os` and differ by under 200 bytes
      -- the flag's genuine content (`arch_reboot`, `arch_irq_inject`, `arch_mpu_probe_addr`,
      `irq_spurious_count`) -- so an `-st` witness finally does transfer.
      **The same pass clears this milestone's silicon debt.** Each item is unwitnessed for its own
      reason and none of them justifies a separate bench trip:
        - The UART-FIFO drain on the reboot path (`arch_console_flush_sync` before `arch_reboot`,
          `kernel/syscall/syscall.cc:389`). Only `mk64f` and `xmc4800` implement the seam and
          neither has an emulator gate, so the sim and QEMU gates exercise only the no-op fallback
          (`arch/common/arch_console_flush_sync_default.cc`) -- the truncation fix itself has never
          run against a real
          UART.
        - `dev_window_exclusive` and `bus_device_slots`: both postdate every silicon capture, so no
          chip has ever run them. Already recorded under the five-apps DEV-window item below.
        - The optimised `arch_reboot` path on `picopi` and `pizero2350`: verified by disassembly
          only, never executed, and neither RP part has an emulator gate. Distinct from the
          never-run RP2040 and imxrt1062 reboot BACKENDS under *Needs hardware* below.
        - `f302nucleo` joins the bench this round: it has an onboard ST-Link and a VCOM console,
          and `tools/flash-stlink.sh:18` already defaults `--connect-under-reset` on for it. It is
          the **only physically-present no-MPU ARM board**, which makes it the sole possible
          silicon witness for the claim that unprivileged root is real on a part with no MPU --
          root starting unprivileged, the ctors and `main` running, selftest green. That is a
          declared objective of the pass, not a by-product. It is NOT the stage-2 enforcement gate,
          which no MPU-less part can meet (see the `bluepill-c8` / `f302nucleo` bullet above).
          **Taken early, in M4.5.6**: `f302nucleo` witnessed the no-MPU claim (`selftest` `1..63` all
          passing) and the RING arm besides. Its fault-reporter root cause was the last thing it
          owed this pass, and that is CLOSED (2026-08-13): the flash command, not the firmware.

## M4.5.6 -- delete the privileged-root posture knob -- and M4.5.7 -- remove the weak-symbol seam mechanism -- BOTH COMPLETE (2026-07-31)

**TWO cleanup sub-milestones, in this order. BOTH ARE DONE** and merged, squashed into `dde73ca`
(PR #6): M4.5.6 deleted the privileged-root posture knob, M4.5.7 removed the weak-symbol
seam mechanism. The order was
not a preference: the knob deletion removes a posture and every `#if` branch behind it, so the seam
pass edited a one-posture tree instead of two. Both are foundation work ahead of M4.6 and
neither is a driver feature. Next is **M4.6.1 -- IRQ + console visibility**.

**Half one -- delete the privileged-root posture knob. COMPLETE** (2026-07-30/31, MinSizeRel,
merged in `dde73ca`; developed as `c5d9b0d` -> `270b6fa` -> `124b68c`, which the captures
stamp). The first
inventory below was written at `c5d9b0d` and has been corrected against the two later commits.
**Capture hygiene, recorded because this milestone broke its own rule.** It wrote down "commit before
a witness pass" and then took most of the later captures at `270b6fa-dirty` or `2fc7799-dirty`
instead of at a committed tip. Only `b4-fault.log` and `b4-ringppb.log` stamp `124b68c`; the `b2-*`
set stamps a clean `270b6fa`. A `-dirty` stamp names the tree it was taken from, not a reproducible
one, so those captures cannot be re-derived from history.

- [x] **The posture knob is DELETED, not defaulted OFF.** Root is unconditionally
      unprivileged on every board: `ThreadAttr::privileged` now defaults `false`, and the
      `cap_seat_authority(&g_root_tcb, CAP_AUTH_ALL)` seat in `kmain` is UNCONDITIONAL rather than
      sitting behind an `#if` -- with no ring to short-circuit `cap_check_authority`, an unseated
      root would fail every authority gate including its own shutdown. The banner's
      `", root unprivileged"` suffix went with it: under one posture that suffix is a constant, so
      it carried zero information. The suffix-free banner is **observed** on `xmc4800-relax`,
      `frdmk64f` and `pizero2350`, but that is evidence about the IMAGE, not the POSTURE -- it proves
      only that those boards ran the new banner code, and since the whole argument for deleting the
      suffix is that it conveyed nothing about posture, its absence conveys nothing either. Calling
      that "witnessed on three boards" was circular. **The posture witnesses are two `rootfault`
      captures**, both at `c5d9b0d`: `frdmk64f`, where `chip_mk64f.cc` keeps SYSMPU `RGD0` at
      supervisor `rwx` so the isolation fault is unreachable from a privileged root, and
      `pizero2350` (precise MemManage, `MMFAR=0x20025020 CFSR=0x82`). Plus the named-skip delta
      below.

      **The name appears in NO code or build file at all.** `270b6fa` deleted the removed-knob
      configure guards along with the scramble-test option's, so a `grep` over
      `*.txt *.cmake *.h *.cc *.json *.in *.sh` returns zero hits. Consequence, and it is a
      deliberate trade rather than an oversight: a stale `-D` for the removed knob is
      **silently ignored** on an in-tree configure AND on an out-of-tree consumer configure, with
      nothing but CMake's generic unused-variable warning to show for it. There is no recovery
      incantation to run, because there is no guard left to clear. The docs still discuss the name
      historically, so "anywhere in the tree" would be false. `master` still defaults the knob `ON`,
      which is why every record taken there says so.
- [x] **The deletion exposed a REAL defect on the panic path, fixed by a new ungated syscall:
      `KOS_SYS_PANIC = 41` / `kos_panic`.** `kmain`'s `kos_shutdown(status);
      KICKOS_UNREACHABLE(...)` tail called `kpanic` DIRECTLY, and after the flip that call runs in
      root's UNPRIVILEGED frame. It is reachable, not theoretical: an app declaring a
      `KICKOS_APP_AUTHORITY` without `KOS_AUTH_SYSTEM` has its `kos_shutdown` refused, and if it
      then returns from `main` the unreachable arm runs. **Measured faulting three ways, with the
      diagnostic LOST every time** -- the worst possible shape for a path whose only job is to
      report. The kernel now panics kernel-side with the caller's message, so the report survives.
      Two implementation constraints, kept because neither is visible at the call site:
        - **Userspace pointer validation reuses the existing `thread_create_call` name-copy pattern**
          rather than inventing a second one: a panic message is the same untrusted-pointer problem
          as a thread name.
        - **The 64-byte message buffer lives in a `noinline` helper.** A syscall runs on the
          CALLER'S stack, and GCC allocates the union of ALL dispatch arms' locals at function
          entry, so a buffer written inline in one arm charges every syscall on every thread for it.
- [x] **`mpu_privileged_guard` / `t_mpu_guard` and the `SKIP_TEST_IF_ROOT_UNPRIVILEGED` macro are
      RETIRED.** Post-deletion its registration condition and its skip condition were both exactly
      `KICKOS_HAVE_MPU`, so **it skipped in 100% of the builds where it existed** -- the
      guard-that-asserts-nothing class this file already tracks, in its terminal form. The premise
      is unreachable as well: the test needs a PRIVILEGED thread to run it, and the only privileged
      thread left is `idle`, which runs no tests. `rootfault` makes the stronger claim on the same
      subject and runs for real. **Confirmed on silicon rather than only by reading**: `frdmk64f` on
      its full service list (`k64uart` + `k64dspi`) went `# skipped: 2` (`mutex_deadlock` +
      `mpu_privileged_guard`) to `# skipped: 1`, 66 cases and 65 ok, the surviving skip being
      `ok 18 - mutex_deadlock # SKIP pool too small` -- the pre-existing `KICKOS_MAX_THREADS`
      constraint already recorded in `docs/reference/boards.md`. **What carries that inference is
      that both runs NAME their skips on the wire, not the totals**: the plan count held at 66 for an
      unrelated reason (a case was added in the same milestone) and has since moved to 67, so compare
      the named transcripts and never the numbers. This also resolves the M4.5.5 open question --
      `master` THEN defaulted root PRIVILEGED, so those rows ran PRIVILEGED and the guard RAN
      rather than skipping, which reconciles all three counts. The `EXPECT_SKIPS` list loses its
      posture-dependent entry too.
- [x] **`rootfault` is now an always-built gate on SIX in-env images** -- `sim`, `qemu`, `qemu-m3`,
      `qemu-m7`, `qemu-m33`, `qemu-riscv`. It previously registered in **no default build at all**:
      it needed the flipped posture, which only hand-runs and the duplicate CI arms provided. The
      count is SIX and not five because `KICKOS_HAVE_MPU` is 1 by arch on the sim, so the sim
      registers it as well.
      Re-witnessed on `frdmk64f` post-deletion:
      `SYSMPU ISOLATION FAULT: port=3 addr=0x20015140 master=0 W EDR=0x80000003`, reported via an
      IMPRECISE bus fault (`CFSR=0x400 HFSR=0x40000000`), which is how SYSMPU reports.
- [x] **`rootauth` is newly gated on `microbit` (`microbit_rootauth`), which closes the no-ring
      authority gap.** `Thread::privileged` is a software field, so `cap_check_authority` stops
      short-circuiting on a part with no ring split -- the authority logic is testable there even
      though the CPU-mode boundary is inert. With the knob gone the app also loses its
      posture `#if` discrimination and is one posture's five arms on identical
      source; it reports `PASS` on `frdmk64f` silicon.
      **REMAINING GAP, recorded rather than papered over: `esp32-wroom` could NOT get a run gate.**
      Upstream QEMU models no ESP32 machine, so no board-to-machine mapping is constructible and the
      Xtensa no-ring case has no in-env carrier at all.
- [x] **The two duplicate CI arms are deleted** -- `ci.yml`'s `build/sim-flip` and the
      `build/$b-flip` loop. Measured before deleting rather than after: `rootfault` was the only
      test they carried that the base arms lacked, and the base `KICKOS_HAVE_MPU=1` arms now
      register it themselves, so the arms were paying two extra toolchain builds for nothing. The
      same pass gives `qemu-riscv-mpu` a `qemu_riscv_rootfault` -- **a gate CI had never run.**
- [x] **The root-MMIO service-list `FATAL_ERROR` is re-conditioned on `KICKOS_HAVE_MPU`, not
      on the posture.** With the knob gone the posture is no longer a variable, but the substitution
      is deliberate and not mechanical: **with the MPU off, an unprivileged root DOES reach MMIO** --
      a cross-domain write completed on a non-MPU qemu image, measured -- so the gate's real subject
      was always ENFORCEMENT and the condition now says so. **The list is then EMPTIED in `270b6fa`**
      once `xmcssc` as a service was witnessed, so no service list is refused today. The variable and
      its gate stay for the next board whose bring-up writes MMIO from root, because that failure is
      silent and total.
- [x] **`consoledemo`'s scramble-test build option is restaged as a standalone app, `conreclaim`,
      registered only when `KICKOS_SERVICE_LIST=kickos_services_none`. THAT OPTION NO LONGER
      EXISTS** -- any doc still naming it is stale. The split was forced by a premise conflict
      rather than chosen: U0C0 admits exactly ONE holder, the scrambler has to be
      it, and `consoledemo` exists to demonstrate the `xmcuart` handover, which needs `xmcuart` to
      be that holder. Two mutually exclusive premises behind one option in one ELF. Separating them
      costs nothing, because the property under test -- `arch_console_reclaim` repairs a garbled
      UART from the panic path -- does not depend on WHO garbled the UART. The remedy this replaces
      is corrected in place under the five-apps DEV-window item below. It carries **no CTest gate**,
      and cannot: the verdict is an operator reading the panic banner on silicon, XMC has no QEMU
      model, and the sim cannot reproduce the fault.
- [x] **A second privileged-write seam: `KOS_SYS_PERIPH_REG_WRITE = 42` / `arch_periph_reg_write`,
      class 2 of the family `arch_periph_enable` opened.** This is what unblocks the XMC bring-up
      body recorded under *Blockers and limits* above. Every part of its shape is a decision:
        - **POSSESSION-gated, NOT authority-gated**, the same shape as `arch_periph_enable`: the
          caller must hold a live DEV window based EXACTLY at the block base.
        - **Granularity is an exact `(base, offset)` ALLOWLIST per chip**, never per block. A
          per-block entry would hand back precisely what the silicon withholds, which turns the seam
          into a privilege-escalation primitive keyed on a grant.
        - **Refusals**: `-KOS_EPERM` for possession, `-KOS_EINVAL` off-allowlist, `-KOS_ENOSYS`
          where the chip has no backend.
        - **The default decline uses the LONE-TU pattern**
          (`arch/common/arch_periph_reg_write_default.cc`, exactly one symbol), **not** a weak
          symbol -- which M4.5.7 then made the mechanism for EVERY seam in the tree. The XMC
          definition deliberately sits in the always-anchored `chip_xmc4800.cc`. That anchoring, not
          any duplicate-definition error, is the guarantee: reversing the rescan group was MEASURED
          to resolve correctly anyway, and a definition in an unreferenced TU silently DECLINES with
          zero diagnostics. Leg 2 of `seam_defaults` is what enforces it.
        - **XMC allowlist**: U0C1 `FDR` `0x010`, `BRG` `0x014`, `CCR` `0x040` -- the three registers
          the XMC4800 reference manual marks `Write = PV` (RM V1.3, Table 18-20). U0C0 has NO entry:
          the kernel owns the console channel's baud and enable, and an absent entry is a refusal.
        - **A PER-ENTRY VALUE MASK, added in `270b6fa`.** An address-only allowlist hands back every
          bit of a register the bus withholds whole, so each entry carries the only bits it may set:
          `(value & ~mask) != 0` is refused `-KOS_EINVAL` before the store. The value is REFUSED
          WHOLE and never trimmed -- the seam does no masking and no read-modify-write, because a
          silently dropped configuration bit is exactly what the consumers' read-back exists to
          catch. On `xmc4800` (`arch/arm/chip/xmc4800/chip_xmc4800.cc:362-381`): `CCR` grants
          `MODE[3:0] | RIEN | AIEN`, 6 bits of 32, with `RIEN`/`AIEN` deliberate because `xmcssc`
          arms them last, and WITHHOLDS `TBIEN`, `HPCEN`, `PM`, `RSIEN`, `DLIEN`, `TSIEN`, `BRGIEN`;
          `FDR` grants `STEP | DM`; `BRG` grants every writable field, which makes that one mask
          near-meaningless as a bound and is stated so in place. A `static_assert` pins each composed
          grant against a literal word, so a field-mask edit in the register header cannot widen a
          grant silently.
          **Silicon witness** (`.session/m456-silicon/b2-pvprobe.log`, `commit 270b6fa`, on a SECOND
          physical XMC unit): `[pvprobe] mask refusal: CCR|TBIEN rc=-22 (want -22), pre=0xc001
          post=0xc001 unchanged`. `pre == post` is the load-bearing part -- it is what separates a
          refusal from a partial store.
      **Gate: selftest `periph_reg_write_unheld`**, the negative arm, in-env on every target.
- [x] **`xmcssc` and the four U0C1 apps converted onto the seam.** `xmc_spi0_start` now contains
      ZERO register access, and `xmcspi` / `xmccshold` / `pvprobe` / `inprstorm` no longer write
      FDR/BRG/CCR directly.
      **SILICON 2026-07-30, and `pvprobe` is the DISCRIMINATING witness** -- one run, same
      unprivileged thread, same held window: the seam's writes to FDR/BRG/CCR land `exact`, while
      DIRECT unprivileged stores to those same three registers report `DROPPED (post == pre)`, with
      `SCTR[U,PV control]` LANDED as the positive control and an ungranted SCU poke faulting
      (`CFSR=0x82`, `MMFAR=0x50004648`) as the negative one. Both refusals were taken on hardware
      too: off-allowlist `rc=-22`, unheld window `rc=-1`. That pair of columns is what proves the
      seam is not a no-op, and no emulator can produce it.
      `xmcspi` exercises the sequence in anger -- real SSC loopback, four words echoed,
      `loopback PASS` -- and it validated the implementer's ONLY self-flagged unconfirmed literal:
      `seam FDR: rc=0 wrote=0x816f read=0x39b816f LANDED` is the `RESULT[25:16]` field drifting
      under the read-back, and the new `FDR_RESULT_MASK` correctly returned LANDED instead of a
      false DISCARDED.
      `xmccshold` reports `VERDICT: hardware CS-hold USABLE` (FEM=1 -> 2 MSLS edges, FEM=0 -> 8).
      `inprstorm`'s finding is STRENGTHENED rather than merely re-taken: the attack now runs
      entirely from the unprivileged holder with root arming nothing (`INPR before=0x1100`,
      `after =0x0`, `CCR =0xc001`), and the console still survives past the storm line, so it stays
      a BOUNDED CPU TAX and not a DoS.
      **The TX-FIFO escalation vector is CLOSED, and the verdict is now STRUCTURAL rather than one
      operating point.** Per XMC4700/XMC4800 RM V1.3 Table 18-20 only `FDR`, `BRG` and `CCR` are
      `Write = PV`, so `TBCTR` (`108H`), the `INx` push aperture (`180H + x*4`), `TRBSR` (`114H`) and
      `CCFG` (`004H`) are all `U,PV` -- the vector needed NO allowlist widening to test. RM subtlety
      worth keeping: writing `TBUF0` (`080H`) is the STANDARD buffer and silently BYPASSES the FIFO,
      so the original `inprstorm` never exercised the FIFO at all. Autonomy was proven first:
      `TBCTR=0x6000000 SIZE=64 LANDED (FIFO armed, no seam)`, then `slow-divider preload TBFLVL=64,
      after 10ms no-fill=0`. Three profiles on silicon (`.session/m456-silicon/c3-inprstorm.log`,
      `c3-inprstormmax.log`, `c3-inprstormfifo.log`, all `commit 270b6fa-dirty`) are IDENTICAL:
      heartbeats 0 through 142, **143 distinct beats** -- the files hold 150 heartbeat LINES because
      the reader duplicated seven ahead of the banner, so write 143 and never 150 -- every
      `dt=300ms` bar beat 0, last beat `t=42608ms`.
      **Why it cannot be made worse by tuning:** a backlog only builds while drain < fill, so the
      sustained rate is `min(fill, drain)`, and fill is capped by the attacker's sub-root CPU share
      refilling a FINITE 64-deep FIFO. FIFO depth and clock rate TRADE OFF; they do not multiply.
      Verdict unchanged (bounded CPU tax, not a DoS), now on a structural argument.
      Distrust of the older ~37,700 ISR/second figure has a SECOND, unrelated reason recorded under
      M4.5.5's `MinSizeRel` re-witness item above (the `Debug` -> `MinSizeRel` move invalidates every
      timing figure); these captures were taken `MinSizeRel`, so what they replace is the operating
      point, not the toolchain caveat.
      **`xmcssc` AS A SERVICE is now WITNESSED and the decision is MADE.**
      the root-MMIO refusal list is EMPTY here -- it and its `FATAL_ERROR` were deleted outright on
      2026-08-05 -- and
      `xmc4800-relax` defaults to its FULL service list under enforcement. The wire at
      `commit 270b6fa` (`.session/m456-silicon/b2-xmcssc-vcom.log`) is
      `[xmcuart] driver up (polled TX)` followed by
      `[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)`: the board did NOT go
      dark, which is precisely what the refusal guarded against.
- [x] **`pizero2350` `rootfault` and `rootauth` are TAKEN.** Both at `c5d9b0d`: `rootfault` a PRECISE
      MemManage (`MMFAR=0x20025020 CFSR=0x82`) and `rootauth` `PASS` with five arms. Captures
      `.session/m456-silicon/c2-pz-rootfault.log` and `c2-pz-rootauth.log`, both non-empty; recorded
      in `docs/reference/boards.md`. Its `selftest` came from the same visit (`1..66`, 66 ok,
      `# skipped: 0`, `region_mode` GRANULE-MULTIPLE granule 32 / 96 B). The board did leave the USB
      bus later in that session -- KickOS has no USB device stack, so recovering it needs a physical
      BOOTSEL press -- but it left after these three captures, not before. An earlier revision of this
      item recorded both capture files as zero bytes and marked the pair OWED; that was wrong.

Landed in `124b68c` (a few captures stamp the `270b6fa` working tree that became it), after the first
inventory above was written. Wire values live in `docs/reference/boards.md`; these are cited, not
duplicated.

- [x] **The `esp32c6` `.data` LMA bug, FIXED, and it invalidates a prior witness.** `esp32c6.ld`
      linked `.data` with an `AT` clause while the load counter kept counting from `.text`, so
      `_sidata` sat outside every loaded segment and `Reset_Handler` copied uninitialised SRAM over
      the `.data` the ROM had already placed correctly. `KICKOS_HAVE_MPU=1` only. Fixed by dropping
      the `AT` plus an `ASSERT(_sidata == _sdata)` (`arch/riscv/chip/esp32c6/esp32c6.ld:280`);
      `esp32.ld` carried the same latent construct and was cleaned image-neutrally. **`virt.ld`
      KEEPS its `AT` and must NOT gain the assert** -- QEMU honours PhysAddr there, so the LMA is
      real.
      **The 2026-07-28 `esp32c6` witness therefore passed BY LUCK.** The corrupting bytes are
      uninitialised SRAM, so the outcome varied with the die and the power-on history. That is why
      bisecting found nothing: there was no bad commit to find.
- [x] **`c6blink` CLOSED** (`esp32c6-wroom`, `commit 270b6fa-dirty`): 10x `pad=1/1 pad=0/0`,
      `PASS (pad tracked the drive on every cycle)`, the control `MPU FAULT ... at 0x6009157c`, and
      `selftest` `1..67` with `# skipped: 0`.
- [x] **`rx72m` closed ALL THREE owed items in ONE visit** (`commit 270b6fa`, a CLEAN tip -- the only
      board this round witnessed from one). `selftest` `1..67` `# skipped: 0` with
      `# region shaping: GRANULE-MULTIPLE (granule 16, 3-granule request reserved 48)`, closing
      M4.5.5's region re-encoding on the third moved board. `rootauth` `PASS` with five arms.
      `rxdrv`: `pinmux P80 -> general I/O rc 0`, 10x `pad=0/0 pad=1/1`,
      `PASS (pad tracked the drive on every cycle)`, plus both controls --
      `pinmux PB1/TXD6 refused (-KOS_EBUSY)` and
      `MPU FAULT: task 'rxdrv' attempted write at 0x8c068`. The `periph_enable` probe splits by
      caller as designed: root `-1` (`EPERM`, it holds no window) and holder `-38` (`ENOSYS`,
      possession passes and there is no RX backend).
- [x] **The RING arm is WITNESSED -- a first for the project.** `f302nucleo`, `mpu off`, a real
      no-MPU armv7m: `ringpriv` `PASS (5 arms)` with `CONTROL=0x3`,
      `APSR after writing 0xF8000000=0xf8000000`, and
      `CONTROL after attempting to clear nPRIV=0x3` UNCHANGED. The prober is
      `user/apps/common/ringpriv` and it is PERMANENT CI, not a one-off: `cmake --preset qemu` IS the
      ring-only posture, so `qemu`, `qemu-m3`, `qemu-m7` and `qemu-m33` all carry `ringpriv` and
      `ringppb`. `microbit` asserts the OPPOSITE outcome with one arm rather than skipping, and does
      not build `ringppb` (`user/apps/common/ringpriv/CMakeLists.txt:61-80`) -- a no-ring core cannot
      have the PPB read refused.
- [x] **`f302nucleo` thread-pool provisioning, and the fleet arena model it forced.** The `-st`
      preset advertised `KICKOS_MAX_THREADS=4` on an arena backing three 1024 B stacks, so
      `kos_ram_alloc(1)` returned NULL and `periph_enable_unheld` failed. NOT a regression -- `master`
      fails identically. `board_config.h` takes `KICKOS_USER_STACK_SIZE` 2048 -> 1024 and
      `KICKOS_ROOT_STACK_SIZE` 2048 -> 1536; the preset takes `KICKOS_MAX_THREADS` 4 -> 3. Both
      chosen against MEASURED watermarks, not guessed: deepest pool worker 592 B, root 1048 B, idle
      76 B, leaving a 488 B / 31.8% margin.
      Silicon result: `ok 46`, `1..63` all passing, `# skipped: 5` -- skips **9 -> 5**, which
      un-skips FOUR real arms (`endpoint_crossdomain`, `mem_self_grant_nonpow2`, `region_mode`,
      `domain_share`).
      `cmake/boot_arena.cmake` plus `arch/common/boot_arena.ld.h` now model the thread-stack pool, so
      an overcommit of this shape is a LINK error instead of a runtime NULL.
- [x] **The sim gained an `arch_periph_reg_write` backend**, so the mask compare, containment,
      alignment and wrap checks are gated on the host rather than only on XMC silicon: a 64 KiB
      window taken from the first of five candidate bases that `MAP_FIXED_NOREPLACE` accepts,
      published at init, with one allowlist entry deliberately placed BEYOND the grantable window so
      the containment check has something to refuse. Six mutations were each proved red on a distinct
      check.
- [x] **The in-env gate count, re-measured at `124b68c`.** Eight configurations, zero failures: sim
      19/19, `qemu` 18/18, `qemu +MPU` 21/21, `qemu-m3 +MPU` 19/19, `qemu-m7 +MPU` 20/20,
      `qemu-m33 +MPU` 20/20, `qemu-riscv +MPU` 15/15, `microbit` 12/12. `selftest` plans `1..68` on
      the sim -- the sim ALONE answers the new seam-backend arm -- `1..67` under enforcement and
      `1..63` without; skips are 0 everywhere except `microbit`'s 9. `panicgate` is FIVE cases (five
      images, one case each, registered on all eight configurations;
      `user/apps/common/panicgate/CMakeLists.txt`). Compare named transcripts and not totals: plan
      counts move for reasons unrelated to any one change.

- [ ] **`kos_thread_create` returns `-KOS_ENOMEM` for two different failures**, so arena starvation is
      indistinguishable from a legitimate pool limit at runtime:
      `kernel/syscall/syscall_thread.cc:306` is "thread pool exhausted" and `:375` is "stack arena
      exhausted". That ambiguity mislabelled
      **8 of `f302nucleo`'s 9 skips** as `SKIP pool too small` when every one of them was arena
      starvation, and it is what made the investigation above cost a session. Two distinguishable
      codes, or one diagnostic line naming which limit was hit, would have ended it immediately.
- [x] **`selftest`'s `mutex_deadlock` skip is mislabelled DIFFERENTLY, and the shared label hides
      that.** Its guard is the configured `KICKOS_MAX_HANDLES` (7 on the supply-7 boards) or
      semaphore exhaustion, not stack arena and not thread count, so no amount of arena work will
      EVER un-skip it -- yet it reads as a pool-size
      skip, identical in wording to the eight that were real. Give it its own reason string.
- [x] **Two boards block the fleet-wide boot-arena assert. CLOSED 2026-08-17: the assert is now
      mandatory on every linker script, and only ONE of the two boards was a thread-provisioning
      problem.** `KICKOS_POOL_ARENA_ASSERT` is invoked by all 16 scripts and omitting it is a
      configure `FATAL_ERROR` like its boot-stack twin. What the two deficits actually were:
      - **`bluepill-c8` was never over-advertising threads.** Its whole deficit was the
        `CHIP_STM32F103` `KICKOS_USER_HEAP_SIZE` default of 8192, inherited from the 128 KiB
        `stm32f411` sibling on a 20 KiB part, and `.userheap` is carved immediately below
        `__kickos_ram_start`. Chip default is now 2048 and the `st` variant carries 0, which is
        what `f302nucleo-st` already did. `MAX_THREADS 2` x 2048 was correct all along, and
        `docs/reference/porting.md` had already written down that the heap carve was what bound
        this board while this item blamed the thread pool.
      - **`frdmk64f` genuinely needed the demand cut**, because enforcement pins its arena base
        above the `.appdata` window. Enforcing variants now provision `KICKOS_MAX_THREADS 12`
        (was the fleet default 16); `flat` keeps 16, which its arena really does back.
      Also **corrected: headroom is per-image only where `__kickos_ram_start` follows `.bss`.** On
      `frdmk64f +MPU` the base is pinned and EVERY image reports the identical figure, so the
      "check it against each" advice was right for `bluepill-c8` and wrong for the K64F.
      Post-change worst-image headroom: `frdmk64f{,-st} +MPU` **+7,072 B**, `frdmk64f-flat`
      **+83,328 B**, `bluepill-c8-st` **+4,096 B**, `bluepill-c8` **+2,560 B**. Sign flip proven
      both ways on both shapes (`-DKICKOS_MAX_THREADS=13` and `-DKICKOS_USER_HEAP_SIZE=8192` each
      fail the link). **`boards/qemu-m33/mps2.ld` is a BOARD-LOCAL linker script**, which the new
      mandatory check caught on its first run; a chip-directory sweep alone would have missed it.
      **NOT witnessed on silicon:** `bluepill-c8` is build-only and `frdmk64f` needs an operator.
- [x] **`f302nucleo`'s fault reporter produces NO dump. CLOSED 2026-08-13: THE FLASH COMMAND, not
      the firmware.** `st-flash --connect-under-reset --reset write` leaves the core under halting
      debug with `DEMCR.VC_HARDERR` armed, so the fault reaches HardFault and the core HALTS at the
      handler's first instruction instead of running it -- `DFSR.VCATCH` set, `DHCSR.S_LOCKUP` clear,
      `HFSR` FORCED with `VECTTBL` clear, and a stacked frame carrying the faulting PC. Proved by a
      single change on ONE boot with no reset or reflash: writing `DEMCR=0x01000000` made the stalled
      boot print its report. `tools/flash-stlink.sh` no longer pairs the two flags. The section below
      titled "the defect is MISFRAMED" was right about the reporter being innocent and wrong about
      where to look next.
      **AND A SECOND CUT HAD A SECOND CAUSE, ALSO NOT THE FIRMWARE: `bench-capture.sh`'s separate
      `st-flash reset` step**, which cut the correct boot off mid-line and started another one that
      stalled -- which is why a capture taken that way holds two boots and a truncated first. Flashing
      WRITE-ONLY and taking no separate reset gives ONE boot, 300 bytes and a complete
      `[fs] survivor ran after the fault`, and a write-only capture of the PRE-fix image reproduces
      that byte for byte, which is what proves it is the capture protocol rather than the image. The
      board now has a gate-verified fault-isolation witness, TAG `m484p2`. **So its post-fault console
      is NOT an unreliable instrument**, and expecting two plan lines there and slicing from the last
      one is retired: take the capture write-only.
      Original notes follow.
      **Superseded reading:** Not the new probers' bug:
      the pre-existing `fault` app truncates at `[f` (338 bytes, `.session/m456-silicon/b4-fault.log`)
      exactly as `ringppb` does.
      **The hardware faults correctly** -- debugger attach confirms what ARM ARM B3.1.1 requires:
      `CFSR=0x00008200` (BFSR `0x82` = `PRECISERR|BFARVALID`), `BFAR=0xe000ed00` (the exact probed
      address), `HFSR=0x40000000` (`FORCED`, because `BUSFAULTENA` is never set in-tree), and the
      UART is ready (`CR1=0x0d`, `ISR=0x006000d0`, `TXE` and `TC` both set).
      **THE SPAN IS NOT NARROW, and two previously recorded readings are NOT evidence.**
      `b5-nuc-fault-gdb.out` is a FIXED 120-instruction budget (`stepi 120`, its line 19), and
      `DHCSR`/`HFSR`/`CFSR` are sampled at the END of the trace, after that budget ran out. At step
      120 the reporter was executing normally inside `kvsnprintf`, storing the FIRST character of
      `"\n=== HARD FAULT ===\n"` into the stack buffer. So "DHCSR bit 19 clear, no lockup" was read
      off a healthy machine mid-dump and says nothing about the failure. Likewise "MSP peak 376 B"
      is not a peak: `0x20003de8` is exactly the bottom of `kvsnprintf`'s frame, which is where the
      trace stopped.
      **Stack exhaustion is arithmetically EXCLUDED.** Entry MSP `0x20003f60`, reporter push 32 B,
      `kprintf` 288 B, then the deepest branch `kconsole_write` 144 B plus `console_emit` 16 B plus
      `write_sync` 8 B: 488 B worst case against a `0x20003800` floor, leaving 1,400 B spare. The
      2 KiB versus 8 KiB `_kernel_stack_size` difference is not the answer. Do not raise it.
      The real unresolved span is the REST of the dump: `kvsnprintf` past its first character,
      `strlen`, `kconsole_write`'s CRLF cook, `console_emit`, `arch_console_write_sync`.
      **The emitted code on that whole path is instruction-identical to `pizero2350`'s**, verified by
      address-stripped disassembly of both builds. The only difference anywhere is two register
      immediates in `arch_console_write_sync` (F3 `ISR`/`TDR` at `+0x41c`/`+0x428` against F4
      `SR`/`DR` at `+0x400`/`+0x404`), each correct for its IP block.
      **Correction to the killed-hypothesis list.** Still killed: hardware-does-not-fault, vector
      unwired, null backend pointer, output-stuck-in-the-ring, `KICKOS_POLL_SPIN_MAX` (1,000,000).
      "Buffered ring plus a missing `arch_console_reclaim`" is ALSO dead, but NOT for the reason
      recorded: `f411disco` has no post-change fault witness at all. Its only one is 2026-07-29 at
      `6646c8e`, under the MPU preset, from a MemManage fault, three tips before the failure, and it
      was explicitly NOT in the retake list after the reclaim widening changed the panic path on
      every board, because the board was not plugged in. The valid substitute witness is
      `pizero2350`: `KICKOS_ARCH=armv7m`, buffered, defines no `arch_console_reclaim`, and dumped
      fine post-change at `c5d9b0d`/`270b6fa`.
      **HYPOTHESIS SPACE NARROWED on silicon, 2026-07-31, without the f302.** A dead ring flush
      CANNOT explain total silence. On `pizero2350` with `console_tx_flush_sync()` deleted from
      `kpanic_enter`, the fault dump STILL reached the wire: the reporter writes through
      `arch_console_write_sync` regardless of ring state, so a broken drain strands PRE-fault output
      and corrupts ordering but never silences the dump. So whatever f302 has, it is not a ring or
      flush fault. That measurement stands; the candidate list it fed is spent, the cause being
      outside the firmware entirely.
      **The LED probe queued here was RUN, and its dark-LED reading was correct**: the core was
      halted at the handler's first instruction, so nothing downstream of it could run. The
      step-capped-trace warning outlived the defect and is kept as method: prefer breakpoints plus
      `continue` over `stepi N`, because a fixed budget samples `DHCSR`/`HFSR`/`CFSR` wherever it
      happens to stop -- which is how a healthy mid-dump machine got read as evidence about a silent
      one, twice.
- [ ] **The structural coverage hole behind it: no emulated gate can exercise a buffered-ring panic
      flush, and the sim CANNOT substitute.** Every fault-dump gate in the fleet runs on an
      UNBUFFERED-console board (mps2 semihosting, `microbit`, `virt`), so "the panic path must drain
      a ring it just reclaimed" has zero in-env coverage. That is why the item above survived to
      silicon. **The sim looked like the fix and is not**: its synthetic TX backend re-raises SIGUSR1
      and re-asserts until the ring empties, so the ring is provably empty at panic time (`used=0`
      measured for both `fault` and `panicgate1`). MEASURED CONSEQUENCE: deleting
      `console_tx_flush_sync()` from `kpanic_enter` outright leaves the whole sim suite green. So the
      flush is unreachable-dead from the sim's point of view. `sim_published_panic` (M4.6.1) covers
      the reclaim and the polled route, not the drain.
      **WITNESSED on `pizero2350` 2026-07-31 (`.session/m458-silicon/pzdrain-*.log`), which is the
      first measurement of the drain with a PROVEN non-empty ring.** A burst-then-fault app with a
      probe inside `kpanic_enter` (after `arch_irq_save`, before the flush) read
      `used_at_panic=419` of a 511-byte usable ring, then 0 after the flush; all 16 burst lines
      reached the wire in order, dump last. Negative control with the flush deleted: the same 419
      stranded, only the 4 already-shifted bytes preceded the dump, and the remainder surfaced AFTER
      it spliced mid-token, flushed only by `kickos_bootloader_handover` on the way to BOOTSEL. On an
      image that halts instead of rebooting those 419 bytes are lost outright. The drain is therefore
      live and load-bearing on RP2350 even though it is dead code on the sim. Captures were taken
      from a dirty tree; the exact diff is pinned at `.session/m458-silicon/pzdrain-tree.diff`.
      Two claims in the tree overstate this and are owed a fix: `tests/integration/check_fault_dump.sh` (its
      header) and `user/apps/common/fault/main.cc` (its header) both say the marker catches a dump
      lost into an armed ring. On the sim it cannot. What `fault_dump` really covers is the
      `RECLAIMED`-to-polled routing.

**Half two -- remove the weak-symbol seam mechanism. COMPLETE** (2026-07-31, developed as an
isolated tip commit, merged in `dde73ca`). Outcome first, then the reasoning that chose it:

- **33 `__attribute__((weak))` definitions removed.** 32 were visible to a plain-fleet `nm` sweep;
  the 33rd, `SystemCoreClock`, is weak only under `KICKOS_BENCH=ON`, which is why the sweep missed
  it. 32 became one-symbol fallback TUs; `arch_mpu_apply` became a PLAIN, non-overridable definition
  because nothing ever overrode it -- `chip_mk64f.cc`'s claim that K64F overrode it was FALSE and
  has been corrected in the code. `.weak NMI_Handler` in 11 `startup.S` files became a file-local
  label. `SystemCoreClock`'s 0 now lives in the SIM arch library only, so an MCU chip that forgets
  it fails the LINK.
- **The placement rule is load-bearing.** Fallbacks live in `kickos_arch_<arch>` and NEVER in
  `kickos_kernel`: the rescan group scans the kernel archive BEFORE the chip archive, so a
  kernel-resident fallback is extracted in the same pass that first makes the symbol undefined and
  then collides with the chip. Seams the kernel declared moved out of `kernel/` into `arch/common/`.
  Canonical statement, stated once: `arch/CMakeLists.txt:11-71`.
- **The real invariant is ANCHORING, not a duplicate-definition error.** Reversing the rescan group
  still resolves correctly (the chip member is force-loaded in pass 1 via `-u g_isr_vector`), so the
  scan-order backstop was measured NEVER to fire. Proof it is insufficient: reversing the group AND
  moving `arch_idle_wait` into an unreferenced TU made the link succeed with ZERO diagnostics while
  `objdump` showed the fallback's `wfi` instead of the chip's `nop`. A silent decline.
- **Payoff realised**: `cmake/boot_arena.cmake` lost its weak/strong precedence logic entirely, and
  two backend definitions of one seam are now a `FATAL_ERROR` naming both files. The thread-stack
  pool modelling (`KICKOS_POOL_ARENA_ASSERT`) is untouched and scraped geometry is byte-identical on
  every board.
- **Two facts for the next porter**: ld's map headings are LOCALE-TRANSLATED (this host emits
  "Membre d'archive inclu..."), so the gate parses the map structurally; and `.text.<symbol>`
  sections do not exist in the hosted build or under the RX toolchain, which also prints map symbols
  without the psABI underscore -- hence the two spellings in the allowlist.
- **Gates**: +1 (`seam_defaults`) on every arm. sim 20, `qemu` 19, `qemu`+MPU 22, `qemu-m3`+MPU 20,
  `qemu-m7`+MPU 21, `qemu-m33`+MPU 21, `qemu-riscv`+MPU 16, `microbit` 13. Fleet links 33/33, MPU
  variants 14/14, zero `.data`/`.bss` change on `bluepill-c8-st` and `microbit`.

**Decision (2026-07-30): remove weak symbols from the arch/kernel seams entirely**, keeping only the
libc-interop exceptions. Two reasons, and the second is the deciding one:

- **Maintainability.** Resolution depends on archive member extraction and link order, neither of
  which is visible at the call site. `cmake/boot_arena.cmake` has to *reimplement* weak/strong
  precedence in CMake to model the boot arena, and that logic carried a latent bug: a `GLOB`
  attributed `arch/arm/common/arch_arm_pmsav8.cc` to the arch family though the build compiles it
  into the CHIP library, which was load-bearing on that file happening to decline the
  `arch_mpu_min_region` override.
- **`__attribute__((weak))` is a GNU extension**, and its interaction with archive extraction is
  implementation-defined. Requiring GCC for a kernel whose whole seam story is portability is the
  wrong trade. This already bit once, in-tree: GCC carries a weak attribute from a declaration onto
  a definition in the same TU, so every `KICKOS_APP_AUTHORITY` override compiled `W` and link order
  picked the winner (`nm` was the check; reasoning was not).

**Inventory -- and the planning figure was WRONG, so do not repeat it.** The sweep counted 48
`nm`-visible weak symbols and inferred "45 of 48 go, 3 stay". Only 33 were ever convertible. The
true split:

- **32 `__attribute__((weak))` backend seams** -- converted (plus `SystemCoreClock`, bench-only and
  invisible to the sweep, and `.weak NMI_Handler` in 11 `startup.S` files).
- **3 libc-interop DEFINITIONS** -- `__dso_handle`, `__malloc_lock`, `__malloc_unlock`
  (`user/src/newlib_stubs.cc`), weak so a libc that *does* provide them wins. Allowlisted.
- **12 C++ vague-linkage COMDAT symbols** -- `kickos::List::push_back`, `SlotPool<T,N>::resolve`,
  `kickos::Kernel::Kernel()`, `kickos::emit`, ... They report `W` in `nm` but are LANGUAGE-MANDATED,
  not a seam mechanism, and are not removable. This is the whole of the counting error.
- **`kickos_app_build_time`** -- hand-rolled C vague linkage, not a seam: `user/include/kickos/app.h`
  defines it inline and the build force-includes that header into every app TU, so a lone TU cannot
  replace it.
- **7 weak UNDEFINED references** -- `__kickos_code_start/_end`, `__kickos_appdata_start/_end`,
  `_kickos_heap_start/_limit`, `__register_frame`. A fallback cannot supply an address for a symbol
  whose whole point is that it may be absent.

**Replacement: the lone-TU pattern, which this repo has already proven** in
`system/init/common/app_authority_default.cc`: the fallback sits alone in its own TU, so a chip defining
the symbol resolves it locally and the member is never extracted. Standard archive semantics, no
compiler extension, no weak attribute anywhere. The constraint is real and must be documented per
file: **such a TU must define exactly one symbol**, or it gets extracted anyway and collides.
Group two fallbacks in one TU only where they are genuinely all-or-nothing (`arch_diag_led_init`
plus `arch_diag_led_set`).
The two other proven alternatives stay available where they fit better: a CMake-selected provider
spliced into the link group (`KICKOS_INIT_PROVIDER`), and no default at all where a missing
override should fail the link (`arch_reserved_blocks`).

**Payoff beyond the removal**: `cmake/boot_arena.cmake` loses its precedence logic entirely -- one
definition per link, found on the chip target.

**Honest about what this trades, not a lateral move.** The lone-TU pattern still rests on LINKER
behaviour rather than language semantics -- a member is extracted only to resolve an undefined
symbol. That rule is standard across every Unix-like linker and MSVC's `.lib` handling, whereas
`__attribute__((weak))` is a GNU extension whose archive interaction is implementation-defined. So
it is a real portability gain, but the seam is still not expressible in pure C.

**Non-regressible: the gate LANDED** as `tests/static/check_seam_defaults.sh`, ctest name `seam_defaults`,
registered in `user/apps/common/selftest/CMakeLists.txt` and running on EVERY board. It reads the
selftest ELF, that target's `-Wl,-Map` link map, `tests/static/weak_allowlist.txt`, every archive of the
rescan group and the app's own objects. Four legs, all four mutation-proved:

1. Each `*_default.cc` member defines EXACTLY ONE global symbol, no other member of the same archive
   defines it, and no fallback sits in `kickos_kernel`.
2. Where a backend defines the seam, the fallback member is ABSENT from the link map entirely. This
   is what makes the ANCHORING rule enforceable rather than commented.
3. Where none does, the fallback IS in the map's inclusion list, pulled by that exact symbol -- plus
   a non-vacuity check, so the gate cannot go quiet if a board resolves no seam from a fallback.
4. Zero weak symbols outside the allowlist, per archive AND in the final ELF; a `_Z`-mangled weak
   symbol in an archive must ADDITIONALLY be COMDAT (proved via `readelf` section-group `G` flags),
   so mangling is not a blanket exemption. Honest limit: section groups are resolved away in the
   final ELF, so mangled names are taken on trust there; the per-archive leg covers KickOS code.

**NOT in M4.5.5.** It touches every arch seam, so it would invalidate the silicon captures just
taken and blow the milestone's scope.

**Slot: M4.5.7, after M4.5.6, and BEFORE M4.6.** Foundation before the driver era, like the
rest of M4.5.x. Half one is the reason for the internal order (see the intro above). The ordering
against M4.6 is the CI gate: once zero-weak is enforced, every new seam is forced into the pattern on
first write. Were it placed after M4.6's driver work instead -- M4.6.1's IRQ substrate and M4.6.2's
USB CDC, the two sub-milestones that add the most new arch seams -- those seams would get written
weak because nothing stops them, and then be rewritten: the same work twice, plus a window where the
tree drifts back. `arch_periph_reg_write` under M4.5.6 is the pattern already being honoured ahead
of the gate.
It also overlaps the M7 seam rework -- `arch_ram_region_size` still carries its `SEAM (MMU era)`
marker -- but the three MPU-geometry seams (`arch_mpu_min_region`, `arch_mpu_region_pow2`,
`arch_mpu_region_encodable`) were converted here rather than deferred into that redesign: they are
fallback TUs now, so M7 rewrites bodies and not the resolution mechanism.

## M4.6.1 -- the IRQ substrate, then the buffered userspace UART on it

**First of the M4.6 sub-milestones, and the substrate the other two stand on.** The first three
items are filed in detail further down this file, so the detail is not duplicated here; the two
after them are carried up from `docs/design-m4-fable-review.md`, which recorded them nowhere else.
It goes first for two reasons: an interrupt-driven, respawnable console driver cannot be built until
the first three are fixed, and none of them needs a board on the bench.

**TWO HALVES, ruled 2026-07-31. BOTH ARE COMPLETE.** The buffered userspace UART
(`docs/design-m4.6-irq-driver.md` sections 7-8) is M4.6.1's SECOND HALF rather than a sub-milestone
of its own, so M4.6.2 stays USB CDC and M4.6.3..N stays the witness pass. The first half is
line-as-capability, reclaim on every death path, and handover ordering. The second half is the
chip-independent layer plus five per-chip consumers, four of which now carry the whole selftest on
silicon -- the per-board record is in *M4.6.1 IRQ consoles on silicon* below.

- [x] **The per-chip `Uart` class plus the two spawns. DONE, five times over.** The shared half is
      `byte_ring.h`, `uart.h` (size-asserted wire ABI), `uart_service.h` (the two loops, the
      doorbell, the RX/TX policy), `KOS_SVC_UART`, and a `uart_service` selftest case that drives
      `serve_one` with no device at all. The silicon edge is `configure`, `service_irq`,
      `tx_irq_enable`, plus the bring-up that allocates the 1 KiB shared block and spawns the IRQ
      thread `{win, shared}` and the service thread `{shared}`. **Asymmetric on purpose**: a DEV
      window has ONE holder, so a service thread that tried to touch the peripheral would fail at
      SPAWN, not at the register write -- the isolation rule is enforced by the domain model
      (design section 3.3, corrected there against an earlier draft).
      **The SIM consumer** closes the in-env half: `system/init/sim/service_list_uart.cc` is a
      `KOS_SVC_UART` port over host fd 1 with TX fed back to RX, running the real two-thread
      driver, gated by `tests/integration/check_sim_uartloop.sh`. Nothing raises that line, so the service
      thread's doorbell is the only thing that can move a byte -- which is exactly what makes the
      doorbell mutation-provable, and it is.
      **The XMC channel question was answered the way the design recommended**: the driver BECOMES
      the console service on `U0C0`, taking it at publish, rather than a third USIC channel getting
      pins. No new pins, and it is the change that retires the polled-TX CPU burn. The three things
      that were silicon-only and unwitnessed are now witnessed on four boards: a hardware TX-empty
      interrupt driving the drain, asynchronous RX from a real line, and the transition-triggered
      half of RULE T1.

- [x] **Reclaim IRQ bindings on thread teardown. LANDED.** Filed under *Found during the M4.5.2 stage-2 flip
      work* below: `irq_detach` has exactly one caller in the tree, nothing in `exit_current` touches
      IRQ bindings, so a dead driver keeps its line forever and its binding slot leaks too.
      `spi_service.h` already promises the respawn this breaks.
- [x] **Gate the mint on `AUTH_IRQ`, and gate use on possession of the line cap. LANDED.** It closed
      a hole where any thread could squat any line on the chip permanently -- one syscall,
      irreversible, no authority needed. Minting takes `AUTH_IRQ`; `wait`/`ack`/`notify` take
      possession of the line cap plus the matching right at `cap_resolve_e`. Ruled 2026-07-31 and
      implemented as ruled, with a narrower per-line authority REFUSED and its falsifier recorded:
      `docs/design-m4.6-irq-driver.md` sections 2.1 and 3.6.
- [x] **The gate breaks all four in-tree tier-1 drivers, so they migrated in the same commit.**
      `k64drv` (`user/apps/frdmk64f/k64drv/main.cc`), `f411spi`
      (`user/apps/f411disco/f411spi/main.cc`), `xmcspi`
      (`user/apps/xmc4800-relax/xmcspi/main.cc`) and `xmcssc`
      (`system/driver/xmc4800/xmcssc/xmcssc.cc`) all call `kos_irq_register` from a thread holding no
      authority -- `f411spi` declares `KOS_AUTH_MEMORY | KOS_AUTH_PINMUX` and nothing else, and the
      spawned drivers run at authority zero. They move to **root claims, then delegates at spawn**;
      no compat shim, since the ABI is unstable until the ABI-freeze milestone (M8, the last one).
      The drivers stay at authority == 0, so the
      frozen cap-index range needs no spawn-ABI work. `selftest` is the one caller that already holds
      `AUTH_IRQ` (`user/apps/common/selftest/main.cc` (`KICKOS_APP_AUTHORITY`)) and its `kos::Irq`
      cases keep working, which also means **the suite cannot witness the refusal from root** -- the
      `-KOS_EPERM` arm needs a worker, the shape `t_cpu_clock_set` already uses.
- [x] **Console reclaim when the DRIVER dies (not the system). LANDED**, and it was the other half
      of the reclaim item above rather than a separate subject: while a driver owns the console
      `console_emit` DROPS every kernel write, so a driver that exits used to leave the system
      permanently mute -- no panic banner, no fault dump, no `kprintf`. Two halves by design: the cap
      layer NOTES the published endpoint reaching `recv_holders == 0` (keyed on the count, not a
      thread identity, so a multi-threaded driver reclaims only when its LAST receiver dies), and
      `exit_current` ACTS after the whole `cap_teardown` loop -- reclaiming inside a cap arm could
      re-init the UART while the dying driver's IRQ cap is still live and its line still armed.
      Gated by `tests/integration/check_sim_drvdeath.sh`, the only hardware-free witness in the fleet, whose
      assertion is one `kos_print` call site absent before the death and present after. **Not
      closed**: a per-chip `arch_console_reclaim` body existed on FOUR chips when this item was
      closed (`mk64f`, `xmc4800`, `esp32`, `esp32c6`) and the set has grown since. On a chip WITHOUT
      one the polled route returns but the DEVICE is whatever the driver left. The live set is
      `grep -rln '^void arch_console_reclaim(void)' arch/*/chip/`; per-chip bodies are fleet work.
- [x] **Console visibility and handover ordering. LANDED**, by the second of the two remedies the
      finding offered -- root VERIFIES, rather than the publish being reordered.
      `console_handover_finish` (`user/include/kickos/sys/driver_service.h`) closes root's own WAIT
      cap and then probes the route with a ZERO-LENGTH rendezvous on cap 0, which returns only once
      the driver has received: no client can run inside the window. Closing before probing is
      load-bearing -- it leaves the driver as the sole receiver, so a death takes `recv_holders` to
      0, which both EPIPEs the probe and reclaims the console. **That is what closes "the driver
      cannot report its own bring-up failure"**: the report is possible because the death gives the
      console back, so the path is no longer structurally mute. A failed spawn self-heals the same
      way (`handle_close` acts on a pending reclaim note, and the helper closes before printing).
      Both cases gated by `tests/integration/check_sim_drvdeath.sh`; the second one is also what
      mutation-proves the probe, since without it the service returns 0 and the app runs on a
      console nothing is serving. **Deliberately NOT the other remedy**: publishing after the spawn
      would have the kernel ring and the driver drive one UART at once. A single-owner device means
      the kernel must let go first, so a span in which kernel-console writes are dropped is
      inherent; what the probe removes is any CLIENT running inside it.
- [ ] **The clock-tree service contradicts its own bring-up DAG, and the DVFS notifier cascade has
      no timeout.** `docs/design-m4-fable-review.md` finding 6, OPEN, recorded nowhere else. Two
      defects in one principle. **The contradiction**: `docs/design-driver-era-scope.md` section 3.1
      makes CLOCK-TREE a persistent RUNTIME service that init brings up BEFORE gpio and the drivers,
      while its G7 dependency list has the clock-tree SERVICE follow the first drivers. Both cannot
      hold. Cheaper resolution recorded there: the DAG's real dependency is only "gate the driver's
      clocks at bring-up", a one-shot init step like pinmux, with no standing service. **The
      cascade**: "Linux CCF shape" hides that CCF notifiers are same-address-space calls under a
      mutex, where each notify here is cross-domain IPC. A rate change during an in-flight SPI EOQ
      or UART frame corrupts the wire, so the fan-out needs a PRE-quiesce (drain/park) plus a POST
      phase, which is a two-phase commit across N untrusted driver threads: one slow or dead driver
      stalls DVFS forever with no timeout, and a driver whose notify handler calls the clock service
      re-enters a single-threaded service parked mid-cascade. **Recommendation as recorded**: drop
      the standing clock-tree service from the M4 principle set, keep init one-shot gating (which
      satisfies the DAG) plus the G4 kernel mechanism with the console handshake as the FIRST forced
      instance of the notify protocol, and design the full service against that proven instance.
      **Outcome recorded 2026-07-30**: no standing service was built, which is the recommendation
      being followed rather than the finding being closed. The no-timeout half is not theoretical:
      finding 5 materialised in exactly that shape in M4.5.6.
- [ ] **`kos_cap_narrow` narrows authority but not endpoint rights, so there is no driver-death
      story.** `docs/design-m4-fable-review.md` finding 5's residual API gap, recorded nowhere else.
      **Sharpened 2026-07-31 by building the death gate**, which needed a driver to die and could
      not get one this way: there is NO kernel path that wakes a receiver parked in `kos_recv` when
      the last `SIGNAL` holder goes. Only the mirror exists (`recv_holders` -> 0 EPIPEs parked
      SENDERS, `obj_close_protocol`), so a console driver parked in `recv` blocks forever however
      its clients go away, and `system/init/sim/service_list.cc`'s own `n < 0` break is unreachable
      defence rather than a working exit. `tests/integration/check_sim_drvdeath.sh` therefore bounds the driver
      to N served messages instead. So the gap has TWO halves now: root cannot drop `WAIT` while
      keeping the endpoint (the narrow), and a parked receiver has no last-sender wake at all.
      `cap_narrow_authority` (`kernel/syscall/cap.cc`) refuses any handle that does not name the
      authority word with `-KOS_EINVAL`, so "keep the endpoint cap but drop `WAIT`" cannot be
      expressed. That is what defeats the only server-death wake the kernel has: last-receiver-gone
      raises `-KOS_EPIPE` on parked waiters, but root keeps a WAIT-bearing cap on a service endpoint
      so it can hand `SIGNAL` copies to clients, so `recv_holders >= 1` however the server dies and a
      client parked in `kos_call` would block forever. `xmcssc` therefore has to panic on a bring-up
      failure rather than exit, and carries the rule as a comment
      (`system/driver/xmc4800/xmcssc/xmcssc.cc`, `bus_thread`). **Recommendation as recorded**: an
      endpoint-rights narrow is the cheap enabler for a real driver-death story. The generalisation
      is already ABI-free (the handle argument takes any cap); the work is the `recv_holders`
      accounting `obj_close_protocol` does, which the `cap.cc` refusal names as the reason it was
      left out.

## M4.6.2 -- USB CDC console (picopi, pizero2350, teensy41) -- SUPERSEDED by M4.9.1

**`roadmap.md`'s ledger moved this work to M4.9.1 and the RP half is BUILT AND WITNESSED on both
parts.** `system/driver/rp2xxx/rpusb/`, both `service_list_usbcdc.cc` and `user/apps/common/usbcdcwit`
exist; `picopi` and `pizero2350` have each carried CDC bytes on silicon. What genuinely survives
below is the teensy41 seam -- absent by construction, the RT1062 controller needing a cache posture
this tree does not have -- and the design reasoning. Read the planning voice as historical.

**Renumbered from M4.6.1**; the IRQ substrate above took that number, and every reference to
"M4.6.1 (USB CDC)" elsewhere in this file has been repointed here. The dependency is unchanged and
it is not a preference. **The stated REASON was wrong, though, and the design gate corrects it**
(`docs/design-m4.6.2-usb-cdc.md`): the `SETUP` deadline is not what forces the ordering, because
both controllers auto-ACK or NAK in hardware and the software budget is ~2 ms, met by roughly three
orders of magnitude. What actually forces it is reclaim-on-death, a reportable handover failure, and
the two-thread shape -- all three of them M4.6.1's. The sequencing survives; its justification did
not. Building the stack and the foundation it stands on at the same time is still the thing to
avoid.

**A CDC console has a ring, so the panic-path drain is the same question again on two new
controllers.** Branch `tools/panic-ring-probe` carries the instrumentation that answered it for the
PL011: a ring-occupancy query plus two build options, one reporting occupancy at panic entry and one
dropping the drain as a negative control. Both default OFF, so a normal build is unchanged. Rebase
that branch onto whatever tree needs it rather than rewriting it; the negative control is also how
the drain gets mutation-proved without a hand edit that must be reverted. Deliberately not on
`master`, which carries no dev tooling.

**The motivation is that three boards are not self-contained.** `picopi` (GP0), `pizero2350`
(UART1 on GP4/GP5) and `teensy41` (LPUART6, pins 0/1) are the boards whose console needs an
external USB-serial adapter wired to header pins, and `pizero2350` and `teensy41` have no diag
LED wired either. All three parts carry a device-side USB controller, and on `teensy41` it is
the port the board already flashes over, so the board can BE the serial adapter.
**It cost bench time on 2026-07-30, which is the concrete case for it**: `pizero2350` left the USB bus
mid-session -- KickOS has no USB device stack, so nothing on the target answers the host -- and
recovering it needs a physical BOOTSEL press. It cost the rest of that board's session; its
`rootfault` and `rootauth` had already been captured by then, so the debt it created was time, not
witnesses.

**Six more corrections from the design gate, kept here because this section is what a reader hits
first.** The RP2040 and RP2350 USB blocks are the SAME IP, RP2350 a documented superset whose only
software-visible delta is clearing `MAIN_CTRL.PHY_ISO` -- verified register by register, not taken
from the datasheet's own assurance -- so it is ONE backend for two boards. The RT1062 really is a
different programming model, but "ChipIdea" appears nowhere in its RM: it says "EHCI-compatible
core", and device mode is explicitly not EHCI. "+1 backend" undercounts the RT1062, which also drags
in the M7 D-cache. Publishing a USB console BLINDS the pin UART it never touches, so an un-cabled
`picopi` would boot silent -- which is why the gate reverses the driver-death `RECLAIMED` ruling for
this case. The handover probe proves the DRIVER, not the LINK: a UART transmits unlistened, a USB
device does not. And a CDC console does not restore `picotool` recovery, which needs PICOBOOT or a
vendor reset interface.

- [ ] **A CDC-ACM class layer, shared, over TWO device-controller backends.** The class half is
      one implementation: device / config / interface descriptors, the control transfers CDC
      needs (`SET_LINE_CODING`, `SET_CONTROL_LINE_STATE`), two bulk endpoints and one interrupt
      endpoint. The controller half is not shareable across the two families, and that is the
      real cost of adding `teensy41`:
        - **RP2040 / RP2350**: a DPRAM-based USB 1.1 device block. These two appear to be the
          same IP, which would make them the `stm32f411` shape (one backend, two boards).
        - **i.MX RT1062**: a ChipIdea/EHCI-style OTG controller driven by queue heads and
          transfer descriptors, which is a different programming model entirely, not a variant.
      So this is +1 backend for `teensy41`, not +1 stack. **Both claims about the controllers
      are unverified here** and the datasheets are in the local reference set; confirm the RP
      pair really is one block before planning on it.
- [ ] **It is a service, not a port.** A `KOS_SVC_CONSOLE` entry that publishes an endpoint,
      exactly like `k64uart`. The handover machinery is transport-agnostic and already carries
      the choreography (create endpoint, publish, grant the window, spawn the unprivileged
      driver, drop root's cap), so nothing in `system/init/` should need to learn about USB.
- [ ] **The panic path reclaims and polls, and this is the part to design rather than discover.**
      `kpanic_enter` already takes the console back from a userspace driver; the USB analogue
      writes into the bulk IN endpoint's DPRAM buffer, marks it available with a length, and
      polls until the controller returns it. **No device-side interrupt is needed, because the
      HOST issues the IN tokens**, so a fault handler can transmit on an already-configured
      device without re-enumerating. That is the DPRAM shape; on the RT1062 the same idea means
      priming a transfer descriptor and polling its status, so the poll is per-backend work and
      needs confirming separately for each. Three constraints follow, and they hold for both:
        - **The spin must be BOUNDED.** With no host holding the port open there are no IN
          tokens and the buffer never comes back, so an unbounded poll hangs a panic on an
          unplugged board instead of reporting and resetting. `KICKOS_POLL_SPIN_MAX` is the
          existing precedent.
        - **A fault before the host finishes configuring has no console at all.** Narrow, and
          no worse than any console needing bring-up, but it means RTT or a pin UART stays
          worthwhile as the early-boot path rather than being redundant.
        - **A suspended device needs resume signalling before it can transmit**, which is the
          one place the fault handler would have to do protocol work rather than a buffer write.
      Expect the same tail loss the UART reclaim already has: `xmc4800-relax` reproducibly
      clips roughly the last 8 bytes the driver had queued (the word pending in `TBUF0`), and
      the USB analogue is a buffer the driver had filled but not yet marked available.
- [ ] **Reboot-to-bootloader takes the USB device away, on all three.** `arch_reboot` hands the
      chip to a bootloader that owns the same port: the RP bootrom re-enumerates as its own USB
      boot device (`2e8a:000f` for the RP2350, witnessed under *`pizero2350`* in
      `docs/reference/boards.md`), and `imxrt1062`'s `bkpt #251` has the MKL02 present HalfKay.
      A USB console goes dark at that call by construction. Correct, and worth stating where the
      reboot seam is documented rather than discovering it on the bench. Note the flip side on
      `teensy41`: that handover is how the board is flashed, so a USB console and the flashing
      path share one connector by design.
- [ ] **Check the idle path against USB liveness before committing to the design.** A device that
      stops answering the host drops off the bus, so whether the tickless idle path keeps the USB
      controller clocked could constrain `arch_idle_wait` on each part. **Do NOT answer it from the
      DAP story.** This item used to say the RP2040 "sleeps when both cores are idle, which is
      already known to gate its debug bus" -- that is a HYPOTHESIS about an open bug whose cause was
      never established (see the DAP item below), not a known fact, so reasoning from it would build
      the USB design on an unproven premise. Answer it from the datasheet, and confirm it by
      measurement on the part.

## M4.9.2 -- the substrate says what it means

Three strands, the first two of which need no board.

### The `volatile` -> relaxed `std::atomic` conversion

Moved here from M6 rather than left recorded in three places. `docs/design-m7-smp.md` carries the
reasoning and now records what is DONE and what is residue; `docs/reference/style.md` states the
rule.

- [x] **The codegen claim was VERIFIED, not trusted, and it held on all five backends.** A relaxed
      32-bit load and store compile to the same single instruction as `volatile` on armv6m
      (`cortex-m0plus`), armv7m, `rv32imac`, rxv3 and Xtensa LX6. Two things the claim did NOT
      cover, both measured:
      - **64-bit is a libcall everywhere**, `__atomic_load_8`, including on Cortex-M4, and a
        freestanding link has no libatomic. So a 64-bit cross-thread field stays `volatile` with the
        reason at the site: `Thread::switch_count` through the cast in `kernel/sync/sync.cc`,
        `kernel/bench/bench.cc`'s `g_irq_entry_ns`, and the selftest's hog deadline.
      - **`is_always_lock_free` is 0 on armv6m and rxv3** even though the load and the store are
        inline plain instructions there, because RMW is not. So no `static_assert` on it, and no
        `fetch_add` anywhere: every converted field has one writer.
      Byte-wide atomics cost one redundant `uxtb` per load on ARM. Accepted.
- [x] **`byte_ring.h` kept its C validity through a two-spelling shim.** It was held to be the one
      shared-memory struct a C driver may have to name, so its counter type was one macro spelling
      `std::atomic<uint32_t>` in C++ and `_Atomic uint32_t` in C (the macro is not spelled here,
      because it no longer exists in the tree). Both spellings agreed on layout, checked on armv6m:
      20 bytes, 4-aligned. **SUPERSEDED in M5**: the premise was wrong, `kos_byte_ring` has no C
      reader anywhere, so that header is C++-only and uses the wrapper, and the one struct a C main
      really must name, `kos_uart_stats`, carries `kos_counter_t` behind `kos_counter_*` instead.
      Every other header spells the C++ type directly.

### What the conversion's sweep FOUND, none of it about atomics

- [x] **`arch_irq_inject` had no `IrqLock` on riscv and xtensa, and now does.** An unbracketed read-modify-write on
      `g_irq_pending` plus an unlocked read of `g_irq_masked`, while an ISR reaching `arch_irq_mask`
      or `arch_irq_clear_pending` RMWs the same words. `arch/rx/rxv3/arch_rxv3.cc` and
      `arch/sim/sim.cc` bracket theirs correctly and are the shape to copy. A MISSING LOCK, not a
      missing atomic.
- [x] **`esp32` had an `arch_console_reclaim` body and NO `arch_console_reclaim_window`.** It was the
      only chip in the fleet with that pairing missing, and `console_on_driver_death` skips the
      `dev_window_free` precondition ENTIRELY when `win_size` is 0. So on `esp32-wroom` the reclaim
      could fire on the service thread's death while the IRQ thread still held UART0 and silence its
      source, parking it forever -- the exact failure `kernel/init/console.cc` describes and that the
      paired chips are paired to prevent. **The fix does bite on that board**: `lx6uart` gives the
      IRQ thread `window_grant` and the service thread none, both spawned unprivileged, so the
      composition records the DEV region and `dev_window_free` really does see a holder and refuse.
      The window is `UART0_BASE` + `0x1000`, which the grant and TRM Table 3.3-6 agree on.
- [x] **`esp32c6` and `esp32` had no `arch_console_flush_sync`**, so `kickos_terminate` emptied the
      console ring and stopped the core with whatever was still in the UART FIFO. Both bodies poll
      FIFO-empty AND the transmitter state machine idle, never FIFO-empty alone, which is the
      mistake `arch/include/kickos/arch/arch.h` calls out. The two chips differ in what the manual
      gives you: the classic ESP32 TRM enumerates `UART_ST_UTX_OUT` outright (`0: TX_IDLE`), while
      the C6 TRM names the field and not its encodings, so THAT one rests on the reset value and
      says so at the constant rather than claiming documentation it does not have.
- [x] **`rp2040`, `rp2350` and `rx72m` had NONE of the three seams**, and all three have a real
      publishing driver, so a console-driver death left the DEVICE however the dead driver left it.
      **Every chip that publishes is now three of three**: mk64f, xmc4800, esp32c6, esp32, rp2040,
      rp2350, rx72m. `stm32f302` stays flush-only, having no userspace console driver.
      Three things the writing turned up that the survey had wrong or missed:
      - **rp2350's console is UART1 on GP4/GP5, not UART0.** Only UART1 muxes to those pins.
      - **The RP window is the whole `0x4000` APB slot, not the `0x1000` register block**, because
        the SET/CLR/XOR atomic aliases at `+0x1000/+0x2000/+0x3000` write the SAME registers under a
        different address. A holder granted only an alias would not overlap a `0x1000` report.
      - **`rx72m` has levers no other chip has.** `SIMR3.IICSDAS = 10b` drives TXD6 LOW, and
        `SIMR1.IICM` puts the channel in simple I2C mode on the same pin. Either leaves a correct
        UART configuration talking to a dead wire.
- [x] **The reclaim window invariant is ONE-WAY, and saying otherwise would have added a second
      truth.** It must COVER every register the reclaim writes, which each chip's `static_assert`
      enforces locally. It does NOT have to equal the service-list grant: `dev_window_free` tests
      OVERLAP, so any holder able to reach a register the reclaim writes has a grant overlapping
      that register and therefore overlapping the window. Widening a grant alone can never open a
      hole, and coupling the two literals across `arch/` and `system/init/` would buy nothing.
- [x] **`kernel/bench/bench.cc`'s `g_irq_entry_ns` was written and never read, and DELETED.** Its
      comment claimed it served "the frozen-counter arches", but there is no `t0_ns` anywhere to
      subtract it from, so that path was never built rather than a reader having been lost -- which
      is what the sweep had to establish before touching it.
      **IT MOVES A MEASURED NUMBER, so do not compare an IRQ-latency figure across this commit.**
      The write was an `arch_clock_now()` call sitting INSIDE the bench handler, between the cheap
      cycle stamp and the seen flag, so it inflated every IRQ-entry sample on every arch, including
      the ones that had a cycle counter and never needed it. Removing it makes the number smaller,
      and that is an instrument change, not a system improvement. Any archived bench figure was
      taken with the extra call in the handler.
      Adding a wall-clock path later means sampling the same clock on BOTH sides of the raise; a
      stamp at one end alone can never yield an interval.
- [x] **`docs/reference/style.md` marked FOUR rules `gated` and not one gate existed** -- `while
      (true)`, include guards, ASCII-only and the SPDX header. Nothing in `tests/static/`,
      `CMakeLists.txt`, `cmake/` or `ci.yml` checked any of them. It is the document that replaced
      `.clang-format`, so the false mark is what would stop a reviewer looking. Writing the four
      gates found 131 pre-existing violations, which says the marks had never been true.
- [x] **`check_doc_names.sh`'s comment stripper scanned `/* */` BEFORE `//`**, so a `/*` inside a
      line comment opened a block that never closed and every identifier after it dropped out of
      the valid set. Silent under-coverage: the gate still passes, on fewer names. It bit twice in
      one session, both times on a comment naming a path glob. Proven both ways -- planting the
      shape ahead of the ring publication barrier, then the only definition of that name, takes the
      old stripper
      from 697 identifiers to 696 and leaves the fixed one at 697, and both agree on a clean tree.
      The scan now takes whichever marker comes FIRST.

### One definition per non-template body

- [x] Non-template bodies moved out of `driver_service.h`, `uart_service.h` and
      `usb_cdc_service.h` into `user/src/`, as M4.9.1 did for `console_ring.h`. Two hazards the
      survey predicted, both real: `irq_pass` and `dev_shutdown` are each a non-template plus
      template overload pair, so a declaration must stay in the header or the template silently
      wins (proven both ways, by the `nm` mangling and by deleting the declaration); and
      `tests/unit/drvbringup` deliberately does NOT link `kickos_user`, so it needed the new `.cc`
      in its own `SOURCES`.
      **`spi_service.h` CANNOT move and stays inline**, which the survey did not predict.
      `system/driver/mk64f/k64dspi/CMakeLists.txt` compiles the service with
      `-Dkos_spi_device_open=k64dspi_device_open` and three more: two targets over one engine, the
      class symbols renamed so a consumer reaching the public names through `kickos_spi_proxy` does
      not collide. A `-D` rename applies only where the CALLER is compiled, so one body in
      `libkickos_user.a` would call a public name the service never defines. It failed to link on
      `frdmk64f` and `xmc4800-relax`, and the header now says why it is exempt so the next reader
      does not repeat the move.
      **The UART needed a second TU**, `user/src/uart_service_dev.cc`, for the three bodies that
      call the link-chosen `kos_uart_*`: in one TU with the ring side they made the ring member
      undefined on any board with no UART backend, and the sim's `selftest` calls
      `uart::shared_init`.
- [x] **The ring publication barrier was the one build knob that is not global**, an out-of-tree `-D` escape
      hatch no CMake file sets. A ring body compiled into `libkickos_user.a` bakes the default
      compiler-only barrier while a consumer's own TU still gets their `-D`.
      `user/src/console_ring.cc` already carries that exposure from M4.9.1, so the class is not new.
      The real answer is a release store on the now-atomic index, which is M5.

### The i.MX RT1062 USB backend, IN this milestone and NOT working yet

A new chip backend, folded in by ruling rather than split off. It is written, build-verified
fleet-wide, and it FAILS ON SILICON at a known point. Do not read it as landed.

- [x] The backend exists: `system/driver/imxrt1062/rt1062usb/` (~721 lines, EHCI-derived dQH/dTD
      lists, RM ch. 42 cited at every register), a `kickos_services_teensy41_usbcdc` list, and the
      `KICKOS_IMXRT_DCACHE=OFF` posture that `design-m4.6.2-usb-cdc.md` section 4.3 chose as
      stage 1. Stage 3 (S7), the non-cacheable MPU attribute, is now BUILT and equally
      unwitnessed: `ARCH_MPU_NOCACHE` on the block's ALLOCATION, encoded by PMSAv7 and
      PMSAv8, admitted through a three-valued per-chip seam. The configure-time refusal of
      D-cache-plus-USB-console is now a WARNING naming the unwitnessed state, and the
      default for a `_usbcdc` image stays OFF until a board proves otherwise.
- [x] `imxrt1062` gained the three console seams and the diag LED, all of which it lacked. The
      LED matters more than it sounds: `kfault_terminate`'s 3-blink pattern already existed and
      was board-agnostic, so the chip was simply throwing it away. **`docs/reference/boards.md`
      was wrong in three places** about this board having no LED; `regs/iomuxc.h` had named the
      pad all along, and a `blink` capture confirmed it.
- [ ] **THE THREAD STOPS MAKING PROGRESS, AND WHAT STOPS IT IS STILL OPEN.** Read the
      paragraph below for what the clock and window measurements establish, and then the
      one after it for why "the grant" is NOT the settled answer it was written up as.
      (`m492te`, banner `7040afe7`, log `.session/logs/m492te-teensy41-selftest.log`).
      The `[usbclk]` line reads `ccgr6=fc3fc3 pll1=80003040 phyctrl=28200000 phypwd=0
      id=e4a1fa05`. Every clock precondition is met -- CCGR6[CG0] usboh3 gated on, PLL_USB1
      LOCK set and BYPASS clear, the PHY fully powered -- and `id` is exactly `ID_RESET`.
      **The kernel reads USB1 at 0x402E0000 and the controller answers. The unprivileged
      driver thread reads the same address and dies on it**, its breadcrumb stopping at
      `STAGE_PROBE`, which is set immediately before that read. The two accesses differ in
      one thing only, the MPU, so the bus, the clock, the PHY and the reset sequence are all
      out. The controller sequence has never yet been reached, let alone tried.
- [ ] **"DIED AT STAGE X" IS THE DRIVER'S WORD, NOT A MEASUREMENT, AND READING IT AS ONE IS
      WHAT PUT THE GRANT IN THE DOCK.** Root prints `a driver thread never reached its loop`
      after `drv::bring_up` times out waiting for the ready latch; the breadcrumb records
      where the thread HAD GOT TO when that timeout fired. Every stage report in this
      investigation means "was still at stage X when bring_up gave up", never "died there".
      **NEITHER SILENCE IS EVIDENCE, and reading them as evidence was the third wrong
      inference of this chase.** The missing `=== THREAD FAULT ===` line is DESIGNED
      behaviour on this path: `kvprintf_route` does call `kconsole_write` unconditionally,
      but `console_emit` DROPS in `ConsoleState::USER_OWNED` (`kernel/init/console.cc`),
      and `user/src/driver_service.cc` publishes the console BEFORE it spawns the driver
      threads. So from the spawn to the unwind every kernel-console write is dropped, fault
      reports included -- which `rt1062usb.cc` already states in its own header comment.
      `faultsurvive` prints on the other four boards because those runs never publish: the
      STATE differs, not the code path. And a steady dark LED rules out a kernel PANIC only,
      because `kickos_thread_fault_exit` ends at `sched::exit_current(KOS_EXIT_FAULT)` and
      the 3-blink `kfault_terminate` is reached from the panic path alone.
      **So the fault path is back, and in the normal build it is the ONLY survivor.** The
      window between `STAGE_PROBE` and `STAGE_RST_WRITE` is a `set_stage`, one `r32` load, a
      relaxed store and a `set_stage`: no syscall, no loop, nothing that can block or yield.
      The thread there can only fault, stall the bus, or still be running, and the last two
      at a single load would take the core with them -- root printed afterwards.
      **The `KICKOS_RTUSB_UNGRANTED_PROBE` control does NOT discriminate, for a second
      reason on top of its `kos::print`:** `USBPHY1` is itself an off-platform AIPS
      peripheral, so the bridge hypothesis below and the MPU hypothesis predict the same
      death. A discriminating control needs an ungranted RAM address, which only the MPU
      can refuse.
- [x] **THE MPU GRANT IS PROGRAMMED, AND THAT IS A CLEAN NEGATIVE.** Region budget is not it
      (`imxrt1062` installs 3 fixed rows, worst case per-thread is 5, and the board booting
      proves DREGION is 16). No static row overlaps `0x40000000-0x5FFFFFFF`. The shape is
      legal: 512 B, power of two, 512-aligned. Nothing drops silently -- the syscall refuses
      with a distinct errno and the one drop in `kickos_arm_mpu_program` is fail-closed and
      cannot fire for this region. And decisively, `kernel/thread/thread.cc` writes
      `[code, appdata, task-domain, window, stack]` into consecutive slots in one loop, and
      silicon already witnesses the entries on BOTH sides of the window: the `blk->stage`
      stores landed, which is why there is a breadcrumb at all, and the thread runs C++ with
      locals on its own stack. The loop reached the far side, so the window went in.
- [ ] **THE STANDING HYPOTHESIS IS THE AIPSTZ BRIDGE, NOT THE MPU, AND THE TREE ALREADY HAS
      THE SEAM FOR IT.** `AIPSTZn_OPACR0..4` reset to `0x4444_4444`, which is Supervisor
      Protect SET for every off-platform peripheral, and `MPR` resets so the bridge honours
      the core's real `hprot[1]`. An unprivileged access to a SP=1 slot is terminated at the
      bridge with an error response and never reaches the peripheral. That fits every
      measurement: privileged kernel read answers, unprivileged read does not, MPU
      irrelevant. `arch.h` already defines `arch_periph_enable` as "ungate the clock AND
      drop the bus-side supervisor-protect", and `system/driver/mk64f/uart_k64.cc` is the
      working precedent on the same vendor's bridge. `imxrt1062` defines no backend, so it
      links the `-KOS_ENOSYS` default and `rt1062usb.cc` never calls it. The RP boards need
      none, which is why nobody hit this before.
- [ ] **IT FORCES A DESIGN DECISION AND THE ANSWER IS NOT OBVIOUS.** The AIPSTZ unit is a
      16 KiB SLOT, and slot 24 holds USB1, USB2 at +0x200 and USBNC at +0x800 -- exactly the
      three the 512 B window was chosen to exclude. `arch.h` grants a base an entry only
      where the bus gate's granularity is CONTAINED by the block and refuses otherwise. On
      this chip it is not contained, so `docs/design-m4.6.2-usb-cdc.md`'s "narrowest unit
      that is its own" does not survive the RT1062. Either accept a coarse AIPS ceiling and
      say so where the design doc claims otherwise, as `frdmk64f` already does, or keep the
      driver privileged. NOT to be settled by whichever is easier to type.
- [x] **SETTLED ON SILICON: IT IS A PRECISE BUS FAULT, SO IT IS THE BRIDGE.** `m492tg`,
      banner `a47e3cc8`, log `.session/logs/m492tg-teensy41-selftest.log`:

          === THREAD FAULT === thread 'rtusbirq' killed, system continues
            PC=0x60014cf8 CFSR=0x8200
            ADDR=0x402e0000

      `CFSR=0x8200` is entirely the BusFault byte: BFSR `0x82` is PRECISERR plus BFARVALID,
      and `BFAR` is the USB1 base. The MemManage byte is `0x00`, where an MPU denial would
      have read DACCVIOL plus MMARVALID. So the AIPSTZ Supervisor-Protect hypothesis is
      CONFIRMED and the MPU grant is exonerated by measurement as well as by code.
      The line that carries this verdict did not exist until the console fallback landed in
      the same session: three captures of inference could not settle what one word did.
- [x] **RESOLVED, AND THE FRAMING BELOW WAS WRONG: THE TWO BARRIERS ARE INDEPENDENT.** The bus
      gate and the MPU answer the same need by different means, and the MPU stays the
      fine-grained authority. Opening the AIPS slot hands the driver nothing, because its
      window is still 512 B and OTG2 and USBNC are outside it. What the containment rule
      actually protects against is a coarse gate over KERNEL-RESERVED registers, and that is
      satisfiable a second way: reserve the slot's remainder, and
      `grant_region_admissible` refuses any window over it for every caller, privileged
      included. So `arch_reserved_blocks` gained `USB1_BASE + 0x200` for `0x3E00`,
      `arch_periph_enable` gained USB1, and `arch.h` now states both ways of satisfying the
      rule instead of only containment. No policy was relaxed.
      **WITNESSED: `m492tw`, arms 12..104, 93 captured, 0 skipped, 0 partial, over the
      board's own USB CDC with no cable, unprivileged and under MPU enforcement.** The
      RT1062 USB device backend WORKS.
- [x] **The framing that was wrong, kept because it was written up as a decision for the
      user and it was not one.** `arch.h` gives a base an `arch_periph_enable` entry ONLY where the
      bus gate's granularity is CONTAINED by the block, and refuses otherwise. On `mk64f`
      an AIPS slot is 4 KB and holds one peripheral, so `uart_k64.cc` is contained and
      works. On the RT1062 the AIPSTZ unit is a 16 KiB SLOT holding USB1, USB2 at +0x200
      and USBNC at +0x800 -- the three blocks the 512 B window was deliberately chosen to
      exclude. Dropping SP for USB1 necessarily drops it for the other two.
      Three ways out, and they trade different things:
        1. Accept a coarse AIPS ceiling for this chip, say so where
           `docs/design-m4.6.2-usb-cdc.md` section 6.4 claims "the narrowest unit that is
           its own", and note that the MPU window stays 512 B so the driver still cannot
           ADDRESS the neighbours -- the bridge is open, the MPU is not.
        2. Keep the USB driver privileged, which honours the rule and gives up the
           unprivileged-driver claim on this board alone.
        3. Refuse the board for USB CDC and leave `teensy41` on its pin UART.
      Option 1 is the smallest change and 2 is the most honest to the rule. NOT to be
      settled by whichever is easier to type, and not settled here.
- [x] **The two wrong inferences that preceded this, kept because both were argued
      confidently.** The MMIO grant was called exonerated on the strength of a breadcrumb
      stage that touches no register at all, and a dark LED was read as ruling out a fault
      when a faulting driver thread is killed thread-scoped and its report DROPPED,
      `drv::bring_up` having published the console before the spawn -- only a kernel PANIC
      reaches the 3-blink path. What settled it was not a better argument but making the
      kernel take the SAME read the driver dies on, so the two differ in one variable.
- [ ] **The instrument is the lesson.** Two flashes with a breadcrumb in the driver's own block
      localised this from "somewhere in a 721-line file" to one register, where the two flashes
      before it produced only silence. The block is the only channel that survives the publish,
      needing no console.

### What the teensy41 bench measured, and the two defects it exposed

- [x] **THE USB CONSOLE POSTURE COSTS ABOUT 6x ON IPC, MEASURED.** `m493dcon` versus
      `m493dcoff` at `38e8122c`, teensy41 `bench`, `mpu enforce`: 61,124 ns per call/reply
      round-trip with `KICKOS_IMXRT_DCACHE=ON`, 363,005 ns with it OFF, at 32 B. The ratio
      runs 5.3x at 8 B to 6.3x at 256 B. Selecting a `_usbcdc` service list flips that knob
      OFF by default, so the console choice silently buys the slowdown.
      **The mechanism is XIP, not the arena.** The I-cache still covers instruction fetch,
      but literal pools and `.rodata` are DATA reads, and with the D-cache off they reach
      external QSPI flash, which the boot config block drives single-pad at 30 MHz. The
      earlier reading that on-chip OCRAM2 bounds the damage was WRONG and is why this was
      left unmeasured. With the cache on the figure is FLAT across payloads and with it off
      it climbs, which is the tell that memory became the bottleneck.
      So S7, a non-cacheable attribute on the block's ALLOCATION, is a priority and not a
      nicety. It is built now; what it has NOT got is a run. The witness it owes, and the
      results that would falsify it, are written into `design-m4.6.2-usb-cdc.md`.
- [ ] **`bench` DIES ON teensy41 BEFORE ITS SWITCH AND IRQ SECTIONS.** Both runs end with
      `=== THREAD FAULT === thread 'root' killed`, `CFSR=0x82` (DACCVIOL plus MMARVALID),
      `ADDR=0x20200038`, which is OCRAM2. The app's own header advertises context-switch
      cost and IRQ-entry latency; neither ever prints. Not fallout from the MPU enable fix:
      `imxrt1062` is the one chip that calls `kickos_arm_mpu_fixed_init`, so it enforced
      before and after. So the D-cache figures above cover call/reply ONLY.
- [ ] **A call/reply round-trip is about 24,000 cycles at 396 MHz and nothing explains it.**
      61,124 ns with the cache ON, flat from 16 B to 256 B, so it is per-round-trip overhead
      rather than copy cost: two switches, each with a full MPU reprogram. No other board has
      a recorded call/reply figure to compare against, which is itself the gap.

- [ ] **A DISJOINT-DEVICE CONSOLE SHOULD FALL BACK TO `KERNEL_OWNED` ON DRIVER DEATH, and that
      kernel delta is unimplemented.** `docs/design-m4.6.2-usb-cdc.md` section 6.2 rules it.
      Today the state goes `RECLAIMED` instead, which on a board whose reclaim target is a
      different device from the published console yields a working channel anyway, so the two
      differ in bookkeeping rather than in whether the board can speak. That is why it has
      not bitten. Recorded here rather than in a per-board service list, which neither
      implements nor depends on it.

### What the selftest atomics pass left open

- [ ] **`g_cd_lit_rc` and `g_cd_bad_rc` are PLAIN `long` globals with the worker-to-main shape
      of the 56 cells that were just converted** (`selftest/main.cc` around 2408). Their
      siblings `g_cd_goodspawn`, `g_cd_goodname_ran`, `g_cd_neg_ran`, `g_cd_badname_spawn`
      and `g_cd_badname_ran` are plain `int` in the same pattern, and nothing in the file
      says why these are plain while their neighbours are atomic. Either the semaphore
      rendezvous is what serialises them, in which case ONE line at the declaration stops
      the next sweep re-atomising them, or it is a gap. Deciding it also decides whether the
      other 58 needed to be atomic at all, so it is not a local question.
- [ ] **`g_prw` has two writer functions and nothing defends the accumulate.**
      `periph_reg_write_worker` and `periph_reg_write_held_worker` both write it. They cannot
      overlap today because the arm spawns one, waits, reads, then spawns the other. But the
      `load | seen` accumulate is NOT load-bearing (the second read asserts only on bits the
      second worker sets), so a plain store would pass too, and nothing records why it is an
      accumulate.
- [ ] **The two `unsigned char` accumulators should be `uint8_t`** under the fixed-width rule
      in `docs/reference/style.md`. Left as-is because changing them also touches a cast at
      each site.

### Atomic manipulation goes behind kos helpers, never at a call site

- [ ] **THE RULE, and it is broader than the counters:** a call site never spells a load, a
      store or a memory order. Every atomic access is behind a `kos_` helper whose NAME
      carries its contract. `Atomic<T, Order>` does this for the C++-only fields by putting
      the ordering in the type; the C-facing counter fields still need helpers, which in M5
      became `kos_counter_increment` and `kos_counter_load` over a one-member `kos_counter_t`.
      The TYPE is what enforces the rule there: a one-member struct has no `++`, no `+=`, no
      bare read and no bare write, in either language, so a call site cannot violate it.
- [ ] **The counter helper, 21 sites.** Every increment in the tree is a load-then-store pair
      on a `kos_uart_stats` field, across `uart_c6.cc`, `uart_lx6.cc`, `uart_k64.cc`,
      `uart_sci.cc`, `rpusb.cc`, `rt1062usb.cc` and the sim service lists. Those fields were a
      C-facing atomic macro because `uart.h` is C-facing, so a C++ template cannot reach them: the
      helper must be a free function valid in both languages. M5 kept the helper and dropped the
      macro, the fields being single-writer plain words.
      The name must state the SINGLE-WRITER precondition, because the pair is not atomic and a
      second writer loses an update with no gate able to see it. NAME IT
      `kos_counter_increment`, not `bump`, which is jargon and says nothing, and not
      `kos_atomic_*`, which would claim an atomicity a load-store pair does not have.
      `rt1062usb.cc` has a local `bump`; promote and rename rather than duplicate.
- [x] **WHY THESE FIELDS ARE ATOMIC AT ALL, since the hardware does not need it.** A 32-bit
      aligned load or store is a single instruction on all five backends, so single-writer
      SPMC has no tearing to prevent. The reason is the LANGUAGE: plain concurrent access is
      a data race and therefore UB, which licenses the optimiser to cache a value in a
      register across a reader's loop, elide a store, or split an access. `volatile` blocks
      elision but has NO defined concurrency semantics and does not order against
      non-volatile accesses. A relaxed atomic is the minimal spelling that is DEFINED, and it
      emits the same instruction, so the choice costs nothing. Relaxed is also SUFFICIENT
      here: a reader sees the old or the new value and both are valid for an independent
      counter. That stops being true the moment a counter's value implies something about
      other data, which is M6's problem, not this one.
- [ ] **Retire "a C driver" as a justification.** It is no longer a goal, so the comments in
      `byte_ring.h` and `uart.h` that cite it must cite the C consumer APP surface instead.
      This does NOT free the shim: `uart.h` carries the `kos_uart_stats` reply a C app names
      and includes `byte_ring.h`, so both stay C-valid on the surviving reason.

### The fleet-wide witness pass, and whatever it turns up

**Last, because it is the only step that needs boards.** It is where every bench-gated item this file
records comes due at once, and each of them is already written down where it was found. **M4.5.6's
bench sessions closed most of what this list used to hold**; what remains is:

- The three M4.9.1 captures predate the merged tree and want retaking: `usbcdcwit` on `pizero2350`,
  the `picopi` `1..104` and the `teensy41` `1..104`. A witness belongs to a TREE.

- `f411disco`'s `f411spi` stage-4 per-app authority witness -- the LAST of the three, and the only
  board of that set still unavailable.
- M4.5.5's general `MinSizeRel` re-witness pass, for the fault addresses, disassembly offsets, symbol
  sizes, stack-depth observations and timing figures that a `Debug` capture cannot carry forward.
- ~~`f302nucleo`'s fault-reporter root cause~~ CLOSED 2026-08-13: it was the flash command's
  `--reset`, and it never needed a replug.
- ~~Right-sizing `frdmk64f` and `bluepill-c8` so `KICKOS_POOL_ARENA_ASSERT` can go fleet-wide.~~
  CLOSED 2026-08-17: the assert is mandatory fleet-wide. Only `frdmk64f` was a right-sizing
  question; `bluepill-c8`'s deficit was entirely its 8K heap carve. Neither board is
  silicon-witnessed, so both remain a MODEL result until an operator takes the K64F.

**CLOSED by M4.5.6, listed so the ledger is not re-opened by habit:** `rx72m`'s one visit (all three
items, `commit 270b6fa`), `esp32c6-wroom`'s `c6blink`, `pizero2350`'s `rootfault` and `rootauth`,
`xmcssc` as a service (the decision is made and the root-MMIO refusal list is gone), and the
RING-ARM witness -- the prober now exists (`user/apps/common/ringpriv`) and `f302nucleo` took it.
Numbered `..N` because a witness pass is expected to OPEN items as well as close them, and M4.5.6's
sessions did exactly that.

**Why the queue is arranged this way.** M4.6.1 and M4.6.2 are both pure code, so **nothing waits on
bench access**: the sub-milestones that can be finished at a desk go first and the one that cannot
goes last. Bench-gated debt stays recorded and explicitly **NON-BLOCKING** -- the precedent is
M4.5.5, whose `rx72m` visit was booked as debt rather than allowed to hold the region-encoding work
open, and which M4.5.6 then paid. `f411spi` is already WITNESS-READY (it builds from this tree,
declares its mask, parks rather than returns, prints an explicit PASS/FAIL, and its flash tooling is
installed), so the pass needs no preparation beyond the board itself.

## An ordering assertion may not rest on a sleep (2026-08-17)

Opened by ONE `call_infoless_revert` failure, seen once in a parallel `ctest` and not reproduced
by 5 standalone and 3 parallel re-runs. It was neither a kernel bug nor a flake to wait out: two
arms staged their choreography on sleep deadlines, and a sleep orders nothing.

**The mechanism, and it is not sim-specific.** `ktime_on_timer` (`kernel/time/time.cc`) drains
EVERY thread whose deadline has passed in one pass, and `policy_pick_next`
(`kernel/sched/policy_fifo_rr.cc`) then takes the strictly highest-priority runnable thread. So
when one stall outlasts the gap between two deadlines, those two wake in PRIORITY order, not
deadline order. The sim runs the whole guest in one host thread, so any host deschedule does it;
on QEMU a loaded host does it; on silicon a long ISR or a slow tickless path does it.
`mtx_time_unit()` does not save an arm: it grows the unit under STEADY load, but a spike arriving
after the calibration defeats it, and its 30 ms glitch cap is a ceiling no calibration can pass.

**THE RULE, which is the part that generalises: an assertion on the relative ORDER of two logged
characters is sound only when the ordering it depends on is established by a rendezvous -- a
semaphore post, a mutex hand-off, a park -- taken by a thread the consumer OUTRANKS, so the
consumer preempts at the post itself. Never by a sleep.** A sleep-staged order assertion is an
instrument whose failure is indistinguishable from the regression it exists to catch. That is
measured, not feared: deleting the D2 boost in `endpoint_call` makes `call_infoless_revert` fail
on `nth('u', 1) < nth('m', 1)`, the SAME assertion the staging race failed. Where a sleep is
unavoidable, every sleeper must satisfy "higher priority implies the earlier deadline", so that a
coalesced drain reproduces the intended order instead of inverting it.

- [x] **FIXED: `call_infoless_revert` staged the D2 boost's own precondition on a race.** The
      boost is conditional on `e->server`, which the server's info-less `kos_recv` is what seats;
      the server reached that recv on a 1-unit sleep and the prio-20 caller called on a 2-unit
      one. Coalesce the two and the caller runs FIRST, finds no server seated, gets no boost, and
      the prio-12 spoiler then outranks the unboosted prio-8 server. Reproduced at **37/50** with
      the unit pinned to 5 ms under 20 ms host stalls, **2/60** under 40/8 ms stalls at the 1 ms
      floor, and **1/200** on a pristine `master` build with the reported assertion verbatim. The
      log inverts `aucmz` -> `mcauz`, and every `count()` and the `-KOS_ENOSYS` check still pass,
      which is why it reads as a one-off. Now staged on one semaphore, no sleep deadline left:
      **0/50** at the profile that gave 37/50.
- [x] **FIXED: `mutex_chain_boost` had the same defect, twice over.** `ch_b` (prio 10) established
      M1 ownership on a 1-unit sleep, consumed by `ch_a` (prio 20) on a 2-unit sleep AND raced by
      `ch_d` (prio 15) on a 4-unit sleep -- two inverting pairs, both with the provider below its
      consumer. Measured **7/60** under 40/8 ms stalls. Now handed along C -> B -> C -> A -> D by
      two semaphores, no sleep deadline left.
- [x] **Two semaphores, not one, and the reason is a real constraint on this pattern.** A post is
      popped by the HIGHEST-priority waiter (`wq_pop_highest`), so a hop that runs UP the priority
      order cannot share a semaphore with any higher-priority waiter. C -> B needs its own;
      A -> D can share, because A outranks D. `ch_c` now carries **6 grants, exactly
      `KICKOS_MAX_SPAWN_GRANTS`**; the limit was NOT raised. No board overrides that knob, and
      `cap.h`'s `KICKOS_MAX_SPAWN_GRANTS < KICKOS_CAP_CHILD_WIDTH` still holds on the three
      `KICKOS_CAP_TABLE_SUPPLY=7` boards.
- [x] **`t_mutex_chain` now asks `pool_can_host(4)` before it creates anything**, which is also
      what keeps the small boards reporting a skip: they cap at 2 or 3 threads and cannot supply
      the staging semaphores either, so probing first is the difference between a skip and a
      create failure. `porting.md` said `call_infoless_revert` was the file's only pool guard;
      corrected.
- [x] **The fix was mutation-tested, and the FIRST ATTEMPT AT IT SILENTLY COST COVERAGE.** In that
      first version B posted A's release itself, before blocking on M2, so the release reached A
      while B still merely HELD M1: A's walk boosted B and stopped, B boosted C later from its own
      block, and the transitive result was two successive ONE-hop walks -- which a one-hop kernel
      reproduces exactly. Truncating the walk to a single hop left arm 19 green, and a
      `KICKOS_ASSERT(depth == 0)` after the `depth++` in Pass 2 never fired. Routing A's release
      back through C, which only runs again BECAUSE B blocked, restores it: the truncating mutant
      now kills arm 19 4/4 on `nth('e', 1) < nth('d', 1)`, and the probe panics inside arm 19.
      **The provenance matters and was checked rather than assumed: the same mutant also kills arm
      19 on `master`, and the probe also panics there, so the two-hop coverage was NEVER absent
      from the committed tree.** The loss was introduced and repaired entirely inside this
      uncommitted work. **The lesson is the method, not the bug: a staging change can leave every
      assertion passing and still stop witnessing what its own comment claims, so a staging edit
      needs its mutant re-run and not just a green suite.**

- [ ] **The `depth > KICKOS_MAX_MUTEXES` bound in `mutex_lock` Pass 2 (`kernel/sync/sync.cc`) is
      unexercised.** Arm 19 now reaches hop two; nothing reaches the bound. A third mutex and a
      fifth worker would do it, and the arm is already at the 4-worker pool ceiling, so this wants
      its own arm rather than an extension of that one.
- [ ] **Arms whose order assertion still rests on a DURATION MARGIN, not on a deadline
      ordering.** These do not invert under a coalesced drain -- every one of them satisfies
      "higher priority implies earlier deadline", which is why they are not the defect above --
      but each still needs one wall-clock span to outlast another, so a host stall past the 30 ms
      unit cap breaks them, and the failure again looks like the regression they exist to catch.
      Measured over 60 runs under 40/8 ms injected host stalls, in the same runs where the two
      fixed arms scored 0: `call_timeout_revert` **37/60**, `cap_reply_bound_slow` **32/60**,
      `cap_reply_bound_fast` **32/60**, `call_donation_slow` **23/60**, `call_donation_hold`
      **1/60**; at a 5 ms pinned unit under 20/5 ms stalls `call_donation_slow` fails **48/48**.
      None of these is a kernel defect -- they are the instruments losing their margin. The full
      set, with the span each compares:
      - `call_timeout_revert`: server spins 6u then 8u against the spoiler's 4u wake and a call
        deadline of 8u armed at 2u.
      - `call_donation`, `call_donation_hold`, `call_donation_pending`: a 4u server spin against a
        3u spoiler wake -- a **1-unit** margin.
      - `call_donation_slow`: two margins, a 3u pre-recv spin that must still be running at the
        caller's 1u wake (nothing asserts it, so losing it makes the arm VACUOUS rather than red),
        and a 4u post-recv spin against a 5u spoiler wake.
      - `mutex_pi_donation`, `mutex_multi_held`, `mutex_owner_died`: a low holder's spin against a
        higher waiter's wake.
      - `cap_reply_bound_fast` / `_slow`: a second caller's call must land inside the window a
        first reply cap is live.
      - `rr_interleave`: the per-iteration burn is `2 x quantum`; a burn shorter than the slice
        never gets preempted and the interleave vanishes.
      Preferred repair is the same as above, a handoff; where a span is genuinely the subject,
      the arm should at least detect its own vacuity instead of passing.

**Why the remaining arms are safe, so the next reader can re-derive it rather than re-audit.**
Every arm was checked against the inverting shape "a lower-priority worker establishes a
precondition on an EARLIER deadline than the higher-priority worker that consumes it". None is
left. Three structural facts do most of the work: `ktime_sleep_ns(0)` YIELDS instead of parking,
so a 0-unit stage holds no deadline at all; `wq_pop_highest` pops parked senders by priority and
not by arrival, so "who parked first" never decides who is served; and in the sleep-staged arms
that remain, the highest-priority sleeper always holds the earliest deadline, so coalescing
reproduces the intended order. The two-sleeper arms with equal priorities (`sleep_order`) are
saved instead by the ready list being FIFO and the sleep queue deadline-sorted.

## M4.6.1 IRQ consoles on silicon: ALL FIVE run the whole suite (2026-08-02)

**The `m461d-*` pass is NOT a committed-tip pass**: every one of its banners stamps `0f5a5bd-dirty`,
an ancestor of `c82cc63` -- so read the banner, not the branch, before crediting these captures to a
commit. The committed-tip pass of record is `257def0` (`m461h-*`), and `STATE.md` carries it. What follows describes `m461d-*` (`1..76` enforcing /
`1..72` not, all pass, 0 skip, 1 partial each -- `cap_capacity` reporting a single class, which is
every hardware board). It is the finished tree: Stage 3's slab, the CRLF cook and the five
first-light markers. Two things only it could witness: every marker reached the wire, and
driver-carried TAP lines end `\r\n` where the `cb5f2a4` capture from the same board ends them with a
bare `\n` -- the two console routes agree byte for byte at last. The `cb5f2a4` pass below is what
closed the driver work and the `rx72m` stop; it stays as that fix's provenance.

Selected with `-DKICKOS_SERVICE_LIST=kickos_services_<board>_uartirq -DKICKOS_ENABLE_SELFTEST=ON`.
Every capture carries `# tap route: stdout endpoint -> console driver (service list published)`, so
the bytes provably crossed `printf` -> `_write` -> `kos_send(0)` -> endpoint -> service thread ->
SPSC ring -> doorbell -> IRQ thread -> the peripheral's TX register, with nothing left in the
kernel's path.

| board | plan | result | capture (`.session/logs/`) |
| --- | --- | --- | --- |
| `xmc4800-relax` | `1..74` | all pass, 0 skip, 0 partial, 2 boots | `m461c-xmc-uartirq.log` |
| `frdmk64f` | `1..74` | all pass, 0 skip, 0 partial, 2 boots | `m461c-k64-uartirq.log` |
| `esp32c6-wroom` | `1..74` | all pass, 0 skip, 0 partial | `m461c-c6-uartirq.log` |
| `esp32-wroom` | `1..70` | all pass, 0 skip, 0 partial | `m461c-lx6-uartirq.log` |
| `rx72m` | `1..70` | all pass, 0 skip, 0 partial, 3 runs | `m461c-rx-uartirq{,-2,-3}.log` |

- [x] **All five images are ONE CLEAN COMMITTED TIP, `cb5f2a4`.** Every banner names a commit. M4.5.6
      wrote that rule down, M4.6.1's first pass broke it four times out of five, and this pass keeps
      it.
- [x] **The three arms `6be8220` added have their first silicon witness here.** The plans are the
      current ones, so `call_donation_hold`, `call_donation_slow` and `call_donation_pending` ran on
      four ISAs and four enforcement backends. The gap the first pass left is closed.
- [x] **Two capture traps, both caught by the plan count rather than by care -- check the count
      first, always.** One pass built the images WITHOUT `KICKOS_ENABLE_SELFTEST`, so the boards ran `1..58` / `1..57`
      instead of `1..74` / `1..70` -- 70-57 = 13 selftest-only arms on the non-enforcing side, but
      74-58 = 16 on the enforcing one, because those boards also lose the three MPU-gated arms
      (`endpoint_bound`, `grant_reserved`, `dev_window_exclusive`), which is the
      block the `rx72m` stop sits at the start of. The tally is what showed it; nothing about the
      run looked wrong. Separately, re-deriving `sim-telem` into a build dir that already had
      `-DKICKOS_HAVE_MPU=0` cached returned 26 instead of 27, because **`--preset` does not reset a
      cached value** -- the trap `STATE.md` documents, hit while measuring the numbers that go into
      `STATE.md`.
- [x] **The K64F needed a human.** Its first flash hung immediately after `InitTarget()`, the wedge
      its `boards.md` entry already describes, and cleared only after physical intervention. Nothing
      in the image, and worth knowing an unattended bench pass cannot recover from it.
- [x] **Two orphaned `cat /dev/ttyUSB2` readers from a previous session were still holding the
      RP2350 console**, 12 hours on. They did not touch this pass's ports, but that is the
      silent-log-clobber trap sitting armed. `fuser` before every capture; kill recorded PIDs.

### The superseded first pass (2026-08-01)

Kept because it cost bench time, and because the two `2511e20`-dirty boards are the run that found
the RR scheduler bugs: `m461-xmc-schedfix.log`, `m461-k64-schedfix.log` (`1..71`),
`m461-c6-ringfix.log` (`1..71`, `b129a65`, the only clean tip of that pass),
`m461-lx6-writeall.log` (`1..67`), `m461-rx-fixed-selftest.log` (`1..67`, stops at `ok 51`, from a
tree whose banner reads `commit nogit`).

### The first board to run: `xmcuartirq` (2026-08-01)

Capture `.session/logs/m461-xmc-uartirq.log`, image `build/bench-xmc` at `a946a12`(-dirty),
`-DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_uartirq -DKICKOS_HAVE_MPU=1`. Superseded as a
result by the table above; kept for the two findings under it that are still open.

- [x] **`xmcuartirq` WORKS end to end on xmc4800-relax silicon**, under enforcement. The kernel
      banner prints on the polled kernel-owned console, the handover passes SILENTLY (this driver
      has no first-light marker -- see below), and then all ten `[gpioblink]` lines plus
      `PASS (10 cycles, readback ok)` arrive through the ENTIRE new chain: root `printf` ->
      `_write` -> `kos_send(0)` -> endpoint -> service thread -> SPSC ring -> doorbell -> IRQ
      thread -> `TBUF0`. The first `[gpioblink]` line alone proves the privileged `CCR` write
      landed, the line armed, the doorbell fired and the drain reached the wire.
- [x] **The app choice was load-bearing and is worth writing down**: `gpioblink` is the ONLY app in
      this image whose output traverses the published console (5 `printf`, 0 `kos_print`).
      `hello`, `hello_c`, `cxxtest`, `stress`, `blink` and `fp_switch` all use `kos::print`, which
      `console_emit` DROPS while the console is `USER_OWNED`. On this board `KICKOS_CONSOLE=both`
      so those still reach RTT -- but never the serial log, which is exactly how a working driver
      can be mistaken for a dead one.
- [x] **FIXED and witnessed at `c82cc63`: the published console cooks CRLF like the kernel does.**
      Ruled: the console abstraction cooks, the transport does not -- `console_write`
      (`user/include/kickos/sys/uart_service.h`, spelled `console_write_all` then) is the console arm
      and expands, `serve_one`'s WRITE op stays byte-transparent. The original finding
      follows.
      Measured on this capture: the kernel banner ends `\r\n` (`kconsole_write` cooks), every
      driver-carried line ends with a BARE `\n`. On a raw terminal that staircases, and it makes
      output visibly change character the moment the console is published. The cook lives in the
      kernel path only; the userspace service layer passes bytes through. Fix belongs in the SHARED
      layer (`user/include/kickos/sys/uart_service.h`) so one change covers all five, not per
      driver -- but decide deliberately which side owns the cook, because "the driver is
      transparent and the console abstraction cooks" is a defensible answer too. What is NOT
      defensible is the two paths disagreeing.
- [x] **All five IRQ drivers now emit a first-light marker, witnessed on silicon at `c82cc63`.** The polled siblings write
      `[xmcuart] driver up (polled TX)` / `[k64uart] driver up (polled TX)` DIRECTLY to the TX
      register, which proves the window grant, the channel and the TX path before any
      ring/IRQ/doorbell machinery is involved. **`k64uartirq` gained the equivalent at `372e7b4`**
      (`Uart::win_puts`, a bounded TDRE poll with `TIE` still clear so it cannot assert the line,
      emitting `[k64uartirq] device up (IRQ TX/RX)` after `configure()`). `xmcuartirq`, `c6uart`,
      `rxsci` and `lx6uart` print nothing on success, so a silent run on any of those four cannot
      self-diagnose. On the XMC the reason it was held back is real and still applies to the
      ordering: a polled write with `TBIEN` already set raises TB events the first `irq_wait` then
      discards, so the marker must go after `priv_write_verify` and before `drain_tx()`.
      Roughly six lines per driver, and the mechanism is now proven on all four boards.
- [x] **FIXED: `Shared::ready` is now honoured by all six consumers, and the header states the
      ordering as binding.** Three skipped the wait entirely and two more did it AFTER spawning the
      service thread, which forfeits the reportable-timeout half. The original finding follows. (`user/include/kickos/sys/uart_service.h`: `irq_loop`
      sets it, nothing reads it), so its documented purpose -- "the service thread never configures
      a device that is not yet clocked" -- is unenforced. **`ConsoleUart::tx_idle()` is no longer
      dead**: `k64uartirq` calls it, and every one of the five drivers now defines it. The rest do
      not wait for end-of-frame, which is why a panic can still truncate one in-flight character on
      four of the five.
- [x] **REMOVED: `rxsci`'s leftover bring-up trace**, `kos::print("[rxsci] trace: pre-publish\n")`
      (`system/driver/rx72m/rxsci/rxsci.cc`). It is debug residue from the storm hunt, not a
      designed marker, and it should either become a real first-light marker per the item above or
      go.
- [x] **`consoledemo` is silently not built for the uartirq lists.** Its `CMakeLists.txt:17-19`
      hardcodes an allowlist of the two POLLED service lists, and configure prints
      `consoledemo skipped: ... publishes no userspace console`, which is factually wrong for a
      uartirq list. Harmless here because `gpioblink` covers it, but the message misleads.

## M4.6.1 bench debt: three drivers were green by timing margin. CLOSED.

Filed by `ef14ab2`, which fixed the defect in one driver and named the other three.

- [x] **FIXED at `cb5f2a4`, and witnessed on silicon there.** `c6uart`, `k64uartirq` and
      `xmcuartirq` called `kickos::uart::tx_write` for a console write.
      `tx_write` returns what the ring ACCEPTED. A plain send has no reply, so the sender can
      neither be told about a short accept nor retry it -- the retry has to live driver-side or the
      stream is spliced mid-token, a line prefix followed by the prefix of a later line. That is the
      measured `domoook##` signature `ef14ab2` chased on `esp32c6-wroom`. The pump that does it
      right is `kickos::uart::console_write` (`user/include/kickos/sys/uart_service.h`, spelled
      `console_write_all` then), already used by `lx6uart` and `rxsci`.
      **`k64uartirq` and `xmcuartirq` do open-code a retry loop of their own**, which is why they
      had not spliced yet; `c6uart` called `tx_write` bare with no loop at all. All three now go
      through the shared pump: one budget, one doorbell policy, and for `k64uartirq` 10x finer
      retry granularity (200 x 1 ms became 2000 x 100 us at the same ~200 ms ceiling).
- [x] **`design-m4.6-irq-driver.md` section 7.5 said the short accept was "strictly better than the
      kernel ring's overflow behaviour". It is backwards. CORRECTED in the doc.**
      The kernel trades an IRQ-masked stall for the byte; a userspace driver must never mask, so a
      short accept is the only option left to it and correctness moves to the caller. **Believing the
      inverted version is what made three drivers look finished.** The kernel ring is NOT lossless
      either: both poll loops are capped by `DRAIN_POLL_CAP` and a stuck channel drops silently and
      uncounted. Sec.7.5 and `docs/reference/console.md` now state the narrower comparison.

## Named by a commit that fixed something else, and never filed anywhere

Each of these was stated as a known-and-not-fixed caveat in the message of a commit that landed a
different fix. Filing them so they stop living only in `git log`.

- [ ] **`k64uartirq` RX cannot self-recover from an overrun** (`372e7b4`). `OR` blocks `RDRF`, and
      IRQ 32 (UART0 error) is unclaimed, so recovery happens only on the next TX doorbell. Design
      section 7.7 predicts this but says "TX or RX wake"; there is no RX arm, so it is TX-only.
      Invisible on a TX-heavy console, which is exactly why it will surface on the first RX-heavy
      one.
- [ ] **The linker's own `.gnu.warning` output is ignored as noise** (`16662b2`). The missing
      `__getreent` that made every Xtensa stdio call crash was announced by the linker and read past.
      Recommendation as recorded: a `-Wl,--fatal-warnings` gate, or a CI grep for linker warnings.
      Not added.
- [ ] **`microbit` has roughly 800 B of IRQ-binding pool headroom with no assert behind it**
      (`d4898cd`). A future regression there fails at RUNTIME as a mislabelled skip rather than at
      link, which is the failure shape hardest to read. The `bluepill-c8` sibling was right-sized;
      this one was not, because nothing forced it.
- [ ] **The RX72M GROUPBL0 demux is verified correct against the manual and has NO consumer**
      (`a19e484`, `09f02b1`). `rxsci` deliberately uses only the dedicated TXI/RXI vectors, so the
      group path is unexercised even in QEMU. Correct-by-reading is the weakest evidence class this
      project accepts.
- [ ] **`-KOS_ENOMEM` cannot distinguish a full cap table from an empty object pool**, so the
      message is misleading on every board (`8f47990`, which fixed the `xmc4800`/`mk64f` instance
      and left the string alone deliberately). It cost a mis-diagnosis once already: two boards
      skipped `mutex_deadlock` as `SKIP pool too small` when no pool was ever full.

## FIXED: `ktime_rearm` programmed a MOVING deadline (2026-08-02)

The defect: `kernel/time/time.cc` recomputed `now` per call and substituted
`now + KICKOS_TIMER_MIN_DELTA_NS`, and `ktime_rearm` runs on EVERY context switch, so inside the
last 20 us of a deadline every call passed a different value -- which is exactly what every backend
dedups on. Absorbing state; it is the mechanism behind a measured RP2350 hang.

- [x] **The fix**: keep the deadline ABSOLUTE and apply the min-delta floor where the deadline is
      born (`ktime_sleep_until`; `arm_slice` already floored the RR quantum locally), so
      `ktime_rearm` hands `arch_timer_arm` the absolute deadline unchanged.
      Rejected alternative: making the ARM `PENDSTCLR` write conditional -- ARM-only, leaves four
      backends exposed, and does not touch the moving-target property at all.
- [x] **`esp32-wroom` needed an arch fix first, and it was an arch bug rather than a caller
      bug.** `CCOMPARE0` is an EQUALITY match against a free-running `CCOUNT`, not a countdown, so
      a compare value already BEHIND the counter when it lands is not late -- it is MISSED, and the
      next match is a full 2^32-cycle wrap away, about 18 s at 240 MHz, which presents as a hang.
      `arch_timer_arm` floored its delta at ONE cycle, and the handful of instructions between
      reading `CCOUNT` and writing `CCOMPARE` is more than that, so the floor was not a floor.
      Latent until the clamp came out, because a 20 us minimum meant the backend never saw a small
      delta. **Fixed in `arch/xtensa/lx6/arch_xtensa.cc`**: arming is now a loop that writes the
      compare, asks whether the counter has already passed it (signed difference, so it is
      wrap-correct), and widens the margin and retries if so. The symptom it produces is a hang at
      `sleep_order` (`m461f-lx6-uartirq.log`, stops after `ok 12`); with the arch fix in,
      `esp32-wroom` runs 74/74 twice WITH the ktime fix applied. **That fix is landed on its own
      merits** and it is a prerequisite for the ktime fix.
- [x] **`rx72m` is green**: `rr_interleave` reports `rr order: ABABAB` under MPU enforcement, at
      `1..78`. **The arm prints the observed order** (`rr order: ...`) on every run, which is what
      makes an interleave failure diagnosable at all -- the bare predicate reports nothing. Two
      properties of that board bound what a future regression there can be: the dedup cache is
      invalidated by both `arch_timer_disarm` and the timer ISR, and RX's `CMTW0` is a count-up
      compare with `CMWCNT` reset to 0, so it has no equality-miss of the Xtensa kind. RX dedups in
      software on the deadline value (`g_rx_armed_ns`), so a stable deadline HITS and `CMWCNT` stops
      being restarted on every switch.
- [x] **Fleet-wide green.** `xmc4800-relax` and `frdmk64f` run `1..78` clean twice each, all six
      boards pass at `m461n-*`, and sim, qemu, qemu-m3/m7/m33, qemu-riscv and microbit are green.
- [x] **The acceptance test is a REGISTERED ctest**, `tests/unit/ktime/ktime_rearm.cc`: it compiles the
      real `kernel/time/time.cc` against a fake clock and a recording `arch_timer_arm`, so it reads
      the exact value the backends dedup on -- a quantity no on-target arm can see. Mutation-proved:
      restoring the floor to `ktime_rearm` makes the moving target visible -- `got 1017500
      want 1015000`, then `1020000`, `1022500`, one step per simulated switch -- and fails 11 ways.
- [x] **Severity as MEASURED, per arch, and it is why the fix is worth its risk.** ARM destroys a
      latched expiry (unconditional `PENDSTCLR`) but the same call reprograms `RVR` and restarts, so
      one clamped arm costs ~20 us and permanence needs a SUSTAINED sub-20-us re-arm rate rather than
      one event. RX never writes the pending-clear on arm (only `arch_timer_disarm` does), so it
      cannot destroy a latch at all and only loses the not-yet-reached case. Xtensa destroys and has
      no guard. RISC-V and sim have nothing to destroy.

**The other defect from the same hunt**: `KOS_SYS_IRQ_DISCARD` (the EDGE stale-pending hole), which
is independent, green on every gate and every board, and mutation-proved.

## rx72m: the stop is FIXED, and what it was hiding is the real finding

`d2804ce` got the first bytes ever onto the RX72M wire and `fb739fc` collapsed `console_write` onto
the shared pump. The board now runs the whole suite, and the cure is attributed by a reverting A/B.
Captures: old `.session/logs/m461-rx-fixed-selftest.log`, `m461-rx-led.log`; new
`m461c-rx-uartirq{,-2,-3}.log` and the reproducing control `m461c-rx-noflush{,-2,-3}.log`.

- [x] **The leading hypothesis is REFUTED, statically.** It was that `arch/rx/rxv3/arch_rxv3.cc`
      routes TXI on `console_tx_armed()`, so a non-zero `armed` after publish would send every TXI
      into the KERNEL's ISR, which drains the kernel's now-empty ring **and clears `SCR.TIE` behind
      the driver's back** -- one byte per doorbell. Two things kill it. **`console_tx_armed` is a
      `bool`, not a count** (`kernel/init/console_tx.cc`), with exactly two writers: `console_tx_init`
      sets it and `console_tx_deinit` clears it, neither on an ISR path -- so "who decrements it, and
      does the decrement run if the ISR stops being entered" has no referent. **And its value is
      provable from the driver having worked at all**: `irq_claim` refuses any line whose handler is
      not `irq_default_handler` with `-KOS_EBUSY` (`kernel/irq/irq.cc`, whose comment names this as
      what enforces INVARIANT H2), the only thing that restores that default on the SCI6 TXI line is
      the `irq_detach` inside `console_tx_deinit`, and `rxsci` bails out loudly if its claim fails.
      It did not bail -- it served 50 clean lines. Therefore the deinit ran, therefore `armed == 0`,
      therefore every TXI took the userspace arm. **Change nothing at that branch**: the predicate
      and the thing it stands for flip together inside one `IrqLock`, and consulting the IRQ table
      instead would add a load to a hot ISR and break `irq_claim` if the two ever disagreed. Only
      the comment above it is worth touching -- it reads as though `armed` were a count, which is
      what invited this hypothesis in the first place.
- [x] **The 299 B/s figure is not evidence about TX pacing.**
      The 5.43 s window is dominated by the tests' own wall time -- `t_rr`'s burns,
      `t_irq_stale_register`'s 2 ms sleep, every semaphore and endpoint handshake. 1624 bytes at
      wire rate is 141 ms of those 5.43 s. And one byte per doorbell would not produce this
      signature anyway: `console_write` rings on every pass at a 100 us sleep, so one byte per
      doorbell is ~10,000 B/s, 87% of wire rate -- marginally slow but complete, never a dead stop.
      Nor could it produce **50 unspliced lines**: the ring would have filled inside 22 and every
      later line would carry the `ef14ab2` splice signature.
- [ ] **What is actually observed, and it is narrow.** Both captures end mid-string inside a single
      `emitf`: `m461-rx-fixed-selftest.log` at `ok 51`, `m461-rx-led.log` at `ok`. TAP emits a case's
      verdict line AFTER the case body returns, and in the `1..67` numbering #50 is
      `privileged_spawn_refused` and #51 is `irq_thread_ctx`. So **test 51 ran to completion** and
      the stop is in the delivery of its own result line. Two candidates remain, and they are
      different bugs:
      1. **The TX drain wedges** -- `d2804ce`'s own reading, the consumer wedging on the first
         genuinely-full ring. It fits directly: the drain delivers a few bytes of that datagram and
         then stops, and everything pushed afterwards is stranded in the ring. **This is now the
         only candidate with a mechanism**, and it is unproven.
      2. ~~The producer hangs at test 52.~~ **CHECKED AND REFUTED, before it was acted on.** The
         appeal was that `t_irqdrv` (test 52, `irq_as_event`) does
         `TAP_CHECK(drv >= 0); // spawn failure would hang the ready handshake below` and then
         `kos_sem_wait(g_irqdrv_ready)`, and that the image is arena-starved -- it reports
         `heap 16 KiB available` against `30 KiB` on the same board's kernel-console image, because
         `rxsci`'s bring-up costs three threads and a 1 KiB ring block, while the case wants a 4 KiB
         page plus an 8 KiB driver stack. All of that is true **except the premise**: `TAP_CHECK`
         expands to `tap::fail(...)` followed by `return` (`tests/tap/tap.h`), so a failed spawn
         leaves the case, and the handshake below is never reached. That is exactly what the
         comment on that line is for. The suite would have emitted `not ok 52` on a starved arena,
         not gone silent. **Nothing to fix here**, and the arena-starvation observation stands on
         its own as something to watch when this board's image grows.
- [x] **FIXED ON SILICON, AND THE FIX IS ATTRIBUTED BY A/B.** `rx72m` now runs the whole suite
      through the userspace driver: `1..70`, 70 ok, 0 fail, 0 skip, 0 partial, `# all tests passed`
      on the wire, at the CLEAN committed tip `cb5f2a4`
      (`.session/logs/m461c-rx-uartirq{,-2,-3}.log`, three runs, byte-identical at 2221 B).
      The fix is the zero-length-plain-send FLUSH arm below. **Proved, not assumed**: a temp
      worktree at the same tip with ONLY that hunk reverted reproduces the stop, three runs,
      byte-identical at 1716 B, every one of them ending mid-string right after
      `ok 53 - privileged_spawn_refused` -- the SAME test the original `1..67` capture stopped
      after, in different numbering (`.session/logs/m461c-rx-noflush{,-2,-3}.log`).
      Deterministic in both directions, so this is not a timing margin.
- [x] **The doorbell is NOT involved, and no doorbell count changes the outcome.** Bench-measured two
      ways: an image with **zero** doorbell posts truncates identically
      (`rxdb-C-swallow.log` -- see the provenance caveat below), and one with **200 posts at bring-up
      plus three on every write** completes cleanly (`rxdb-S-spurflood.log`). So a spurious
      `kos_irq_notify` at bring-up is not a candidate for this stop, and sec.7.5's
      counting-semaphore argument **stands as written and must not be corrected**.
      **PROVENANCE CAVEAT on those two exhibits, and it is not a small one.** Both banners stamp
      `commit b56ceff` with no `-dirty`, which is impossible: `b56ceff` is master, it contains no
      `rxsci.cc` and no `service_list_rx72m_uartirq.cc`, and it cannot plan `1..72` -- yet both
      captures show `[rxsci] device up` and `1..72`. The mechanism is `cmake/build_stamp.cmake`,
      which stamps `git describe --dirty`; that does NOT flag UNTRACKED files, so a tree whose
      driver existed only as new files stamps clean. **Neither exhibit is traceable to a tree**,
      and the stamp cannot be used to identify either one. The CONCLUSION they support is
      corroborated independently (the byte-exact-prefix measurement below, and the fix landing on
      the flush protocol), so it stands -- but do not cite these two logs as evidence of what a
      given commit did. Any future capture of an untracked-file build must record the tree by
      other means.
- [x] **The truncation was never a wedge and never mid-run.** The short stream is a BYTE-EXACT
      PREFIX of the complete one, short by exactly **511 bytes -- one TX ring's usable capacity**,
      `KOS_UART_TX_SIZE` being 512 and a ring holding `size - 1` (re-measured on the committed
      captures `m461c-rx-uartirq-2.log` vs `m461c-rx-noflush-2.log`, NULs stripped and the TAP
      portion isolated: full 1877, short 1366, common prefix 1366, delta 511 -- which is what lands
      the figure exactly on the ring capacity, so the count has to be taken this way).
      Every test ran and passed. The tail was produced and lost at SHUTDOWN:
      `system/init/common/default_init_run.cc` sends two zero-length plain sends after `main` returns and
      `root_entry` then calls `kos_shutdown`, which masks interrupts and halts. `rxsci` was the one
      driver not implementing that request, so root was released instantly and halted on a full
      ring. The flush arm is the driver's implementation of the protocol, not a workaround. Proved
      by mirror: replacing `console_flush` with a blind 300 ms sleep and NO doorbell restores the
      whole tail.
- [x] **The real defect, which the tip did NOT fix: `service_irq` read `TDRE` before arming `TIE`.**
      The only raise this driver can use is a TDR-to-TSR transfer taken with `TIE` ALREADY 1 (UM
      sec.42.12.2(1) p.2308 -- setting `TIE` afterwards raises nothing). If the in-flight transfer
      completed between the `TDRE` read and the `tx_irq_enable()` at the bottom of the pass, it
      landed with `TIE` clear: no edge, and the pass then armed a source that could never fire.
      Terminal state: ring non-empty, `TDRE` 1, `TIE` 1, no edge left. A RULE T1 violation, and why
      the drain intermittently fell a ring behind. FIXED by arming at the top of each iteration,
      before observing `TDRE`.
- [x] **That defect is a RACE, and 3-of-3 either way is luck**: the same tip measured 0-of-5 in
      another pass. The window is a few instructions against an 87 us byte time, so any scheduling
      shift flips it, and every perturbation (an extra sleep, a polled probe, a raised budget) can
      appear to "fix" it. A 3-run A/B on this defect proves nothing; only the mechanism does.
- [ ] **The probe for the mechanism, and it must not share a channel with the bug.** `d2804ce`
      found the `mark()` TDR probe was itself generating the storm it was measuring, because a TDR
      write with `TIE` armed IS an interrupt. Use `arch_diag_led_set` (P80/PORT8, which shares
      nothing with SCI6 -- `.session/logs/m461-rx-led.log`), not a UART marker. (`RXSCI_TRACE` and
      its `'P'` push are GONE as of the generic-service rework; the warning stands for any
      replacement probe.) The one measurement that settles it: `Shared::stats` already carries
      `irq_wakes`, `tx_bytes` and `irq_spurious`, readable over the endpoint with `KOS_UART_STATS`.
      Run the reverted control, which now reproduces on demand, and read the three counters at the
      stop. Run the `RXSCI_NO_RX=1` control alongside, as `d2804ce` did, so the reading cannot be
      misattributed to RXI6.
      **A reproducible failing image is the asset this leaves behind**: build the tip with the
      `if (n == 0)` arm removed from `rxsci`'s service loop and the stop returns, every time.
- [x] **`rxsci` was the only driver that did not treat a zero-length plain send as FLUSH. FIXED.**
      `system/driver/rx72m/rxsci/rxsci.cc` takes `if (info.reply_cap < 0) { console_write(...); }`,
      which for `n == 0` is a `tx_write` of nothing plus a doorbell. `k64uartirq`, `xmcuartirq`,
      `c6uart` and `lx6uart` all special-case it to `console_flush`. **Consequence**: root's
      zero-length shutdown flush (`09fafd9`) does not drain `rxsci`'s ring, so the tail of a clean
      run -- including `# all tests passed` -- is lost on this board even once the stop is fixed.
      Three lines, mirroring the `k64uartirq` arm.
- [ ] **The TX drain loop re-reads `SSR` immediately after a posted `TDR` write** (`rxsci.cc`). The
      same file's `tx_irq_disable` documents, with three manual citations, that an RX I/O write is
      POSTED and that a read-back is mandatory before the next instruction may rely on it -- and
      this loop relies on exactly that, expecting `TDRE` to have gone to 0. If the `SSR` read can
      retire first, the loop pops a second byte and overwrites `TDR` before the first reached `TSR`:
      silent byte loss. Same-peripheral read-after-write is very likely ordered on this bus, so this
      is **unproven** -- but it is the identical hazard class the file already treats as mandatory,
      and the guard is one free read.

## Two kernel defects, found and understood: BOTH FIXED (2026-08-02)

Both were found while chasing driver behaviour, and both were kernel-side, so they outlived
M4.6.1. Each landed with a gate that is RED without it.

- [x] **FIXED 2026-08-02. `ktime_rearm` re-derived the deadline it programmed, defeating every
      arch dedup guard at once.** The min-delta floor moved from `ktime_rearm` to
      `ktime_sleep_until`, where the deadline is BORN and the floor is applied once against one
      clock reading, so `ktime_rearm` hands `arch_timer_arm` the absolute deadline unchanged.
      `arm_slice` already floors the RR quantum locally, so the storm protection the old
      guard was credited with never depended on it. Proven on all six boards (`m461n-*`) and by the
      REGISTERED `ktime_rearm` ctest, mutation-proved. The `CONFIG_SCHED_PERIODIC_TICK` arm has the
      SAME shape (`now + period`, recomputed per call) and would want anchoring in a file-static
      deadline; that half is unproven either way -- no preset builds it.
      **The RX half, mechanism first**: the clamp made every call pass a different
      `deadline_ns`, `arch/rx/rxv3/arch_rxv3.cc`'s software guard therefore never hit, and the
      arm sequence unconditionally rewrites `CMWCNT = 0`, so under a switch rate faster than
      20 us CMTW0 never reaches the compare. An early fire is the other sub-case, and the
      difference is a register: RX's `arch_timer_arm` does NOT clear
      `ICU.IR[CMWI0]` (only `arch_timer_disarm` does), so an expiry that DID latch survives every
      subsequent arm and fires the instant `PSW.IPL` drops. ARM has no such immunity --
      `ICSR.PENDSTCLR` is written on every reprogram. Per-arch severity as MEASURED:
      **RX** loses only the not-yet-reached case; **ARM** loses that AND the latched
      one, but a single lost arm costs at most one min-delta (the same call re-arms 20 us out),
      so permanence needs a SUSTAINED sub-20-us switch rate rather than one event; **Xtensa** has no
      dedup guard at all and `wsr.ccompare0` clears the pending match, so every rearm both
      destroys and reprograms -- self-healing only because it reprograms the REMAINDER, which is
      exactly what the clamp took away; **RISC-V** (absolute `mtimecmp`) and **sim** (POSIX
      `TIMER_ABSTIME`) have nothing to destroy and suffer only the pushed compare.
      **Rejected: making `PENDSTCLR` conditional.** It is ARM-only, it leaves RX, Xtensa, RISC-V
      and the sim exposed, and it does not touch the moving-target property that is the actual
      root cause -- the countdown restart would survive it untouched.
      **Gate:** `tests/unit/ktime/ktime_rearm.cc`, which compiles the
      real `kernel/time/time.cc` against a fake clock and a recording `arch_timer_arm` and reads
      the exact value the backends dedup on. Host-only of necessity: that value never crosses the
      arch seam, so an on-target arm can only time a wake. The end-to-end symptom WAS reproduced
      on the sim (two yielding peers, ~5 us/switch, a 1 ms sleep measured at 1.35-10.8 ms against
      1.03-1.78 ms fixed) but is not landed: the starvation ends at the first inter-switch gap
      above 20 us, so the reading is host-jitter-dependent and the arm would be silently vacuous
      on a loaded machine.
- [x] **A tier-1 EDGE line could not clear a stale pending from userspace. FIXED by a new verb,
      not by changing rearm.** `rearm_locked`'s first-arm-only clear is deliberate -- the
      coalesce contract needs it -- so `KOS_SYS_IRQ_DISCARD` / `kos_irq_discard(irq_cap)` was
      added instead: `CAP_WAIT`, the same chokepoint as wait and ack, touches the controller's
      pending state and nothing else (no mask, no unmask, `needs_rearm` untouched). See
      `docs/design-m4.6-irq-driver.md` section 5.3. **Gate:** selftest `irq_discard`, the exact
      inverse of `irq_mask_coalesce` against the same three back-to-back raises -- one service
      where coalescing gives two. Mutation-proved by emptying the syscall.

## The capability table: stages 0, 1 and 2 are LANDED; Stage 3 is the elastic storage

**SUPERSEDED in mechanism by the capability-table rework, which re-derived the table from a clean
sheet and DELETED the storage shape recorded below.** Gone: the fixed bucket classes (runs of
4/16/64, each with its own free list), the per-spawn declared capacity and its ABI field, the
narrow-only clamp against the spawner, `Thread::cap_class`, the per-board default spawn-capacity
knob, and `KICKOS_MAX_HANDLES` as a board knob -- so `KCAP_INDEX_BITS` no longer derives from it
either. In their place: a fixed `CapRun caps` member in `Thread` with no capacity and no class id,
ONE uniform chunk size with no classes, `thread_cap_capacity` returning the one image-wide
`KICKOS_MAX_HANDLES` for every task that holds a run (and 0 for idle, which holds none), and a
configure-time width sum (`cmake/cap_table.cmake`). **The one image-wide width was repealed by
M4.7.3**: root keeps the sum, a spawned task gets `KICKOS_CAP_CHILD_WIDTH`. See `docs/design-capability-table.md`
section 3 for the deletions and section 6 for the provisioning that replaced them. Everything
below is kept as the record of what was built and why, checkboxes included; read it as dated
history, not as the current shape.

A design spike and an adversarial review of it ran on 2026-08-01. **Both were EXPLORATORY and are
squashed out of this branch's history per `docs/README.md` -- spikes never reach master.** Their
durable teaching is Book chapter 8.7
(`docs/book/stale-handles-generation-width-and-allocation-policy.md`). Everything below is what
survives as *work*, restated with the review's corrections already folded in, so nothing here
depends on a document that no longer exists.

**The question.** The per-task capability table is a fixed array inline in the TCB, so the image
pays `(KICKOS_MAX_THREADS + 2) x KICKOS_MAX_HANDLES x sizeof(CapEntry)` whether a slot is ever
seated or not, sized by the most demanding task in the image. The `+ 2` is real and easy to miss:
`kernel/init/kmain.cc` declares idle and root as file-static TCBs outside the pool, and both carry a
full table. On every MCU in the fleet the arena begins where `.bss` ends, so shrinking that array
hands the bytes straight back to the thread-stack arena. Two things are welded together that should
not be: **the ceiling** (how many capabilities one task may hold) and **the cost** (how many bytes
the image pays for the possibility).

**Demand is genuinely uneven, counted from the tree rather than assumed**: a polled console driver
holds 1 capability, an IRQ-driven UART or SPI service task 2, the three-task `rxsci` 2 per task, a
plain worker child 1 to 2 -- and the selftest's deadlock case holds 6 and spawns children at 5
grants. Against that, an image provisions 7, 10 or 11 slots for **every** task, almost entirely to
hold one case in one build.

### What is landed

- [x] **Stage 0 -- stop paying for reserved slots nothing seats** (`264beae`). `KOS_CAP_AUTHORITY`
      named no pool object, held no refcount and never bumped a generation: it is now
      `Thread::authority`, a `uint8_t` in the EXISTING padding after `privileged`, which costs zero
      (placing it before `handles[]` instead cost 4 bytes of padding per TCB and ate a quarter of
      the saving). The authority cap's own slot and the reserved spare were explicitly for
      nothing once the word moved to the TCB; both deleted, `KICKOS_CAP_FIRST_DYNAMIC` lowered
      4 -> 2. `KOS_CAP_CLOCK` was NOT deleted and still holds index 1: it has no writer yet
      because it is a provision for a userspace CPU governor, whose authority bit is
      `AUTH_PSTATE`. **Not a flag day, and that was
      checked before it was relied on**: all 19 `KOS_SPAWN_DELEGATED_CAP0` sites express the rule
      through the constant, none hardcode a literal, and the constant never read
      `KICKOS_CAP_FIRST_DYNAMIC` anyway. Measured on linked ELFs: `microbit` and `bluepill-c8` -64 B
      each, `frdmk64f` -432 B. A functional gain nobody costed came with it -- the `cap_count >= 2`
      spawn refusal is gone, so a child taking an authority word is no longer limited to one
      delegated capability.
- [x] **Stage 0's other two items landed earlier and separately**: the `uint8_t` refcount
      `static_assert` became a runtime `-KOS_EOVERFLOW` refusal at the single increment site
      (`4bf7362`, and it found that the deleted assert's own arithmetic was wrong), and
      `kos_irq_claim` stopped reporting a full cap table as `-KOS_EPERM` (`cddd045`).
      **`4bf7362`'s refusal has no selftest case and cannot have one on this fleet** -- the maximum
      live refcount reachable on any board is 253 of 255.
- [x] **Stage 1 -- lift the ceiling and decouple spawn** (`264beae`). `KCAP_INDEX_BITS` is derived
      from `KICKOS_MAX_HANDLES` with a floor of 4; the floor is not arbitrary, since the generation
      is capped by its `uint16_t` storage rather than by the remainder, so narrowing below it buys
      nothing and would renumber every handle on the small boards. **The sign boundary is 15 index
      bits**, pinned by `static_assert` and proven by build rather than by comment: 32767 builds,
      32768 is refused (it collides with the `KOS_CAP_AUTHORITY` pseudo-handle, `INT32_MAX`), 65536
      is refused (negative in `int32`). `KICKOS_MAX_SPAWN_GRANTS` (default 6, max in-tree use 5) now
      sizes the four caller-stack arrays in `kernel/syscall/syscall_thread.cc` that were sized by
      `KICKOS_MAX_HANDLES` -- 84 B instead of 126 B today, and instead of 28 KiB at a 2048 ceiling.
      **That decoupling is what makes any ceiling lift safe at all.**
- [x] **Stage 2 -- bound the two interrupt-masked windows** (`6be8220`), ruled MANDATORY rather than
      deferred because Stage 3 is what makes them bite. `thread_effective_prio` **no longer touches
      `handles[]` at all**: reply donors come from a new intrusive `HeadList` on the TCB (the caller
      links in via its own free-list node while reply-parked) and served endpoints from a sweep of
      the endpoint pool for `server == t`, which needed no new storage because `Endpoint::server`
      was already the back-pointer. `cap_teardown` takes and releases its own `IrqLock` every 4
      slots, with a new `bool Thread::dying` as the in-teardown marker -- `state` could NOT be the
      marker, because the sweep drops the lock and a switch back in rewrites it to `RUNNING`. Two
      repairs fell out: `domain_release` moved BEFORE the sweep, because the "same critical section
      as the EPIPE-wake" argument is unavailable under chunking and ORDER has to replace atomicity;
      and the console reclaim is gated on a teardown-active COUNT rather than a flag, because two
      sweeps can overlap. Chunk 4 was pinned by `static_assert` BELOW `kcap_smallest_class_slots()`
      (that helper and the whole capability-class mix were deleted in M4.7.1; neither name exists)
      -- Stage 3 re-expressed the bound against the smallest bucket class rather than against the
      fleet's smallest `KICKOS_MAX_HANDLES`, because a sweep is bounded by the RUN a task was
      given. At 16 the whole fleet would take one chunk and the preemption path would be dead code.
      **Both defects were real but only prospectively**, bounded by today's small knob. The decisive
      figure was waste, not latency: across a full run the funnel visited 405,504 table slots to
      find 20 donors, never more than 2 on a single call; teardown visited 339,968 to release 189
      caps, never more than 5 per dying thread. At N=4096 on a 16 MHz `microbit` that is 4.1 ms and
      2.8 ms of interrupt-masked time -- what Stage 3 would otherwise have shipped into an RTOS.
      **This satisfies the review's amendment 5 in full**, which required a preemptible teardown
      sweep with a DYING table state AND a funnel donor structure before any large bucket class
      could be offered. Both now exist, so Stage 3 is unblocked.

### Stage 3 -- the storage itself. LANDED 2026-08-02.

**The design, honestly named: spawn-declared capacity over a partitioned slab.** ("Elastic" was the
spike's word and it over-claims. On an allocator-free kernel every design is a static bet -- a
2048-slot bucket is 16 KiB of `.bss` on every boot of that board, subtracted from the stack arena,
claimed or not. What this actually buys is the **unwelding of the per-task ceiling from the per-task
cost**, which is the thing that was asked for.)

The array stays a contiguous run with a task-relative index, but it leaves the TCB. The control
block keeps a pointer, a capacity and a bucket id. The parent declares the child's capacity in the
spawn parameters; the kernel takes a run from a slab and attaches it after create. The slab is
**statically partitioned into fixed bucket classes** -- some runs of 4, fewer of 16, one or two of
64 -- each with its own free list. No splitting, no coalescing, so no fragmentation and the refusal
is always truthful.

**Why this shape and not the alternatives.** A global table with per-task ownership and a quota has
strictly better create and teardown costs and exact elasticity, but it breaks
`KOS_SPAWN_DELEGATED_CAP0` -- the frozen guarantee that delegated capability *i* lands at child
index *i+1*, and that because a fresh child table has generation 0 the child's handle value **is**
*i+1*, known a priori with no handoff. Downstream of that: the generic bring-up helper, all three
service protocol headers, every driver under `system/driver/`, nine in-tree apps and well over a
hundred selftest assertions. That is the flag day, and it is why the global design is not the
choice. An inline-plus-overflow-chunks design puts a chain walk or a second dereference on the
resolve path of every capability-bearing syscall, forever. **The decisive property of the chosen
shape is that its runtime path never touches a shared resource**: the slab is touched at spawn and
at exit only.

**Requester-pays is kept as ACCOUNTING, not as storage origin.** Funding the table from the task's
own memory budget (the Tock shape) does not compose here. `domain_for`
(`kernel/domain/domain.cc:198`) dedups an identical base into ONE shared domain, so where a purse
exists at all it is per-**domain** and writable by every member -- carving per-task kernel state out
of a region other tasks can write is unsound on sharing grounds before MPU geometry even enters.
And the geometry is against it too: on the power-of-two backends (PMSAv7, RISC-V NAPOT) shaving
kernel state off the top of a granted region forces the remainder down to the next lower power of
two. A design whose isolation property is arch-dependent is the worst outcome. **Revisit at M6**,
when a page is the granule and the shape becomes available without the tax.

**THE GATE CORPUS WANTS A LOOK AT M7, and M7 does its own homework on it.** No analysis is
pre-chewed here on purpose. The one datum worth not re-deriving: M5.2.1 added roughly ten lines
of gate per line of behaviour, and about a third of the largest gate is DECLARATION
(`tests/static/trap_redzone_roots.txt`, `trap_redzone_indirect.txt`) rather than logic, with
three of those declarations numerically inert today by their own mutation testing. Whether that
is proportionate is the question; the growth is not uniform, one 185-line gate covering a whole
escalation class while another 1563 lines cover one number per backend.

**A PER-THREAD KERNEL STACK IS NO LONGER SCHEDULED HERE. RESEQUENCED to M5.2.1** (2026-08-20 to the then-M5.2.2, folded into M5.2.1 on 2026-08-21) after an external audit pass; the reasoning below is kept because the pow2-tax and arch-dependence arguments still hold, but they lost to the fact that every finding of that pass was red-zone arithmetic composition in a scheme M5.2.1 deletes. What follows is the ORIGINAL note, VERBATIM: its milestone numbers predate the 2026-08-21 swap, when M6 meant SMP and M7 meant the MMU. Both moved, so inside this note read "M6" as multicore (now M7) and "M7" as page tables (now M6).

**(superseded) A per-thread kernel stack was scheduled here, decided 2026-08-20, and NOT at M6.** The
syscall contract (`arch/include/kickos/arch/arch.h:512-518`) requires dispatch to run in privileged
THREAD context on the calling thread's own continuation; it does NOT require the user's stack. The
user's stack is simply the zero-memory way to get a per-thread continuation, and the cost is that
the kernel's own frames live at addresses the user names, so isolation rests on an arithmetic bound
rather than on the memory map. M5.2.1 found that bound wrong at the low edge on all three archs at
once, which is the argument for eventually not needing it. A per-thread kernel stack satisfies the
same contract, costs one extra stack times `KICKOS_MAX_THREADS` (16 by default, fatal on
bluepill-c8's 20 KB, comfortable on the K64F), and pays the pow2 rounding above until a page is the
granule. SMP does NOT force the question: with a big lock each core runs its own thread's kernel
path on that thread's own stack, so nothing contends. What M6 was to inherit was narrower, making
the single `g_rv_trap_stack` per-core, and **M5.2.1 ALREADY DID IT**: the array is
`g_rv_trap_stack[KICKOS_NUM_CORES][KICKOS_RV_TRAP_STACK_SIZE]` indexed by `arch_cpu_id()`, which
folds to `0u` by preprocessor at one core and is gated by `check_cpu_id_fold.sh`. So M6 inherits a
substitution rather than a change. The `-fstack-usage` margin gate M5.2.1 adds for the bound becomes
the kernel-stack SIZING gate here, so that work transfers rather than being thrown away.

**The items.** Renumbered from the spike, and item 3 below is CORRECTED against what `6be8220`
actually did.

**What landed, and what it actually cost.** The default is BEHAVIOUR-identical -- one class at
the full ceiling, one run per possible task -- so no board's refusal points moved. It is NOT
byte-identical: measured on `hello` at `-Os`, the mechanism
costs **+196 B of text and +8 B per TCB** (a pointer, a capacity and a class id), against a slab
exactly the size of the array it replaced. A board only comes out ahead once it declares a mix.
Two do, and they are the two that can be RUN:

| tree | mix | selftest `.bss` before | after |
| --- | --- | --- | --- |
| `qemu` (mps2) | 8x17 + 10x2, default 8 | 74,708 | **74,516** (-192) |
| `sim` | 6x10 + 10x8 | -- | the fleet's other witness |

**The saving comes from the DEFAULT, not from the classes.** With the default capacity at the
ceiling every spawn asks for the largest class and a mix buys nothing at all -- that was measured
the hard way before it was understood. Root needs the full table; workers hold 1 to 3 caps.

**The 64 KiB parts pushed back, and so did microbit.** `stm32f103` and `stm32f302` overflowed FLASH
by 88 and 80 bytes; three separate reductions got them back (loops bounded by the live class count
rather than the array bound, a `memset` for the run zero, and folding the spawn's duplicated unwind
into the existing one), and the last 48 bytes came from excluding the new `cap_capacity` arm on
those two chips by name. microbit excluded it too, for a different resource: the arm's three
file-scope result globals came out of its 16 KiB arena and flipped `mem_self_grant` from RUN to
SKIP -- **the third time in this milestone** that a shared test's statics starved that board's
shape-specific probes, and the second time after writing the warning down.

- [x] 1. Replace the inline array with a pointer, a capacity and a bucket id; attach after create.
      `kernel/thread/thread.cc` zeroes the whole TCB at create, so the attach must happen after.
      **Handle idle and root explicitly** -- they are file-static, not pool slots, exactly as
      `kmain.cc` already has to seat root's authority by hand. Idle can take a capacity of zero.
- [x] 2. The bucket slab plus per-class free lists, in the style already used for reclaimed task
      stacks (a size-class free list threaded through the free blocks themselves is house style;
      a general variable-size allocator with coalescing must be REFUSED, because fragmentation
      turns "refuse when full" into "refuse while space exists").
      Zero the run at attach; the generation words may be left alone, which is cheaper and strictly
      safer than clearing them.
- [x] 3. **Re-bound the scan sites on the per-task capacity. There are THREE, not four.** The spike
      listed the effective-priority funnel in `kernel/sync/sync.cc` among them; `6be8220` removed
      that term entirely, so **the funnel is not "re-bounded" -- it no longer reads the capability
      table at all**, and the invariant to preserve is exactly that: no term of the
      effective-priority funnel may read `handles[]`. What remains to re-bound is `cap_install`,
      the free-slot probe, and `cap_teardown` -- and teardown's chunk loop must now be derived from
      the per-task capacity rather than from `KICKOS_MAX_HANDLES`, with the `static_assert` that
      pins `KCAP_TEARDOWN_CHUNK` below the fleet minimum re-expressed against the smallest bucket
      class.
- [x] 4. A new spawn field for capacity, narrow-only, defaulting to a board knob so every existing
      app is source- and behaviour-compatible. A plain app sees nothing new.
- [x] 5. Per-board bucket mixes, starting from the demand counts above rather than from a guess.
- [x] 6. **Define the storage seam now** -- attach a run, detach a run, per-class accounting behind
      it -- so the M7 page backend can replace the slab under an unchanged interface. The slab is
      scaffolding and should be named as such.

**The two decisions the review left open are TAKEN (maintainer, 2026-08-02).** Both are recorded
with the consequence that comes with them, because each was chosen against a real alternative.

- [x] **Capacity is a CEILING, narrow-only. Not a conserved budget.** Child capacity <= parent
      capacity; each child's capacity is fresh and nothing is conserved. **The consequence, stated
      plainly rather than discovered later**: narrow-only bounds WIDTH, never total consumption --
      a parent of capacity 32 may spawn any number of 32-children -- so **the per-class bucket
      counts are the ONLY wall against slab drain**, and hazard 3 must be read that way. Root is
      therefore provisioned at the fleet maximum and every ceiling in the system is root's.
      The alternative (Genode's conserved, transferable quota with totals invariant) would close the
      drain structurally, and it was declined for cost: a new bookkeeping plane, a transfer
      protocol, a return-on-death path and more ABI surface. **Revisit it only if a measured drain
      appears**, not on principle.
- [x] **Refuse, never spill.** A request whose own class is exhausted is refused even when a larger
      class is free. This keeps the per-class count the single meaningful knob, keeps every refusal
      attributable to exactly one number, and keeps refusal order-independent and auditable per
      board at configure time. **The price, accepted knowingly**: the system can refuse while slab
      bytes sit free -- hazard 4's cousin at class granularity. Spilling up was declined because it
      lets small spawns eat the large buckets, so the 32-cap server the large class exists for is
      the one that ends up unable to spawn, and the refusal stops pointing at any single knob.

**The hazards, so a later pass checks rather than re-derives.**

1. **Runtime pool drain** -- a task creating capabilities in a loop empties a shared pool and every
   other task's next create fails. **Does not apply here**: the runtime path never touches the slab.
   This is the single strongest argument for the chosen shape.
2. **Drain by proxy through the reply mint.** Both reply-capability mint sites write the
   **receiver's** table (`kernel/syscall/syscall_ipc.cc`, the call fastpath and the recv slowpath);
   nothing ever installs into a sender's. So a task's capability consumption can be driven by a
   peer's syscall -- but only for tasks that recv with a non-zero info pointer, only by their own
   clients, and both sites probe before committing -- with `cap_has_free_slot` as written, and with
   `cap_can_take_reply` since M4.7.3 replaced it. Under this
   design that consumption lands in the server's own declared run and nowhere else. **The current
   exposure is preserved exactly, not widened.**
3. **Spawn-time slab drain.** Real, and the mitigation is NOT "spawn is authority-gated" -- it
   isn't. `KOS_SYS_THREAD_CREATE` dispatches with no authority check
   (`kernel/syscall/syscall.cc`); inside `thread_create_call` only the privileged flag, the MMIO window
   (`AUTH_MEMORY`) and the authority word are gated, so any task can spawn an unprivileged default
   child. The mitigations that actually hold: every spawned child costs a TCB slot, so drain is
   bounded by `KICKOS_MAX_THREADS`; the same ungated actor can already deny every future spawn by
   exhausting the thread pool and the stack arena, which is a strictly worse denial; and with fixed
   classes and no splitting, **refusal is order-independent** -- a spawn refuses if and only if
   concurrent same-class demand exceeds the static count, which is auditable per board at
   configuration time. A heap gives no such statement.
4. **Fragmentation as a false refusal.** Avoided by fixed classes. If a first-fit bitmap over one
   flat slab is ever chosen instead for code simplicity, this hazard returns and must be measured
   rather than assumed away.
5. **Refcount saturation as a cross-task channel.** Now that the compile-time bound is a runtime
   refusal, tasks delegating many capabilities to one object can drive that object's count to its
   ceiling and make an honest delegation fail elsewhere. Bounded and typed, but it is a shared
   counter. A wider counter costs one byte per pool slot if it ever matters.
6. **Structural wall versus policy wall.** Under the chosen shape the storage boundary IS the
   defence and the arithmetic cannot be skipped. Under a global table the whole isolation story is
   one quota field, with no second line. That asymmetry matters more than the byte counts.

**What would make this wrong.** The demand counts above are a static read of the tree, not a runtime
measurement -- they say what the current apps hold, not what a peak looks like under a load nobody
has run, and no high-water mark exists. If a workload appears that needs capacity elastic *in time*,
this is the wrong shape. If the delegation packing is being reworked for another reason anyway, the
global design's costs are better on every axis except hazard 6. And at M7 the whole storage-origin
question should be re-argued from scratch rather than inherited.

**Refused, and recorded so it is not re-priced**: narrowing `CapEntry` below 8 bytes, in any form.
the reply encoder packed a 24-bit generational thread handle plus an 8-bit call sequence
(`kernel/syscall/syscall_ipc.cc`), a word that is routinely NEGATIVE -- which is why
`cap_reply_caller` decodes with masked rather than arithmetic shifts. An `int16_t obj` therefore
**cannot represent a reply capability at all**, and a 16-bit re-cut leaves roughly 7 bits of THREAD
generation, the one counter with cross-task, holder-uncontrolled retention. A 6-byte AoS entry is
only reachable by narrowing `obj`, since any struct retaining a 4-byte member pads back to 8; the
only honest alternative layout is structure-of-arrays, which is the one variant worth revisiting if
the bytes ever genuinely matter.

## Capability Stage 4 -- per-grant destination indices (2026-08-02)

Chosen over the other two recorded deferrals (runtime capacity growth, which reopens hazard 1; and
the conserved budget, which reverses the ceiling decision). This one was already blocking something
real: under default placement the FIRST delegated cap lands at child index 1, which is
`KOS_CAP_CLOCK`'s well-known index, so a parent could not hand a child a capability without aliasing
a reserved name -- and the narrowed hand-off to a driver manager is recorded as waiting on exactly
this. It also retires the trick of delegating a throwaway capability purely as a SPACER to push a
later one onto the index the child expects.

- [x] **`kos_thread_params::cap_dest`**, an OPTIONAL array of `cap_count` bytes parallel to `caps[]`.
      Null, or a 0 entry, means default placement (`KOS_SPAWN_DELEGATED_CAP0 + i`).
- [x] **A parallel array rather than a third field in `kos_cap_grant`, and the reason is the common
      path.** The field was written first and reverted: `-Wmissing-field-initializers` under
      `-Werror` made every one of the ~30 brace-initialised delegation sites in the tree spell a
      trailing `0` that means nothing, forever, on the overwhelmingly common path. Explicit
      placement is the rare case, so it is the one that pays. The frozen 8-byte `kos_cap_grant`
      stays frozen as a side effect.
- [x] **0 is a safe sentinel for "default"** and costs no expressiveness: index 0 is the kernel's
      stdout slot and `cap_install_at` refuses it outright, so 0 was never a legal destination.
- [x] **Validated before the child exists**, so a bad list costs `-KOS_EINVAL` and never a
      half-built thread: no two grants may land on the same index counting the defaulted ones
      (O(n^2) over at most `KICKOS_MAX_SPAWN_GRANTS`), and the destination array is snapshotted into
      kernel memory with the same double-fetch discipline as the grant array.
- [x] **The bound is the run the child ACTUALLY receives, not the capacity the spawn declared, and
      the first attempt got this wrong.** Validating against the request refused `capq_run(1)` --
      a legitimate ask of 1 that rounds up to the smallest class -- because the default destination
      1 was not below the declared 1. The child's table really is the granted run, so the check
      moved to just after `cap_slab_attach`, before any reference is taken, where the only unwind
      owed is the run itself.

- [x] **The spacer-cap workaround is retired**, which is what this stage existed to remove.
      `t_irq_reclaim` delegated `g_done` a SECOND time purely to push the line cap onto
      `CH_IRQ`; it now names `{CH_DONE, CH_IRQ}` directly, leaves index 2 empty, and takes one
      fewer reference. That also makes the positive path run on every board.
- [x] **`cap_dest` arm, mutation-proved three ways.** Two refusals (two grants on one slot, and a
      destination past the child's table) plus a collision against a DEFAULTED entry, all with no
      worker and no file-scope state -- both bad spawns are refused before a slot is claimed, so
      neither ever runs.
      **The first version of the arm was not falsifiable and the mutation said so.** Removing the
      destination from the install site entirely -- placing every cap at `i + 1` again -- left it
      GREEN, because the refusals do not depend on where a cap lands and `t_irq_reclaim`'s worker
      ignores the return of its `kos_irq_ack`. The arm now delegates the completion semaphore at
      index 3 with nothing at 1 or 2: ignore the destination and the cap lands at 1, the worker's
      post never lands, and root is never released. That surfaces as a TRUNCATED RUN rather than a
      `not ok`, which is cruder than an assertion and is the deliberate trade -- the alternative
      wants a report channel, and two file-scope words is exactly what starved microbit's arena
      twice in this milestone. There is no `sem_trywait` to do it more neatly.

## Found during the M4.7 cap-rework pass (2026-08-03)

Filed together because one pass turned them up, not because they share a fix: two
scheduler/teardown defects found root-causing the intermittent `sim_stress` failure, two
capability-plane findings from re-reading the reserved indices and the per-board sizing, and one
consumer-facing build limitation that is NOT M4.7 scope. Each says what it rests on and whether it
was read or measured.

- [ ] **Out-of-tree boards are not supported, and that contradicts a stated principle.**
      `cmake/kickos.cmake:30` derives `KICKOS_BOARDS_DIR` from `<repo>/boards` with
      `get_filename_component` -- no cache variable, no override -- and
      `kickos_load_board_descriptor` (`cmake/kickos.cmake:55-73`) has exactly three outcomes: an
      in-tree `boards/<board>/board.cmake`, or, only where `KICKOS_IN_TREE` is FALSE, the single
      board the installed package was built for, or `FATAL_ERROR`. The provisioning path
      no longer looks for a board directory at all: M4.7.5 deleted the per-board headers, so the
      values come from `boards/<board>/configs/<variant>/defconfig` and the only in-tree search
      left is the chip's own include dir. That does not change the conclusion here, because a
      consumer still cannot supply a board without editing the KickOS tree or carrying a patch
      against a vendored copy, while `docs/book/README.md:26` says porting a CPU is "the
      small arch/chip seam, not a kernel restructure". Shape of the fix: make the boards search
      path a LIST a consumer can extend, and let the board include-dir lookup search the same
      list. NOT M4.7 scope -- recorded so it does not evaporate. Read directly.
- [ ] **Round-robin refunds a full quantum after a long preemption.**
      `kernel/sched/policy_fifo_rr.cc:114-127`, `policy_on_switch_in`: a slice survives the switch
      only while `slice_deadline_ns` is still in the future, so a preemption LONGER than the
      remaining quantum falls through to `arm_slice` and the thread is granted a fresh full one.
      The stress app runs RR at a 300 us quantum (`user/apps/common/stress/main.cc:216`) and
      preemptions on a loaded runner routinely exceed that, so round-robin degenerates toward FIFO
      in exactly the environment the preserve-the-slice fix was written for. It covers short
      preemptions only. Read directly; not observed as a failure.
      **Reconciled 2026-08-06 against `docs/reference/invariants.md:172`
      (`rr-quantum-is-wall-clock`), which states this case as INTENDED**: the quantum measures
      wall-clock and not CPU time, and CPU-time accounting was refused by name because it needs an
      `on_switch_out` hook plus a `slice_left_ns` field and weakens the peer-latency bound to
      "eventually". That paragraph is byte-identical at `0fb3ba6`, so it PREDATES this entry. Not a
      defect to fix, then, but an over-broad claim to bound: the invariant promises no
      equal-priority peer waits longer than one quantum of REAL time, and with a preemption longer
      than the remaining quantum the peer's actual wait is preemption plus quantum.
- [ ] **The concurrent capability-teardown path is never exercised.**
      `kernel/include/kickos/cap.h:326-328` states that an RR slice expiring in `sched::tick_rr` is
      the only thing that switches a dying thread out at a chunk boundary, and so the only way two
      threads are ever inside `cap_teardown` at once. Instrumented counters put that path at **zero
      hits** across the whole suite, including with the quantum cut to 25 us. Forced (a spin in the
      chunk gap plus every churner made RR) it takes 10402 hits with 7 concurrent sweeps and the
      suite still passes, so the design appears sound and nothing guards it against regression. That
      leaves `g_cap.teardown_depth` and `cap_teardown_active()` (`kernel/syscall/cap.cc:52`, bumped
      `:821`, decremented `:872`)
      and the deferred console-death reclaim that reads it (`kernel/sched/sched.cc:209`) untested by
      anything in-tree. A restructuring that removes the chunked window would DELETE the question
      rather than answer it, which is worth deciding deliberately rather than by side effect.
      **Measured by a subagent with instrumented counters -- the zero-hit figure and the forced-path
      figure are both its numbers, worth re-deriving before acting on them.**
      **NARROWED by `tests/unit/capsweep/`**: the host gate now drives one sweep into another and
      pins `teardown_depth` as a COUNT plus the deferred console reclaim that reads it, so what is
      left of this entry is the ON-TARGET hit count, which is still zero. A restructuring that
      removes the chunked window is the S5 mutant of
      `docs/design-m4.8.2-host-unit-tests.md` section 8.6 and now fails a named arm.
- [x] **FIXED in M4.8.2. `sched::wake` dereferenced `kernel().current` unguarded** while `tick_rr`
      in the same file guarded the same pointer, and `kernel().current` is null between
      `sched::init` and `sched::start`. Latent rather than live (no reachable pre-start waker was
      found); the asymmetry was the defect. The null test is the first of the guard's three clauses,
      and deleting it SIGSEGVs the `sched_wake` gate. That arm constructs the null by hand, so it
      proves the clause is EXERCISED, not that the defect was reachable.
- [ ] **`KOS_CAP_CLOCK` is aliased by default spawn delegation, so today it reserves nothing.** It
      is index 1, held for "a board's well-known clock/time service cap"
      (`system/include/kickos/sys/cap_index.h:35`) -- the provision a future userspace CPU governor
      would name, `AUTH_PSTATE` being its matching authority bit. But default placement puts
      delegated cap `i` at child index `i + 1`, so the FIRST delegated cap lands on index 1:
      `user/include/kickos/sys/abi.h:248-249` states the placement ("under default placement they
      land at child indices 1..cap_count") and `abi.h:256-258` states the consequence outright. A
      parent avoids it only by naming a `cap_dest`, and nothing requires one, so the reserved slot
      is routinely overwritten. Closing the aliasing is a precondition for the provision meaning
      anything: either default placement starts at `KICKOS_CAP_FIRST_DYNAMIC`, or the index stops
      being reserved. Read directly; the placement claim checked in `abi.h` itself.
- [ ] **STALE, WRONG MECHANISM (not just a rotted line number): `mk64f`'s handle budget has no
      recorded derivation.** This described `boards/frdmk64f/configs/base/defconfig` and
      `boards/xmc4800-relax/configs/base/defconfig` each stating `KICKOS_MAX_HANDLES=12` directly.
      Neither defconfig sets any such symbol today, and `cmake/cap_table.cmake` now explicitly
      forbids a board from stating the table's width at all ("A board must NOT state the width...
      Kconfig declares no such symbol, so an attempt to set it is refused by name"): the width is
      summed at CONFIGURE time from the kernel's reserved-index count, the service list's retained
      caps (`RETAINED_CAPS`), and the app's declared peak (`CAPABILITIES`), each stated by whoever
      owns the fact. Whatever open question this pointed at -- an unjustified `mk64f` figure --
      needs to be re-derived against that configure-time sum; there is no board-stated constant
      left to cite a line number against.

## Found by the 10-angle review (2026-08-02)

Ten angles ran against the finished branch. What they found splits three ways: fixed in the same
pass, recorded here because it is a DESIGN change rather than a patch, and recorded here because it
is a claim I could not verify either way. Each item says which.

### Fixed in this pass

- [x] **No `static_assert` that some cap class equals `KICKOS_MAX_HANDLES`.** `kmain` asks the slab
      for a `KICKOS_MAX_HANDLES` run for root and `kpanic`s when none fits, so a mix whose widest
      class stopped short of the ceiling would not degrade -- the board would not boot. Four angles
      converged on this independently. `kcap_largest_class_slots()` plus the equality assert pinned
      it at the time, and the default spawn capacity was bounded against the largest class rather
      than against the ceiling macro. (Both went with the capability-class mix in M4.7.1: there is
      one uniform width per task now, so neither helper exists.)
- [x] **The multiclass switch could drift from the class table.** The selftest derives
      `cap_capacity`'s PARTIAL permission from the CMake variable, so a mix reaching the compiler
      by any other route than the one root-CMakeLists block would silently make that expectation
      wrong. It is now emitted as a compile definition in both branches and cross-checked in
      `cap.h` against `kcap_class_count() > 1`.
- [x] **LX6: a UART0 sub-source with no row in `UART0_LINES` LIVE-LOCKS the machine.** The demux
      posted no line for it, so the level-1 handler re-entered on the still-asserted CPU interrupt
      forever and never reached the kernel's spurious accounting -- which is only entered through
      `kickos_isr_irq`. The in-tree comment claimed the null-object handler would mask CPU int 13;
      it cannot, because nothing calls into the kernel. Reachable by an unprivileged driver holding
      the window grant simply enabling `RXFIFO_TOUT`, the standard RX idle source. The demux now
      silences unroutable sources (`INT_ENA` clear + `INT_CLR`) so refusing to route one costs that
      driver its interrupt, never the machine.
- [x] **`irq_discard` and `irq_ownership` both claimed line 11**, and `t_irq_ownership`
      deliberately POISONS its line (leaves it bound to a stale handle). It worked only because
      registration order happened to put `irq_discard` first. `DISCARD_LINE` moved to 15, and the
      false "unused by the other IRQ tests" comment is now a warning that the line is poisoned.
- [x] **Five false comments and a batch of stale figures.** `k64uartirq`'s "returns only on cap
      loss" (`irq_loop` ends in `kos_exit` and never returns), `c6uart`'s `configure` return read as
      an achieved baud when it is the request echoed back, `icu.h`'s `SPAN` arithmetic
      (`GENAL1 + 4` is `0x878`, the span is `0x880` rounded up), the spawn stack-cost comment
      ("four arrays, 14 bytes" -- it is six and 16 since `cap_dest`), and a comment saying the
      destination checks run against the DECLARED capacity when they run against the granted one.

### Recorded, not patched -- design changes

- [x] **FIXED. Console reclaim triggered on `recv_holders` reaching 0, but a driver's IRQ thread
      holds no endpoint cap.** The precondition is now the DEVICE's window holder set
      (`arch_console_reclaim_window` + `dev_window_free`), and `KOS_SYS_THREAD_KILL` is what makes the
      deferred reclaim terminate. Gated by `sim_driver_death` case 3, mutation-proved four ways.
      History and the design reasoning follow.
      **DESIGNED AND MEASURED 2026-08-02; parked on `wip/console-reclaim-window-precondition`
      (`edc5b15`), which does NOT link on `f302nucleo-st`.** Three findings settle the shape:
      (1) *"The driver is gone" is not expressible from kernel state, by design.* `domain_for`
      skips the dedup loop whenever an MMIO grant is present (`domain.cc:205`) and the dedup loop
      requires `region_count == 1` (`domain.cc:210`), so a driver's IRQ thread and service thread
      are in DIFFERENT `Domain` objects -- one grant, one domain, one thread (`domain.cc:200-204`).
      The service thread asks for no window precisely so its spawn is not refused `-KOS_EBUSY`
      (`k64uartirq.cc:494-497`). `domain_release` is a bare decrement with no refcount-zero hook.
      The isolation principle is what makes the driver invisible to the kernel; this defect is a
      consequence of it, not an oversight.
      (2) *The expressible precondition is about the DEVICE, not the driver*: no live domain holds
      the console's register window -- `dev_window_free()`, already the authority for
      one-holder-per-window. The ordering is free: `exit_current` runs `domain_release`
      (`sched.cc:202`) before `console_on_driver_death` (`sched.cc:221`) on every exit, so a sticky
      note plus this predicate reclaims on the last holder's own exit with no new hook.
      (3) *It must NOT land without the kill primitive, and that is not a preference.* With the IRQ
      thread still alive the reclaim is deferred, the console stays `USER_OWNED` with no receiver,
      and the bring-up failure reports -- the service-spawn-failure path at `k64uartirq.cc:546-547`,
      the ready timeout, and `console_handover_finish` -- send into a receiverless endpoint and go
      MUTE. Part (A) alone converts loud bring-up failures into silent ones, which is the exact
      property `console_handover_finish` exists to provide.
      Cost: +116 bytes on `f302nucleo-st`, which has 4. Blocked on the suite split above.
      No gate yet, and the existing `sim_driver_death` is VACUOUS for it: simcon is single-threaded
      with no window, so the new predicate never executes. The gate needs a two-thread sim console
      driver holding a fake DEV window, asserting the console does NOT come back while it is held.
      Original diagnosis follows. So a multi-threaded driver whose SERVICE thread dies while its IRQ thread is
      alive and still owns the device trips the reclaim: the kernel re-initialises a UART a live
      thread is driving. The comment at `kernel/syscall/cap.cc` argues the opposite -- that keying
      on `recv_holders` is what makes a multi-threaded driver reclaim only when its LAST receiver
      dies -- which is true only if every thread that touches the device holds a WAIT cap, and the
      IRQ thread deliberately does not. Reachable through the bring-up failure paths this branch
      changed. A correct fix needs the reclaim precondition to be "no thread of this driver is
      live", which needs a driver identity the kernel does not have, or the kill primitive the
      `CAP_IRQ` close arm is already waiting on. Not improvised at the end of a session.
- [x] **`cap_seat_stdout` has an UNCHECKED precondition** (`t` must hold a run) because the guard
      costs 12 bytes and `stm32f302` has none. Every caller satisfies it structurally today; the
      precondition is written into `cap.h` and anything that can create a thread without a run has
      to buy the space and add the guard back. See the FLASH-wall item below -- these are the same
      problem.
- [x] **The 64 KiB parts were at the FLASH wall; the suite is now SPLIT and they are not.**
      `f302nucleo-st` had 4 bytes free (65532/65536) and `bluepill-c8-st` 240 -- measure both, since
      the two parts are not equally tight. `selftest/CMakeLists.txt:66-69` had said since the third by-name
      exclusion that the fourth should be a split; that is what landed.
      Mechanism: every registration site is `TAP_ADD(name, fn)` and the macro is redefined once at
      the region boundary, elided sites expanding to `((void)sizeof(&fn))` -- an unevaluated operand
      that GCC counts as a use, so `-Werror=unused-function` stays live fleet-wide, and that emits
      nothing, so `-ffunction-sections` + `--gc-sections` drops the body. Confirmed with `nm`: part
      1 carries `t_mutex_deadlock` and not `t_irq_discard`, part 2 the inverse and it alone links
      the `kos_irq_discard` stub.
      Totality is a CONFIGURE-TIME assertion on every board, split or not: `_tap_arms_p1` and
      `_tap_arms_p2` are stated independently of `_tap_arms` and `p1 + p2 != total` is a
      `FATAL_ERROR`. Mutation-proved -- dropping one arm from region 2's count fails
      `cmake --preset sim`, a NON-split board, with exit 1. NOT caught, and harmless: an arm moved
      across the boundary WITH its clause keeps the sum equal.
      Result: `f302nucleo-st` 15020 bytes free on the binding image, and all three by-name
      exclusions retired on both chips (the tiny-extras exclusion macro deleted outright,
      `cap_capacity` narrowed to `microbit`). Arm count 71 -> 74 on both.
      **WITNESSED 2026-08-02 at `20f6d43`** (`m461m-f302-p1.log`, `m461m-f302-p2.log`): `1..44` and
      `1..30`, zero `not ok`, and all three restored arms ran on `f302nucleo` silicon for the first
      time -- `cap_dest` PASS, `cap_capacity` PARTIAL as predicted, `irq_discard` PASS. 44 + 30 = 74
      matches the configure prediction. `bluepill-c8` is not on this bench and remains unwitnessed.
- [x] **The sim's slab is 43% unreachable by default spawns.** Its mix is 10x6 + 8x10 with
      the default spawn capacity left at `KICKOS_MAX_HANDLES`, so every undeclared spawn asks
      for 10 and takes a class-1 run -- 8 of them, against 16 threads -- while the ten 6-slot runs
      are reachable only by an explicit declaration. Lowering the default spawn capacity to 6 is the
      obvious fix and it is WRONG here: `t_cap_capacity` compares a `capq_run(1)` against a
      `capq_run(0xFFFF)` clamped to root's capacity, so dropping the default to 6 makes both round
      to the same run and the arm reports PARTIAL. The sim is the fleet's ONLY witness that a
      declared-small child is a tighter ceiling. Fix the provisioning and the witness together, or
      not at all.

### The thread kill primitive (unblocks the reclaim fix and two dead close arms)

- [x] **LANDED as `KOS_SYS_THREAD_KILL = 45` (cooperative cancellation, gated on spawn parenthood). The facts below are what the design was built on and are kept as the record.** Its absence
      is already documented in two places: the deliberately-empty `CAP_IRQ` arm of
      `obj_close_protocol` (`kernel/syscall/cap.cc:304-315`) and the leak-never-strand branch of
      `irq_ref_drop` (`kernel/irq/irq.cc:290-296`), which says the arm should be added "the day a
      kill primitive lands". Facts gathered 2026-08-02, all verified against the tree:
      - **`irq_wait` has NO post-park error channel.** It parks in `sem_wait(&b->sem)`
        (`irq.cc:211`), and `sem_wait` returns void and never touches `wait_result`
        (`sync.cc:99-109`); the syscall arm returns 0 unconditionally (`syscall.cc:210-223`).
        Giving `irq_wait` an error return is the crux of any cancellation design.
      - **Userspace is already ready for it**: `irq_loop` breaks on `kos_irq_wait(...) != 0` and
        then calls `kos_exit(0)` (`uart_service.h:139-165`), so a cancelled IRQ thread exits
        cleanly with no driver change at all.
      - **Thread handles exist but nothing resolves one.** `kos_thread_create` returns
        `handle_for(i)` = `(gen << 8) | index` (`syscall_thread.cc:487`, `thread.h:365-368`), and
        `syscall_thread.cc:32-33` states that a resolver must reject `state == EXITED`. There is no
        thread capability type -- `CapType` is EMPTY/SEM/MUTEX/ENDPOINT/REPLY/IRQ and nothing else
        (`cap.h:90-101`). Every driver DISCARDS its spawn handles today; a kill-by-handle route means
        the bring-ups must start keeping them.
      - **Prefer cancellation to destroy.** Marking a target and waking it out of its wait with an
        error, letting it run its own `exit_current` -> `cap_teardown`, reuses all the existing
        teardown ordering and refcount discipline. A hard async destroy would have to unpick a
        thread that may hold a PI mutex, a domain reference and a cap run from another thread's
        context.
      - **Two blocking states resist cancellation** and should be refused rather than half-handled:
        `CALL_REPLY_WAIT` is queue-less with no back-pointer from caller to server
        (`syscall_ipc.cc:371`), so locating it needs a scan of every thread's `reply_waiters`; and
        `SLEEPING` uses the separate `tnext`/`on_timer` links whose `sleepq_remove` is file-static
        in an anonymous namespace (`time.cc:22-53`) with no cancel entry point exported.
      - **There is no "destroy wakes waiters" precedent to copy.** `kos_sem_destroy` is an ALIAS of
        `kos_handle_close` (`sys.h:120`), the `CAP_SEM` close protocol is empty (`cap.cc:228-231`),
        and `sem_ref_drop` LEAKS rather than waking at refs->0 with a waiter (`cap.cc:82-87`). The
        only third-party early return in the tree is the endpoint `send_waiters` EPIPE
        (`cap.cc:275-300`); `recv_waiters` are never woken by anything.

### Left owing by the console-reclaim / cancellation change (2026-08-02)

**SILICON, 2026-08-02 at `20f6d43` (`m461m-*`): the change is witnessed and the list below shrank.**
All five `*_uartirq` service lists ran the whole suite through the userspace driver with their
first-light markers on the wire, so the bring-up paths this change touched are exercised on real
hardware: `xmc4800-relax` `1..78`, `frdmk64f` `1..78`, `esp32c6-wroom` `1..78`, `esp32-wroom`
`1..74`, `rx72m` `1..74`, zero `not ok` anywhere. What is still NOT witnessed is the DEFECT PATH
itself -- no bench run kills a live console driver's service thread, so the deferred reclaim and the
cancellation are proven only by `sim_driver_death` case 3 and its four mutation proofs.

- [ ] **`kos_thread_kill` has NO fleet coverage.** Every refusal assertion -- bad handle, big
      handle, a stranger's thread, spawner-accepts, exited-slot EBADF -- lives in the sim
      `drvdeath` app, so no non-sim board exercises the syscall at all. Adding a `thread_kill`
      selftest arm means raising the whole-suite floor `_tap_arms` AND the clause for the region the
      arm lands in, `_tap_arms_p2` for region 2 or `_tap_arms_p1` for region 1
      (`user/apps/common/selftest/CMakeLists.txt`). Read the current values out of that file, never
      out of this entry. The three are deliberately independent expressions, and the split's totality
      FATAL fails the configure on EVERY board the moment p1 + p2 stops equalling the whole, so a
      half-done edit cannot pass quietly -- but the conditional `math(EXPR ...)` clauses beneath each
      one are per-posture, so an arm gated on `KICKOS_HAVE_MPU` moves a clause and not the base.
- [ ] **The stale-spawner-tag clear in `ThreadPool::alloc` is argued, not gated.** It is what
      stops a reclaimed slot's new occupant inheriting the right to kill the previous occupant's
      orphans. Witnessing it needs a thread-slot reuse plus a surviving orphan whose spawner held
      that slot, and no test stages that. It is the ONE part of the kill gate with no mutation
      proof -- everything else in it is proved RED-then-GREEN.
- [ ] **microbit's user arena is 0-7 bytes from the cliff where `mem_self_grant` stops running.**
      MEASURED at `736f4c1`: adding a bare 8-byte kernel `.bss` array flips that arm from RUN to
      SKIP and reds `microbit_selftest`. This is not about any one change -- it means the NEXT
      kernel `.bss` byte anyone adds is a coin flip on that board, and it already forced the
      console-window design away from two stored words and away from a `Thread* spawner` field
      (40 bytes). Give the arena headroom, or make the arm state its requirement, rather than
      leaving the next byte to discover it.
- [x] **`ThreadSet::cancel_all` cannot kill a driver thread wedged BEFORE its first `kos_irq_wait`.**
      `cancel_all()` (`user/include/kickos/sys/driver_service.h`) calls `kos_thread_kill` on every
      spawned peer, but the kill is COOPERATIVE and honoured only inside `kos_irq_wait`, so a peer
      that has not yet reached its first wait rides out the unwind alive. The loudness regression
      this entry used to report is fixed: `unwind()` now closes `ep` BEFORE cancelling peers and
      before printing, so the endpoint's last receiver holder goes to 0, the console is noted dead
      and reclaimed, and the tag reaches the wire. On `k64uartirq` and `xmcuartirq`
      (`system/driver/mk64f/k64uartirq/k64uartirq.cc`,
      `system/driver/xmc4800/xmcuartirq/xmcuartirq.cc`, both `thread_count = 2, barrier_after = 1`)
      leg L8 forces the {EP, WAIT} holder's index >= `barrier_after`, so at a ready-timeout no
      receiver has been spawned yet and this specific case never exercises the gap -- but the
      underlying `cancel_all` hazard is general and still open for any peer wedged before its own
      first wait.
- [ ] **The sim's `arch_console_reclaim_window` returns the fake PV register block unconditionally.** The
      sim console's wire is host fd 1 and its reclaim is a no-op, so this is a modelling statement
      -- "if a sim console driver held registers, these are the registers" -- not a hardware fact.
      True in the only sense the sim can make it true (that block is the only DEV window the host
      admits), and it is what lets case 3 exist at all, but it is the softest part of the gate.
- [ ] **`SIMCON_WIN_BASES` is a THIRD copy of `SIM_PVREG_BASES`** (`sim.cc`, the selftest, and now
      `service_list_sim.cc`), each carrying a "must equal" comment and no check. Following the
      existing precedent rather than fixing it was the right call mid-change; fixing it is owed.

### Found by the adversarial review of the cleanup plan (2026-08-03)

The plan was reviewed before execution; it killed three items and found a live bug the plan
never touched. What was FIXED is in the commit. What was found and NOT fixed:

- [x] **`rpusb.cc` breaks the bring-up ordering its five siblings follow.** SUPERSEDED by M4.8.1:
      the descriptor's `barrier_after = 1` against `thread_count = 2` puts the readiness poll
      BETWEEN the two spawns and leg L8 makes any other placement unrepresentable under HANDOVER,
      and the generic `unwind` closes the endpoint before it prints. Record:
      `docs/design-generic-driver-service.md`.
- [ ] **The console register window is stated 7+ times per chip with no cross-check**, and it is
      not an xmc-only shape: `mk64f` is identical and `chip_mk64f.cc` already documents the
      unenforced invariant ("the two must not drift"). Single-sourcing it through a header WAS impossible
      when this was written, because a `system/init/` service list got exactly one include dir
      (`system/include`) and the driver `REGDIR` is PRIVATE, so a chip's base-address header was
      unreachable. M4.7.5 removed that premise: the headers are public as
      `<kickos/chip_mmap.h>`, the chip include dir is on the path, and the rows name the constant.
      The DEDUPLICATION is therefore done. The idea below stands on its own and is not replaced by
      it, because it answers a different question, drift versus authority: `cap_console_publish` has no owner check at all today, and
      requiring the publisher to hold exactly `arch_console_reclaim_window()` closes the drift on
      every board with machinery that already exists (`caller_holds_mmio_block`). That is a
      feature, not cleanup.
- [ ] **Grant span and reclaim span are not required to be equal**, and a future chip may need
      them different. `console.cc` tests RANGE OVERLAP, not equality, so welding both to one
      constant would assert an invariant the code does not have.
- [ ] **`arch/arm/chip/xmc4800/usic.h` and `regs/usic.h` are two near-duplicate copies of the
      USIC channel-offset table in two namespaces**, and `usic.h` carries its own independent
      `0x40030000` literal while `regs/usic.h` derives from `mmap.h`. `usic_uart.cc` consumes the
      un-derived one. Larger duplication than the console window.
- [x] **The cap working-set derivation is copy-pasted in six board headers** (four at 7, two at
      12) plus the fleet version in `config/system.h`. Only the two 12s were rewritten here.
      Moving the arithmetic to the app was rejected: the terms belong to three different layers
      (kernel reserved plane, suite peak, service-list retained cap), so relocating gives three
      homes for one sum with nothing tying them.
- [x] **A compile-time check of the app's cap demand is not expressible.** microbit runs at
      `MAX_HANDLES 7` only because `mutex_deadlock` SKIPs at RUNTIME, which no `static_assert`
      can see; and the demand varies by posture and by split part. If it is ever wanted, it
      belongs in CMake via the existing `cmake/boot_arena.cmake` preprocessor probe, which
      already reads `KICKOS_*` macros out of headers, keyed per posture.
- [x] **CLOSED BY DELETING THE MECHANISM: `bluepill-c8` was the only board that shadowed a chip
      `board_config.h`**, and the lookup was either/or, so any knob the chip right-sized and the
      board omitted silently reverted to the fleet default -- that is how `KICKOS_MAX_SEMAPHORES`
      regressed, and nothing gated the shadowing. Both the per-board and per-chip `board_config.h`
      and the either/or lookup between them are gone: a knob now has exactly one place to be set,
      its Kconfig declaration's own `default N if CHIP_X` list (e.g. `KICKOS_USER_HEAP_SIZE` in the
      root `Kconfig`), which a board's defconfig may explicitly override but cannot silently omit
      into. There is no longer a second header to shadow, so a second board cannot repeat this.
- [ ] **Two comments describe a defect two incompatible ways, so one of them is wrong about the
      code**: `tele_pingpong/main.cc` says a non-last thread exit is currently broken on ARM
      while `sched_exit/main.cc` documents it fixed and `check_sched_exit.sh` gates it. (The
      `rxsci.cc` `RXSCI_LED_TRACE` half of this item is RESOLVED: the macro is deleted. Its only
      writer was the service thread's short-accept branch, which `uart::console_thread` now owns,
      so keeping it would have left a witness nothing sets.)
- [ ] **`usbcdcwit`'s `STALL_MAX = 2000` is 5x off its comment** (each zero-accept sleeps 0.2 ms,
      so the bound is ~400 ms, not ~2000 frames). The comment was corrected; if 2000 frames was
      the intent, the CONSTANT is what needs changing.
- [ ] **About 46 ` -- ` occurrences remain, nearly all inside string literals** (re-counted
      2026-08-16; the 72 this box carried had drifted): linker `ASSERT()`
      diagnostics, and the rest `kprintf`/`kos::print`/TAP text. They are user-visible output,
      not comments, and some are grepped by gates (`service_list_sim.cc`'s banner), so they were
      left alone. A separate output-text pass could take them.

### Recorded -- reported by an angle, NOT verified by me

- [ ] **`g_stdout_target` is not cleared when the console driver dies**, so children spawned after
      the death are seated on a dead endpoint. Traced: NOT a defect. `kos_send` to an endpoint with
      `recv_holders == 0` returns `-KOS_EPIPE` immediately (`syscall_ipc.cc:115`) and `_write`
      falls back to `kconsole_write` on any non-positive return (`newlib_stubs.cc:45`), which after
      the reclaim drives the polled UART. Cost is one wasted syscall per write. What IS real and
      unfixed: the kernel's identity reference on that endpoint is never dropped, so the endpoint
      object is pinned for the life of the image.
- [ ] **RX72M is said to have lost plain-vector dispatch**, and **`kos_irq_discard` is said to be a
      no-op on three backends**, and **RXv3 is said to write `IR` on a LEVEL line**. Each is
      plausible and each is silicon-gated; none was reproduced this pass. Check them on the bench
      before acting.

## Found re-deriving the gates (2026-08-02)

- [x] **`sim_published_console` compared a posture-1 TAP stream against a posture-0 expectation, and
      failed naming a regression that did not exist. FIXED.** The gate's expected arm count is
      computed by the CALLING tree's CMake and depends on `KICKOS_HAVE_MPU`, but
      `tests/integration/check_sim_published.sh` re-configures its own build tree with a bare `--preset sim`,
      and `CMakeLists.txt` defaults that knob to 1 for the sim arch. So `cmake --preset sim
      -DKICKOS_HAVE_MPU=0` produced a red gate reading `TAP plan is 75, expected exactly 71` on a
      perfectly healthy tree. The posture is now forwarded as a required fourth argument, and the
      CMake comment that claimed "the same board posture" is true rather than aspirational.
      Verified both ways: red before, green at both postures after. **The class matters more than
      the instance** -- this is a gate that fails on a correct tree, which is the mirror of the
      M4.5.8 audit's finding, and any other gate that re-configures a sub-build has the same
      exposure.
- [x] **A source-comment citation to a doc that was about to be deleted.**
      `kernel/include/kickos/instance.h` cited a section of the cap-table spike by file path.
      `doc_names` reads TRACKED MARKDOWN ONLY, so it would never have caught it. Removed by hand,
      and the reasoning it pointed at is stated inline instead. The general hazard is already
      recorded in `STATE.md`; this is the second instance.
      **The markdown half of the same hazard, the gate DID catch**, and only after the squash: this
      very entry originally named the deleted file, passed `doc_names` while the file still existed,
      and went red the moment it did not. That is the gate working, and it is also why the squash
      and the doc rewrite could not safely be done in either order without re-running it.

## M4.6.2 USB CDC-ACM: WITNESSED on pizero2350 silicon (2026-08-01)

The driver landed at `9832416` unrun. It now works end to end on an RP2350. Captures
`.session/logs/m462-*`.

- [x] **The host ENUMERATES it.** `picotool load -x` an app built against
      `kickos_services_pizero2350_usbcdc`, and Linux bound its `cdc_acm` driver:
      `usb-KickOS_KickOS_console_0001-if00`. `lsusb -v -d 1209:0001` parses 2 interfaces --
      IF0 Communications / Abstract (modem) with interrupt IN `0x81` (16 B, bInterval 16),
      IF1 CDC Data with bulk OUT `0x02` and bulk IN `0x82` (32 B each), one config, 100 mA.
      **This empirically validates the descriptor tables, the chapter 9 request machine and the
      multi-packet EP0 data stage** -- the exact part flagged as highest-risk because there is no
      USB 2.0 or CDC/PSTN specification in the local reference set, so two review passes could
      only check internal consistency. A real host stack is now the check.
- [x] **Bulk IN carries data, byte-exactly.** `tx=918 drop=0 used=0`, 918 bytes received on the
      CDC tty, `buff=0x33` -- bit 4 is the bulk IN completion, so the controller demonstrably
      transmitted. The ring drains fully; nothing is stranded.
- [x] **Both earlier zero-byte results were MEASUREMENT ERRORS, not driver faults**, and both are
      worth recording because each looks exactly like a broken driver:
      1. **The first test app could not possibly have worked.** `hello` emits everything through
         `kos::print`, which `user/include/kickos/sys.h:21` defines as "straight at the kernel
         console ... **NOT stdout**", and `console_emit`'s `USER_OWNED` case is a bare
         `return; // DROP`. The ring was never written. This is the `kos_print`-does-not-survive-a-
         published-console blocker in STATE.md, and the project's own M4.3 notes already say
         `hello` is not a stdout demo vehicle. The stdio path is `_write -> kos_send(0)`;
         `gpioblink` is the pure-stdio app (6 `printf`, 0 `kos_print`).
      2. **A CDC device sends nothing until the host opens the port.** With the ring full and
         `queued=1`, `bufin=0xc41c` held AVAILABLE with LENGTH=28 and `buff=0x3` showed EP0
         completions only -- the host issues no IN tokens while no reader holds the tty. Opening
         it started the flow immediately.
- [ ] **Still owed**: the production (non-diag) service list on a pure-stdio app, bulk OUT (host
      to device) which nothing has exercised, and `teensy41`, which is a marked seam and not a
      half-built backend. Note `Shared::configured` does not clear on unplug -- no backend arms a
      disconnect or suspend source -- so a host that vanishes without a later bus reset leaves it
      reading 1. Wiring that needs matching resume handling and is bench-gated, not blind.
- [x] **The production list has been TRIED, three times, and delivered nothing.** Captures
      `.session/logs/m462-cdc-witness{,2,3}-{uart,acm}.log` (2026-08-01, 18:56 to 22:27, at
      `b129a65`-dirty). The UART side shows the device getting all the way to
      `[rpusb] host configured the device (t=118ms spins=127764)`, so enumeration and configuration
      are fine on this list too -- and then **every ACM capture is 0 bytes**. This is what the
      "bulk IN delivers nothing" reading refers to; it is NOT contradicted by the 918-byte diag
      capture above, because that one ran a different service list. **Both explanations already on
      record must be re-excluded before anything else is suspected**: the app must be pure stdio
      (`kos::print` is dropped under `USER_OWNED`), and the host must have the tty OPEN or no IN
      tokens are issued. A third candidate is specific to this list: if the witness app uses
      `kos_call` and root is refused it, the app never reaches its output at all -- check the app's
      declared authority before reading the silence as a driver fault. Driver codegen is
      bit-for-bit identical across MPU postures, so it is not posture-sensitive.

## The two sharpest IRQ-driver silicon questions, answered from the manuals (2026-08-01)

Both were flagged as the highest-risk unknowns behind the five unrun `*_uartirq` drivers. Verified
against the TRMs in the local reference set, not against HAL headers or the web.

- [x] **RX72M `GENBL0.ENj = 0` DOES clear both `ISj` and `IR110`. The tier-1 group mask is CORRECT.**
      The manual states it three times, decisively at **sec.15.5.4 p.542**: "when the
      GENBL0/.../GENAL1.ENj bit is set to 0, the corresponding GRPBL0/.../ISj flag **and IRn.IR
      flag** become 0" (also sec.15.2.23(3) p.502, sec.15.2.24(3) p.504, sec.15.2.1 p.480). No latch
      and no explicit clear: the BL/AL groups have **no clear register at all** (`GCRIE0`/`GCRBE0`
      exist only for the edge groups, sec.15.2.25 p.505) and `GRPxxx` is read-only, so
      `arch_irq_clear_pending`'s no-op for group lines is right. `IRn.IR` must not be written for a
      level source ("neither 0 nor 1 can be written", sec.15.2.1 p.479 note 1) and the tree writes
      it nowhere. Arm/disarm ordering matches sec.15.7.1/15.7.2 p.545 exactly. **Nuance that was
      missing from the in-tree comment and is now added**: `IRn` is the OR over the group
      (Fig.15.17 p.542), so masking one source clears `IR110` only if no sibling is still asserting.
- [ ] **The RX group-mask path has NO in-tree consumer and has never been exercised, even in QEMU.**
      `rxsci` deliberately does not use `TEI6`/`ERI6` (`rxsci.h:19-25`), which are the only GROUPBL0
      sources in play. So the code just verified as correct-per-manual is also completely unwitnessed.
      Worth a forcing consumer before it is relied on.
- [x] **ESP32-C6: the `0x2000_1000` CPU-interrupt enable is UNDOCUMENTED but almost certainly REAL,
      and "fixing" it to INTPRI would be a REGRESSION.** `0x2000_1000` appears nowhere in TRM v1.2,
      and "PLIC" appears zero times in the TRM, the C6 datasheet, the WROOM-1 datasheet or the
      dev-kit schematic; the CPU sub-system region `0x2000_0000..0x2FFF_FFFF` (Table 1.4-1)
      documents only `CORE_XT_EN` @`0x2000_0900` and the CLINT. The documented mechanism is INTPRI
      @`0x600C_5000` (Table 5.3-2; sec.1.6.2) plus `mie`. **But the tree never writes INTPRI's
      ENABLE at all**, so it sits at its documented reset value of 0 -- and CPU int 31 (the inject
      doorbell) and CPU int 30 (UART0) both deliver on silicon. Under sec.1.6.2 that is impossible
      unless `0x2000_1000+0x00` is a live view of the enable. The in-tree offsets are NOT INTPRI's,
      so it is not a plain alias, and the difference is self-validating: `inject_doorbell_init`
      writes `pri(31)` at `+0x8C` and `THRESH=0` at `+0x90`, which under INTPRI's map would instead
      set `THRESH=7` with `PRI_31=0` -- masked, never fires. The doorbell fires.
      **Therefore `arch_rv_hw_mask` is fine**: the unmask being silicon-proven proves that ENABLE bit
      is the operative gate, so clearing it is a real mask and not merely self-consistent.
      **This also retires an old wrong conclusion**: "INTPRI is vestigial" came from a bring-up
      session that applied THESE offsets to the INTPRI base, which masks the line -- a complete
      alternative explanation of the failure that was read as evidence. It was never established.
      **CLOSED ON SILICON 2026-08-01: `VERDICT DISTINCT`.** The `c6intpri` probe
      (`user/apps/esp32c6-wroom/c6intpri`, capture `.session/logs/m461-c6-intpri.log`) read INTPRI
      directly from an app holding a 256 B PMP NAPOT window at `0x600C_5000`, and **every INTPRI
      register reads `0x00000000`** -- ENABLE, TYPE, EIP_STATUS, PRI_29..31, THRESH, FROM_CPU_0/1,
      the documented reset state -- while the inject doorbell **demonstrably delivers**
      (`spurious 0 -> 1`). Neither of the two independent boot-time writers of `0x2000_1000`
      (`inject_doorbell_init` bit 31, `console_buffer_init` bit 30) shows up in INTPRI's ENABLE.
      `0x8C` reads 0 rather than 7, which also kills the "same block, offsets shifted by 4"
      hypothesis. So **`0x2000_1000` is a separate, live, undocumented CPU-interrupt controller**,
      the in-tree code is correct, and moving it to INTPRI would have been a regression -- now
      proven rather than argued. `0x2000_1000` itself is Rule 7 reserved and root is unconditionally
      unprivileged, so no app can read it back; the probe prints that fact rather than implying a
      reading it cannot take.
      The probe design was corrected once before it was trusted: the original in-flight delta could
      never have fired, because `console_buffer_init` already unmasks the console line at boot, so
      the later `kos_irq_unmask` was an idempotent re-OR of a set bit and the delta was structurally
      0 whatever the answer. That same fact supplied the SECOND independent boot-time witness.
- [x] **The LX6's TX-empty latch defect does NOT exist on the C6.** `UART_INT_RAW` there is
      `R/WTC/SS`: hardware self-sets and it clears only on a write-1 to the matching `UART_INT_CLR`
      bit (Register 27.3 + the Access Types glossary), so it does not follow the level condition back
      down. `c6_tx_push`'s per-push `INT_CLR` is correct. `UART_CONF1_REG` has no `_SYNC` suffix, so
      the threshold write needs no `UART_REG_UPDATE` (sec.27.5.1) -- the quiesce path is correct.
- [x] **Citation and comment corrections applied** (values untouched, uncommitted): esp32c6
      `mmap.h` (UART ch.26->27, RMT ch.30->37, the false "PLIC + CLINT share one page at
      0x2000_1000 (TRM ch.1.7)" replaced with what the TRM actually places there),
      `regs/plic.h` (false "TRM sec.1.6" cite and the "vestigial INTPRI" claim removed, INTPRI's
      real offsets and the off-by-one-register trap recorded, `prio > thresh` -> `>=`),
      `regs/intpri.h`, `regs/intmtx.h` (a "Table 10.4.2" that is not a table -> sec.10.4.1),
      `chip_esp32c6.cc` (dropped an esp-idf quote presented as authority), and rx72m `regs/icu.h`.

## f302nucleo: the defect is MISFRAMED -- the fault reporter is innocent (CLOSED 2026-08-13)

**EVERY BOX BELOW IS CLOSED BY THE SAME ROOT CAUSE, and they stayed unticked under a header that
says CLOSED.** `st-flash --connect-under-reset --reset write` left the core under halting debug with
`DEMCR.VC_HARDERR` armed, so the first HardFault halted at the handler's first instruction instead
of running it. Both surviving candidates below -- a bad vector fetch and LOCKUP during stacking --
are FALSIFIED by instrument (`HFSR` `FORCED` with `VECTTBL` clear, `DHCSR` `S_LOCKUP` clear,
`CFSR=0x10000` with no `STKERR`), and the single-change proof is a gdb write of `DEMCR=0x01000000`
on the same boot with no reset and no reflash. Kept for the instrument lessons: the UART markers
were confounded because every marker that fires runs with `CR1.TXEIE` clear, and a debugger left
armed is a legitimate suspect before the silicon is.

Second bench pass of 2026-08-01, at `ab2a52d`(-dirty), by direct-to-`USART2->TDR` marker
instrumentation on the panic path, uncommitted scratch behind a default-off build option.
Captures: `.session/logs/m461-f302-markers{,2,4}.log`, `m461-f302-reset-series.log`.

**The instrumentation was proved BOTH ways before any negative reading was trusted**, because a
silent marker and a broken emitter look identical on the wire. Control `'0'` is emitted from
privileged thread mode in `kmain` right after `kdiag_led_init()`; control `'1'` from UNPRIVILEGED
thread mode on PSP in the app. Both fire. The unprivileged store is sound by ARMv7-M's default
memory map -- the Peripheral region `0x40000000..0x5FFFFFFF` carries no privilege attribute, and the
PPB at `0xE0000000` is the only architecturally privileged-only window, which is the very asymmetry
`ringpriv/ppb.cc` is built on.

**Result on a CLEAN NRST boot, byte-identical across 5 consecutive resets (349 B each):**

```
0 <complete banner> 1 [faul        then nothing, held for 90 s
```

- [x] **The markers stop at the first TX interrupt, and the split CANNOT be read as "`kos_print`
      never returns".** `'1'` fires, `'2'` (immediately after `kos_print` returns) does not,
      `'3'` (immediately before `__asm volatile("udf #0")`) does not, `'@'` (first instruction of the
      HardFault shim) does not.
      **The split is CONFOUNDED**: every marker that fires runs while
      `CR1.TXEIE` is 0, and every marker that does not runs while TXEIE is 1 with bytes queued --
      `'0'` precedes `console_buffer_init()` entirely, and `'1'` runs before the first
      `console_tx_write` arms the line. So the boundary is "before/after the TX interrupt is first
      armed", NOT "before/after `kos_print` returns", and both controls landed on the safe side of
      it. The markers cannot see past that line because they are themselves raw `TDR` stores.
      Reading the code agrees nothing in that chain can block. The fast path in
      `console_tx_write` (`kernel/init/console_tx.cc:139-170`) is taken because 64 <= 511 free, and
      it copies, sets `head`, arms TXEIE, primes one byte and returns **with no polling at all**; the
      bounded `wait_slot`/`drain_sync` overflow path is never entered. `user_range_ok` is <= 8
      iterations and the CRLF cook is 63.
      What the evidence actually supports is narrower: **no instruction whose observable effect is a
      raw `TDR` store survives past the first USART2 TX interrupt.** Whether the CPU stopped, or
      merely the bytes stopped reaching the wire, is exactly what is not yet distinguished.
- [x] **`hello` is NOT immune, so `fault` is not special -- the discriminator is the BOOT PATH.**
      This was the lever the whole "why is `fault` different" line rested on, and testing it removed
      it. On a clean NRST boot (`st-flash reset`), `hello` loses BOTH of its root-thread `kos_print`
      lines and its banner truncates mid-line, then ping/pong flows perfectly and indefinitely
      (`.session/logs/m461-f302-hello-rootlines.log`). The SAME binary booted by
      `st-flash --connect-under-reset write`, reader armed first, prints the COMPLETE banner, BOTH
      root lines, then `ping 1` (`m461-f302-hello-rootlines4.log`, lines 42-43). Two different apps
      now show the same clean-NRST-versus-debugger-resumed split.
      **Neither simple story covers both observations.** "Early bytes are lost, then it settles" does
      not explain the marker image's banner arriving COMPLETE on a clean NRST before dying at
      `[faul`; "the CPU freezes" does not explain `hello` recovering and running forever.
- [x] **Live hypothesis that would make the firmware INNOCENT: the ST-Link VCOM itself.** The console
      on this board is the ST-Link V2.1's own VCOM, on the SAME probe that asserts NRST for
      `st-flash reset`. If that bridge drops or fails to forward bytes around a reset, early output
      is lost ON THE WIRE and there was never a firmware defect. Being argued against the two
      observations above rather than assumed.
- [x] **ANSWERED on the bench 2026-08-01 by the LED probe: the `udf` EXECUTES and the HardFault
      handler is NEVER ENTERED.** LD2 (PB13) driven by a raw `GPIOB->BSRR` store -- no UART, no
      polling, no syscall -- with cycle-counted dwells so the states are humanly separable, plus a
      self-test flash proving the store itself works. Observed by the maintainer:
      **one short flash, then solid ~2 s, then dark forever.**
      That reads, unambiguously: the app ran, `kos_print` RETURNED (the 2 s dwell is after it), the
      `udf` was reached (the 1 s dark dwell is before it) -- and then the fault-reporter entry, which
      lights LD2 as its FIRST action before any UART store, never ran. So exception entry itself
      fails: a bad vector fetch, or LOCKUP during hardware stacking. **Every UART marker past TXEIE
      was a false negative**, exactly as the confounded-split analysis predicted.
      A design flaw in the first version of this probe was caught before it was flashed: `'2'` and
      `'3'` sat SIX instructions apart, so the LED pulse would have been ~100 ns and invisible, and
      "reached the `udf`" would have presented to the eye as "never lit" -- collapsing the two states
      the probe exists to separate. The dwells are what make the reading real.
- [x] **The `[faul` truncation is EXPLAINED, and it is not a defect.** The same LED-probe image put
      the COMPLETE `[fault] executing an illegal instruction (expect a fault dump)` line on the wire
      (617 B) where every earlier build stopped at 2-5 characters. The only difference is the 2 s
      dwell inserted after `kos_print`: given time, the ring and the TX ISR drain the whole line
      perfectly. The truncation was only ever the `udf` landing before the drain finished -- and the
      panic path then failing to flush the remainder, which is the ORIGINAL filed symptom, now
      correctly attributed to a handler that is never entered.
- [x] **What is left, and it is now a narrow question**: why does exception entry fail on this chip?
      Candidates are the vector fetch (VTOR is never written; reset default 0 aliases flash, and
      external IRQs from the SAME table demonstrably work, so the table is readable) and LOCKUP
      during hardware stacking to PSP. `fault` runs unprivileged on PSP like every board. The
      escalation path is `udf` -> UsageFault, unenabled in SHCSR, so HardFault with `HFSR.FORCED`.
- [ ] **Separately, a rig artifact is now distinguished from the defect.** `hello`'s clean-NRST
      truncation is a FRONT-END LOSS WINDOW at the ST-Link VCOM around NRST, not firmware: nothing in
      this firmware drops the first N bytes and then works flawlessly forever, and
      `CONTEXT.local.md` already records that on this board you must arm the reader and reset as a
      separate step "or the banner is lost". It does NOT explain the marker image's byte-identical
      349 B stop across 5 resets -- host USB timing does not land on the same byte five times. **Two
      phenomena, previously conflated.** The clean separator, if it is ever worth the bench time,
      is to capture on an FTDI wired to PA2 instead of the probe's own VCOM.
- [x] **The panic path is HEALTHY, and this is now positively witnessed.** One boot behaved
      differently: the one left running by `st-flash --connect-under-reset write` (core resumed by
      the flash tool, debugger attached) rather than by a clean `st-flash reset`. There the line
      completed and the markers `d D F G P` all fired -- `drain_sync` returned, the flush returned,
      `kpanic_enter` reached its end AND returned into the reporter, and **`P` means the CFSR/HFSR
      PPB reads succeeded**. So flush, reclaim and the fault-register decode are all exonerated on
      silicon. Different ENTRY STATE, not flakiness: the clean-reset behaviour is 5/5 deterministic.
- [x] **The recorded `CFSR=0x00008200` / `BFAR=0xE000ED00` does NOT belong to this app.** `udf #0`
      raises UsageFault `UNDEFINSTR` = CFSR bit 16 (`0x00010000`), which is CLEAR in that reading,
      while `BFAR=0xE000ED00` is `SCB_CPUID` -- an address that appears nowhere in the tree except
      `user/apps/common/ringpriv/ppb.cc:35`, a different executable. **The stale-sticky-bits escape
      does not save it**: CFSR and HFSR reset to 0, and the `fault` image demonstrably booted (full
      banner), which requires a reset. So that register set was read against a resident `ringpriv`
      image, or transcribed from a ringpriv session. It has been quoted as this defect's primary
      hardware evidence since it was filed. **It is not evidence for this defect.** Marker `P`
      supersedes it: the PPB reads succeed.
- [x] **`kfault_terminate` is not reached, and the LD2-dark reading was CORRECT** -- but for a
      reason two steps upstream of where it was being applied. It was read as "the fault path dies
      inside the dump span"; it actually means the fault never happened.
- [x] **Next probe is NOT another marker sweep.** The question is now narrow: where does the first
      buffered write block, and why does `hello` survive it. Delegated; a one-line discriminating
      probe is preferred over another broad pass.

**Do not re-derive the old hypothesis list.** Console-broken, ring-flush, reclaim-hang, vector
routing, stack exhaustion and `KICKOS_POLL_SPIN_MAX` are all now moot for this item: they describe a
panic path that a clean boot never enters. `arch_console_reclaim` on this chip is the empty default
(`arch/common/arch_console_reclaim_default.cc`; only `mk64f`, `xmc4800` and `esp32` define a body),
so it could never have hung -- that candidate died on inspection, before the bench.

## M4.6.1 selftest bench pass -- `irq_claim_gate` and `irq_reclaim` on silicon (2026-08-01)

The first silicon witness of the two IRQ-capability cases M4.6.1 added. Default posture on every
board (**no** `KICKOS_SERVICE_LIST` override -- the existing console, not the unrun `*_uartirq`
drivers), `KICKOS_ENABLE_SELFTEST=ON`, `MinSizeRel`. Captures in `.session/logs/m461-*-selftest*.log`.

| board | arch / enforcement | plan | result | skips |
| --- | --- | --- | --- | --- |
| `xmc4800-relax` | armv7m / PMSAv7 `enforce` | `1..71` | all pass | 1 (`mutex_deadlock`) |
| `rx72m` | RXv3 / RX-MPU `enforce` | `1..71` | all pass | 0 |
| `esp32c6-wroom` | rv32imac / PMP `enforce` | `1..71` | all pass | 0 |
| `esp32-wroom` | Xtensa LX6 / `mpu off` | `1..67` | all pass | 0 |
| `frdmk64f` | armv7m / SYSMPU `enforce` | `1..71` | all pass | 1 (`mutex_deadlock`) |

**ALL FIVE BENCH BOARDS PASS.** `irq_claim_gate` and `irq_reclaim` are witnessed on **four ISAs and
four distinct enforcement backends** (PMSAv7, SYSMPU, RX-MPU, PMP) plus one no-MPU part, so the
capability-shaped IRQ line is no longer emulator-only. The `1..71` / `1..67` split is exactly the
enforcement-versus-not plan count STATE.md predicts, machine-confirming the per-posture arm floor.

- [x] **`frdmk64f` needed a physical replug first: the OpenSDA J-Link wedged.** `VTref=3.300V` (board
      powered, probe alive), device selected, then the SWD connect hung immediately after
      `InitTarget() end`, twice, with no timeout of its own -- the documented wedge, whose only fixes
      are PHYSICAL. The maintainer replugged and it flashed and ran first try, SN unchanged
      (`000621000000`), re-enumerating on `ttyACM0`. **The destructive `unlock Kinetis` mass-erase
      was NOT used and was not warranted**: the part was not locked, it was wedged, and that path
      wipes flash.
- [x] **FIXED and re-witnessed: the `mutex_deadlock` skip is ROOT'S CAP TABLE, not any pool.**
      `xmc4800-relax` and `frdmk64f` are the only two boards in the fleet with a NON-EMPTY default
      service list, and its SPI service (`xmcssc`, `k64dspi`) keeps its request endpoint cap in
      root's table for the life of the image. That one retained cap is the entire delta: dynamic
      slots are `MAX_HANDLES - KICKOS_CAP_FIRST_DYNAMIC`, root holds SPI-ep + `g_lock` + `g_done`,
      leaving 5 where `t_mutex_deadlock` needs 6. (`KICKOS_CAP_FIRST_DYNAMIC` has since fallen to 2,
      handing every table two more dynamic slots, so this particular shortfall is no longer
      reachable on a 12-handle board; the diagnosis stands as the worked example.) The three boards that pass all run
      `kickos_services_none`, and the logs say so directly -- the skippers print
      `stdout endpoint -> console driver (service list published)`, the passers print
      `kernel debug console`. Nothing per-board differs in the constants; all five ship
      `MAX_HANDLES=12`.
      **The message was lying**, exactly as in the M4.5.6 precedent but in mirror image: there
      "pool" meant "arena", here it means "cap table". `-KOS_ENOMEM` is returned both by an empty
      pool and by `cap_install` finding no free slot (`kernel/syscall/cap.cc:580-597`), so the test
      cannot tell them apart at the call site.
      **Fixed at `MAX_HANDLES=14` on those two chips only** (chip-scoped
      `arch/arm/chip/{xmc4800,mk64f}/include/kickos/board_config.h`, so no other board is touched).
      **NOT 13, which is what the arithmetic alone suggests**: 13 is an EXACT fit with zero spare,
      which is the very condition that caused this bug -- the original 12 was also chosen as an
      exact fit, and the first service list to retain a cap broke it silently. 14 budgets for a
      second retained service cap. Ceiling is 16 on xmc (`cap.h KCAP_INDEX_BITS`) and 15 on k64f
      (`16 threads x HANDLES + MAX_ENDPOINTS <= 255`).
      **Witnessed on silicon 2026-08-01**, both boards under enforcement: `ok 19 - mutex_deadlock`,
      `# skipped: 0`, `1..71` all pass (`.session/logs/m461-{xmc,k64}-selftest-h14.log`). Cost is
      +160 B bss on xmc and +288 B on k64f, against 29 KiB and 125 KiB of arena. `bluepill-c8-st`
      and `microbit` still link and the sim gate is 24/24.
- [x] **It was NOT an M4.6.1 regression, which is why the fix is provisioning and not a revert.** `SKIP pool too small` on `xmc4800-relax` and `frdmk64f`, and identically at
      M4.5.5 (`28d8af6`) on both -- `.session/logs/m455-xmc-selftest.log` and
      `m455-k64f-selftest.log`, 1 skipped each. Checked precisely because M4.6.1 twice pushed a
      shared-test static over a board's arena; this is not a third instance. XMC heap moved
      30 KiB -> 29 KiB across the same span, which is the milestone's code growth, not a leak.
      **STATE.md's "Skips 0 everywhere except `microbit`'s 10" was therefore WRONG on two counts**
      and is corrected.
- [ ] **Captures stamp `ab2a52d-dirty`.** The tree carried the uncommitted f302 marker
      instrumentation (guarded `OFF`, so the selftest images are functionally unaffected) plus doc
      edits. M4.5.6 wrote the rule "commit before a witness pass" and this pass broke it again. The
      selftest evidence stands -- the instrumentation compiles out and touches no selftest TU -- but
      the stamp cannot prove that on its own, so **re-stamp these four captures against a committed
      tip** when the marker work is reverted.

**Rig lesson, and it cost two captures.** On the ESP boards the flash -> arm-reader -> reset-as-a-
separate-step rule (correct for J-Link and ST-Link, where flash and console share one probe) actively
CORRUPTS the log: a second process opening the port to pulse RTS disturbs the already-armed reader,
and the result is a 160-byte capture that is byte-identical across runs -- the ROM banner, then a
contiguous ~2 KB hole swallowing the whole KickOS banner and tests 1..32, then a few lines, then
silence. It reads exactly like a board defect and is not one. `.session/cap_esp.py` exists precisely
for this: it resets and captures on ONE serial handle. With it the same board returned a clean
`1..67`. Both bad captures are kept for comparison (`m461-lx6-selftest.log`,
`m461-lx6-selftest2.log`) alongside the good one (`m461-lx6-selftest3.log`).
**Second, smaller trap:** these serial logs contain NUL bytes, so plain `grep` silently treats them
as binary and prints nothing at all -- not even "Binary file matches". Use `grep -a`, or a skim of
`tail` will disagree with a `grep` of the same file and the `grep` will look authoritative.

## f302nucleo fault-reporter bench pass (2026-08-01, at commit 97a85e4) -- CLOSED 2026-08-13

First bench measurements on this defect since it was filed. They NARROW it substantially and kill
the framing it was filed under. Captures in `.session/logs/f302-*`; the banner stamps `97a85e4`, a
committed tip.

- **The console is fully healthy, on BOTH paths.** The boot banner comes out complete -- that is
  `arch_console_write_sync`, the polled writer -- and `hello` then prints continuously past 69
  ping/pong lines, which is the BUFFERED ring drained by the TX interrupt. So USART2, the ring, the
  TX IRQ and the NVIC line all work. Any explanation that starts "the console is broken" is out.
- **`fault` emits EXACTLY TWO characters** of its first line, `[f` of
  `"[fault] executing an illegal instruction"`, and then nothing ever again. Two is the signature of
  the ring's PRIME: `console_tx_write` writes one byte to `TDR` itself and a second lands in the
  shift register, leaving the rest of the line queued when the illegal instruction executes.
- **So the flush is silent too, and that is new.** At the fault `kpanic_enter` should do three
  things in order: `console_tx_flush_sync()` the ~58 queued bytes, reclaim the console to the polled
  path, then print the dump. NONE of the three produces a byte. The filed description --
  "the fault reporter produces no dump" -- is therefore too narrow: the failure is at or before
  `kpanic_enter`, upstream of the reporter, and it takes the queued application text with it.
- **LD2 was OFF throughout the `fault` run, and the POSITIVE CONTROL PASSED**: `blink`, which drives
  the same pin through the same `arch_diag_led_*` backend, blinks LD2 normally on this board (both
  observed by the maintainer, 2026-08-01). The pin, the LED and the backend are therefore good, so
  the OFF reading is evidence and not an absence: **the CPU never reaches `kfault_terminate`.**
  PB13 is also confirmed correct for this target -- the F302R8 wires LD2 to PB13, not the Nucleo-64
  usual PA5.
- **Next probe**: bisect INSIDE `kpanic_enter` rather than chasing the
  reporter -- flush, then reclaim, then banner -- since all three are now known silent and the LED
  says the chain does not reach its end. `arch_console_reclaim` on this chip is a candidate: check
  whether stm32f302 defines one or links the no-op.
- **Rig correction**: the console came out on the ST-Link V2.1 VCOM at `/dev/ttyACM1`
  (`usb-STMicroelectronics_STM32_STLink_0675FF...-if02`), NOT on an FTDI wired to PA2 as
  `CONTEXT.local.md` describes for the STM32 fleet. Flashing is plain
  `st-flash --connect-under-reset write <bin> 0x08000000`, then arm the reader, then `st-flash
  reset` as a separate step so the banner is not lost.

## Found verifying the esp32 tree against its TRM (2026-08-01)

The manual arrived and every in-tree register VALUE proved correct. Two citations and one semantic
claim did not. Recorded because the semantic one is the sort that only surfaces as a hang.

- [x] **FIXED: `regs/uart.h` described TX-empty in a way that would stall every long burst.** It
      said the source is "a LEVEL source ... so `INT_ENA` (not `INT_CLR`) is what gates it". The
      CONDITION is level on occupancy, but the RAW bit is a LATCH (`ST == RAW & ENA`, dropped only
      by writing 1 to `INT_CLR`; TRM v5.8 Table 31.6-4). A driver author trusting that sentence
      skips the per-push clear and hangs the moment a burst fills the FIFO. The kernel's polled ring
      never noticed because it gates on `INT_ENA`.
- [x] **FIXED: two wrong chapter citations in the esp32 headers.** The peripheral base was cited as
      "TRM 1.5.3" -- chapter 1 is the ULP coprocessor, the table is 3.3-6 in chapter 3 -- and the
      Timer Group as chapter 18, which is the RNG. Both in `mmap.h`, and the first repeated in
      `chip_esp32.cc`'s file header.
- [x] **FIXED: a stale HW-unverified caveat.** `CONF1.TXFIFO_EMPTY_THRHD` was flagged as
      unverified width and position; it was right all along ([14:8], 7 bits, Reg 19.10). A caveat
      that outlives its doubt is not free -- the demux reads that field's VALUE.
- [ ] **Consider the per-SOURCE interrupt mask this port does not use.** TRM 8.3.3: writing an
      INTERNAL interrupt number into a peripheral's map register disables that source for the CPU.
      That is finer than the `INTENABLE` bit the port masks with, which is per-CPU-interrupt and so
      necessarily coarse when sources are OR-ed onto one line. Not adopted, and the reason is sound
      (INTENABLE is core-local and single-cycle; the map write is an APB round-trip inside an ISR).
      It becomes the answer if a second peripheral ever has to share CPU interrupt 13.

## Found while writing the M4.6.2 USB design gate (2026-08-01)

- [x] **Neither RP chip ever configures `PLL_USB` / `clk_usb`, and the ROSC degraded path needs a
      USB refusal.** A USB device block cannot run off an unconfigured USB clock, so this is a
      prerequisite for M4.6.2 rather than part of it. The related hazard: on the degraded ROSC path
      the frequency is not USB-legal at all, so the driver must REFUSE to enumerate rather than
      enumerate wrongly and present a broken device to the host.
- [ ] **No `arch_console_reclaim` body on `picopi`, `pizero2350` or `teensy41`.** Same gap the UART
      work hit on esp32 and rx72m: a console driver death leaves those boards dark. It bites harder
      for USB, because the panic path there must also deal with a host that may have gone away.
- [ ] **`arch_reboot` is selftest-only on all three USB boards**, and `imxrt1062`'s `bkpt #251`
      HalfKay handover is recorded in-tree as BENCH-UNWITNESSED and not vendor-documented. The
      M4.6.2 section states it as fact; it is not one.
- [ ] **The RP2040 DAP-power-up failure is an OPEN bug with an UNCONFIRMED cause, and the
      workaround recorded for it does not exist.** Two separate corrections to what the notes imply,
      the second confirmed by the maintainer on 2026-08-01.

      First, the flag. Its name is kept in a fenced block below, because naming it in prose would
      fail the very gate that proves the point:

      ```
      KICKOS_RP2040_DEBUG_KEEPALIVE
      ```

      `CONTEXT.local.md` presents it as a default-on build flag that busy-idles core0 so the DAP
      stays live. `git grep` finds ZERO hits across every tracked file, so it is not merely
      disabled, it is absent.

      Second, and this is the part that matters more: **there has never been a working fix, and the
      root cause is not established.** The recorded explanation -- that the RP2040 auto-sleeps when
      both cores WFI and gates the debug bus -- is a HYPOTHESIS that was never confirmed, so it
      should not be repeated as a finding. What is actually known is only the symptom: the first
      flash of a freshly-powered board works, and once KickOS runs, J-Link finds the SW-DP and then
      fails to power up the DAP, after which reflashing needs a power-cycle.

      Consequences. Operationally, a `picopi` running KickOS needs a power-cycle to reflash, full
      stop, and any bench plan that assumes otherwise is wrong. For M4.6.2 it is worse than an
      inconvenience: the design gate asks whether the tickless idle path keeps the USB controller
      clocked, and the honest answer is that this board's idle behaviour is not understood well
      enough to predict it -- the one existing datapoint about idle gating a peripheral is itself
      the unproven hypothesis above. So that question must be settled by measurement on the part,
      not by reasoning from the DAP story. `CONTEXT.local.md` is maintainer-owned and gitignored, so
      the correction there is reported rather than made.

## Found while writing the per-chip UART drivers (2026-07-31)

Four items from the four bench-board drivers. None is a regression from that work; two are
pre-existing defects it FIXED and two are holes it exposed and could not close.

- [ ] **The RX72M group VECTOR is still claimable, so a thread can starve a whole group.**
      `docs/design-m4.6-irq-driver.md` section 9.3 rules that the group vector (110 for GROUPBL0)
      must not be a claimable logical line -- a thread owning "the group" could starve every source
      in it -- but nothing ENFORCES that: `kos_irq_claim(110)` is legal today. The fix is a
      kernel-side line-admissibility hook so a chip can declare a line un-claimable, which is why
      the driver work could not close it (arch and driver code cannot refuse a kernel syscall).
      Until then it is a footgun, not an exploit: only an `AUTH_IRQ` holder can reach it.
- [x] **The ESP32-C6 CPU-interrupt enable the tree uses is not in the TRM.** `arch_rv_hw_unmask`
      sets a bit at `0x2000_1000 + 0x00`, and TRM v1.2 documents NO register at that address -- the
      documented enable is `INTPRI_CORE0_CPU_INT_ENABLE_REG` at `0x600C_5000 + 0x0000` (TRM section
      1.6.2 item 2 p.55, section 10.4.2 / Reg 10.64 p.395, INTPRI base p.177; the CLINT is at the
      CPU-subsystem base + 0x1800, which is also not this). The new `arch_rv_hw_mask` deliberately
      clears the SAME word its twin sets, because symmetry is the requirement and the unmask path is
      silicon-validated -- but that makes the pair consistent, not correct. Either `0x2000_1000` is
      an undocumented alias that happens to work, or the mask is writing nothing and only `mie`
      is doing the masking. **Settle it on the bench**: claim a C6 line tier-1 LEVEL, let the source
      assert, and check that exactly one `irq_wait` returns rather than a livelock. If the alias is
      wrong, both halves move to the INTPRI address.
- [x] **FIXED: the C6 kernel rearm path was wiping the driver's `UART_INT_ENA` on every ack.**
      `arch_rv_hw_unmask` carried a destructive `INT_ENA = 0` / `INT_CLR = all` / threshold block
      that ran on EVERY rearm. Before a userspace grant that was merely wasteful; after one it is a
      RULE L1 breach -- the kernel reaching into a block granted to a driver and clearing the
      interrupt enables the driver had just set, once per ack. Moved to a one-shot quiesce at
      bring-up, so only kernel-owned registers remain in the rearm path. The demux likewise no
      longer writes the UART block at all, which was eating the owner's error latches.
- [x] **FIXED: the RX72M console TXI ISR ignored the handover.** `kickos_rx_console_txi_isr` drove
      the kernel ring unconditionally, so after `console_tx_deinit` it would still run
      `console_tx_isr`, which clears `SCR.TIE` behind a userspace driver. Since the INTB slot is
      fixed in flash on this part, that made a userspace TX driver impossible on rx72m rather than
      merely racy. It now branches on `console_tx_armed()`.

## Found while ruling the XMC console seam (2026-07-31)

Both came out of an RM pass for M4.6.1's `TBIEN` question and neither is about the UART. Recorded
here because they are pre-existing isolation facts, not things that pass created.

- [ ] **`FMR.SIOx` lets any window holder pulse ANY of USIC0's service-request nodes with one
      unprivileged store**, so a per-entry seam mask on a USIC interrupt-enable bit is mostly
      blast-radius documentation rather than a boundary. RM V1.3: `FMR` bits 16-21 are `w`,
      classified `U,PV` (`docs/reference/porting.md`), and "writing a 1 to this bit field
      activates the service request output SRx of this USIC channel". `SR[5:0]` are MODULE-scope,
      shared between both channels (RM 18.7, p.18-153), so the target NVIC line may belong to
      another driver or to the kernel. The sibling escape is already WITNESSED: `INPR` is `U,PV`
      and the `inprstorm` capture re-points SR0 from the U0C1 window
      (`user/apps/xmc4800-relax/inprstorm`, `TODO.md` M4.5.6). **Not a regression and not newly
      opened** -- the M4.5.6 verdict on that class was a bounded CPU tax rather than a DoS, on the
      structural `min(fill, drain)` argument. What is owed is honesty in the seam's own comments:
      they should not imply a mask closes what two unprotected registers leave open.
- [ ] **Whether U0C0's `KSCFG.MODEN = 0` gates the whole USIC0 module, which would dark
      `xmcssc`.** RM p.18-153 says "if the module clock is disabled by KSCFG.MODEN = 0, the module
      cannot be accessed", but `KSCFG` is a PER-CHANNEL register at `U,PV`, and the RM text read so
      far does NOT settle whether one channel's `MODEN` gates the module or only its own channel.
      If it gates the module, an unprivileged U0C0 holder can silently kill the SPI service on
      U0C1 -- a cross-channel escape that no seam mask touches, because the store needs no seam.
      **Cheap to settle on the bench**: `pvprobe` already has the shape. Pre-existing, and
      independent of the M4.6.1 console work.

## Found during the M4.5.9 design-tier pass (2026-07-31)

- [ ] **RX72M's ICU reserved block is too small, so Rule 7 cannot refuse an over-broad grant over
      the group registers.** A live grant-admissibility hole, recorded only in
      `docs/design-m4.6-irq-driver.md` section 6.4 and orthogonal to the IRQ work that found it.
      `arch_reserved_blocks` (`arch/rx/chip/rx72m/chip_rx72m.cc:353-371`) reserves
      `{mmap::ICU, 0x400}` with `mmap::ICU = 0x0008_7000` (`arch/rx/chip/rx72m/include/kickos/chip_mmap.h`), so the
      window is `0x87000..0x873FF`. That covers `IR`/`IER`/`IPR` but **not** `GRPBL0 0x87630`,
      `GENBL0 0x87670`, `GRPAL0 0x87830` or `GENAL0 0x87870`. A privileged over-broad grant
      covering `0x8763x` therefore succeeds today and the Rule-7 predicate has no basis to refuse
      it, which also leaves the SYSMPU/MPU union wrong for anything that does take it. Fix: extend
      the entry to span `0x87000..0x8787F`, size `0x880`. The RX IRQ controller is MPU-GOVERNED
      memory, unlike the ARM PPB, which is why it has to be reserved at all.

## Found during the M4.5.2 stage-2 flip work (2026-07-28/29)

- [x] **An IRQ line is never released, so a driver that exits cannot be respawned. FIXED in M4.6.1**
      by making the line a capability: the binding moved from a bump array with no free path to a
      generational `SlotPool`, and `irq_ref_drop` detaches the line then frees the slot at refs -> 0.
      It hangs off `cap_teardown`, the walk `exit_current` already ran, so all three death paths
      (exit, fault-kill, voluntary close) converge with no second teardown walk. Detach strictly
      before free is load-bearing: `irq_event_isr` holds the binding's address as its pre-bound arg.
      The documented path this used to contradict now works: `user/include/kickos/sys/spi_service.h`
      says `serve_loop` returns on `EPIPE` so root can respawn, and the respawning thread's claim of
      the same line now succeeds.
- [ ] **`kos_bus_cfg.cs_index` is accepted and never interpreted.** `k64dspi` drives one hardwired
      GPIO CS (`PTC4`) and `xmcssc` one fixed `SELO0`, so neither `fold()` reads the field, neither
      bounds it, and neither refuses an out-of-range value. Harmless while every driver has one CS
      line, and a trap the moment one has two: the M4.5.2 device slots let a client configure slot 0
      and slot 1 with different `cs_index` values and get the same physical line. `bus-service.md`
      and `bus.h` now say so; a multi-CS driver has to read and bound the field, and that is when
      the `-KOS_EINVAL` refusal the contract wants becomes real.
- [x] **The tier-1 mint was completely ungated. FIXED in M4.6.1**, no seventh authority bit added.
      Statement of record: `docs/design-m4.6-irq-driver.md` sections 2.1 and 3.6.
- [ ] **FOUR in-tree apps grant a DEV window a live board-service driver already holds, so the
      M4.5.2 one-holder-per-window check (`domain_for` -> `-KOS_EBUSY`) now refuses their spawn.
      Silicon-only: no in-env gate covers any of them** (all are `kickos_add_diagnostic_app` or a
      hardware-observable demo, none has a CTest gate), so nothing goes red until the next bench run.
      The same gap covers the suite itself: `dev_window_exclusive` and `bus_device_slots` postdate
      every silicon capture, so the case totals stamped in `docs/reference/boards.md` are right for
      their commits and neither new case has ever run on a chip.
      Verified statically on `xmc4800-relax-st -DKICKOS_HAVE_MPU=1`, whose service list resolves to
      `kickos_services_xmc4800relax` (`xmcuart` U0C0 + `xmcssc` U0C1) -- the `xmcspi` and
      `consoledemo` ELFs both carry `kickos_board_services`, so both drivers are up before `main`.
        - `xmcspi`, `xmccshold`, `pvprobe`, `inprstorm` each grant `U0C1_BASE`/`0x200` =
          `[0x40030200,0x400303FF]`, the exact window the `xmcssc` bus service holds. This is a REAL
          pre-existing conflict, not a false positive: two drivers configuring one USIC channel. The
          four predate `xmcssc` joining the service list (M4.4) and silently became conflicting then.
          Run them against a console-only service list
          (`-DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_console`, an existing provider) so U0C1
          has no other holder. Note M4.5.6 changed what these four DO without changing this conflict:
          they now reach FDR/BRG/CCR through `arch_periph_reg_write` instead of writing them directly,
          but they still grant the same U0C1 window, and the one-holder check is about the window.
        - `consoledemo`'s scrambler grants `0x40030000`/`0x200` = the exact window the unprivileged
          `xmcuart` driver holds. Here the double grant is the POINT (garble a live console, prove
          `arch_console_reclaim` recovers it), so the check structurally obsoleted the way it was
          staged. **RESOLVED in M4.5.6, and not the way this entry first proposed**: the scrambler is
          now its own app, `conreclaim`, REGISTERED only when `KICKOS_SERVICE_LIST` already resolves
          to `kickos_services_none` -- a kernel-driven console with no DEV holder anywhere, so the
          scrambler is the sole holder. The scramble-test build option no longer exists.
      **The remedy shape is the part worth keeping, because the obvious one is unbuildable.**
      `KICKOS_SERVICE_LIST` is ONE global cache variable, resolved in the root `CMakeLists.txt` before
      any subdirectory is added, so an app's own `CMakeLists` can never set the list it needs -- it can
      only observe the one already chosen. So the encodable form is a REGISTRATION GATE, not an
      override: register the app when the resolved list is compatible, and otherwise say at configure
      time which list is holding the window and stand down. That is exactly what `conreclaim` does (a
      `message(STATUS ...)` naming the resolved list, then `return()`), which is louder than a silent
      skip and cheaper than a `FATAL_ERROR` that would break every unrelated build of the tree. The
      four U0C1 apps remain a per-image build discipline -- the caller passes the console-only list at
      configure time -- and giving them the same registration gate is the open half of this item.
- [ ] **Respawn vs `-KOS_EBUSY` on the device window -- documented, cannot bite today, revisit with
      SMP or a higher-priority supervisor.** `spi_service.h` says `serve_loop` returns on EPIPE so
      root can respawn; a respawn issued while the dying driver still references its domain would now
      earn `-KOS_EBUSY`. Two independent reasons it cannot happen now: (a) `sched::exit_current`
      calls `cap_teardown` (which EPIPE-wakes the parked respawner) and `domain_release` in the SAME
      `IrqLock` critical section, `cap_teardown` first, so a woken supervisor always observes the
      window already free; (b) root runs at `KICKOS_PRIO_MIN + 1` = 2, below every service driver
      (11-12), so on single-core it cannot preempt a driver between `serve_loop` returning and
      `exit_current`. Opens if a supervisor ever outranks a driver, on SMP, or if death is detected
      any other way (watchdog/timeout/a future join) -- then join before respawning, or retry on
      `-KOS_EBUSY`. Note the respawn path is ALREADY broken for an unrelated reason (the IRQ-line
      entry above), so no in-tree caller exercises this yet.
- [ ] **`pvprobe` and `inprstorm` print via `kos::print`, not `kickos::emit`**, so their output is
      silently dropped on any board whose console has been published to a userspace driver. Only
      `rootfault`, `mpu_fault` and `rebootdemo` include `emit.h`. The fix is one include and a call
      swap, and it matters out of proportion to its size: these two are the probes the
      unprivileged-root design's evidence rests on, so a silent probe reads as a probe that found
      nothing.
- [~] **`f411spi` cannot run under the flip: its bring-up shim writes MMIO from `main`. ADDRESSED by
      stage 3, silicon-unwitnessed.** The `stm32f411` `arch_periph_enable` backend covers the SPI1
      clock gate and the pinmux encoding covers `PE3`, but `frdmk64f` was the only board on the bench
      for stage 3, so the `f411disco` run is bench debt. Found 2026-07-29 while flipping `f411disco`, and witnessed
      rather than inferred -- the app faults MemManage on the first store of `main`
      (`RCC_AHB1ENR` @ `0x40023830`, `CFSR=0x82`, `MMFAR=0x40023830`), before it ever spawns the
      unprivileged driver that holds the 32 B SPI1 grant. Same shape as `c6blink` and `rxdrv` before
      their windows were reworked: the escalation surfaces (RCC clock-enable, GPIOA/GPIOE mux) are
      deliberately kept out of the driver's window, which is exactly why they need kernel mediation
      instead of a wider grant. NOT a flip blocker -- it is a `kickos_add_diagnostic_app`, never a
      production image, and the stage-2 gate is `selftest` + `rootfault`, both green on that board.
      Its loopback arm is also still unwitnessed in the default posture (needs the PA7->PA6 jumper),
      so the chip's peripheral-window proof stays open either way.

## Found during the M4.5.2 review (2026-07-29)

- [ ] **A user-facing test suite does not exist.** The kernel selftest tests the KERNEL through the
      syscall surface and deliberately does not test the user-facing surface, so nothing anywhere
      checks that `printf`, `std::cout`, heap behaviour and libc integration work per board.
      `hello` passing is the entire coverage, and on some boards `hello` has no gate at all: QEMU
      models no `stm32f302`, so `f302nucleo` carries **no CI gate of any kind**
      (`docs/reference/boards.md`). **The two suites cannot merge**, and the `-st` presets are why --
      the kernel suite is provisioned FOR the kernel. `f302nucleo-st` (`cmake/presets/arm.json:137`)
      now runs it with `KICKOS_USER_HEAP_SIZE=0` and `KICKOS_USER_STACK_SIZE=1024`, which is
      precisely the opposite of what a user-API suite has to exercise. Sharp consequence of that
      preset: with the heap carve at zero the `-st` gate on that board no longer exercises the heap
      at all, so a heap regression on a 16 KiB part would go unseen.
- [x] **CI exercises an unprivileged root** (M4.5.5). Three new arms: `qemu` and `qemu-m33` at
      `KICKOS_HAVE_MPU=1` with root UNPRIVILEGED, then a `-D` and now the only posture (a real
      armv7m/v8m `npriv` boundary over both
      PMSA revisions), plus a flipped **sim** arm which witnessed the authority logic and region
      composition but never a CPU-mode boundary. The flipped ARM arm registered two gates the
      privileged posture could not host -- `rootfault` and `rootauth`'s flipped arm -- and a `selftest`
      whose `mpu_privileged_guard` skip was permitted by name.
      `qemu-m7`/`qemu-m3` were deliberately NOT flipped in CI: both re-run the same PMSAv7 path as
      `qemu` for two more toolchain builds. Verified green flipped by hand at this gate (13/13 and
      12/12), as was `qemu-m33` (13/13) and `qemu` (14/14).
      **SUPERSEDED by M4.5.6, and this is the arms' whole point being absorbed rather than lost**:
      with one posture there is nothing to flip, so the duplicate `build/sim-flip` and `build/$b-flip`
      arms are deleted and the BASE `KICKOS_HAVE_MPU=1` arms register `rootfault` themselves -- on six
      images, none of which registered it in a default build before. `mpu_privileged_guard` is
      retired, so no posture-dependent skip is permitted by name any more.
      Deliberately NOT done at the time: flipping `frdmk64f`'s preset default. It would have broken the
      locked order (the knob's deletion was step 4, after the 4.5.5 re-witness), given the fleet a
      third posture alongside XMC's console-only special case, and silently broken `k64drv`, which
      cannot run flipped by design. It would also have changed only what is BUILT, not what is TESTED,
      since no CI job runs frdmk64f with MPU non-vacuously. Moot now: every board is flipped.

- [x] **CI builds the enforcement gates optimised -- DECIDED: the pin is gone** (M4.5.5). The "ARM
      PMSA enforcement run gates (v7 + v8)" step passed `-DCMAKE_BUILD_TYPE=Debug` explicitly,
      overriding the fleet's `MinSizeRel` default, so the four gates covering the enforcement posture
      compiled `-O0`. Dropping it was measured first: all four are green at `MinSizeRel`
      (`qemu` 12/12, `qemu-m33` 11/11, `qemu-m7` 11/11, `qemu-m3` 10/10). Those gates now test what
      ships, and CI can reach the `-Os`-only class the K64F PIT lost write fell into.
- [ ] **`sam3x8e` over-alignment: PARKED on hardware absence, not open.** The chip HAS an MPU on
      silicon (Atmel SAM3X/SAM3A datasheet, Cortex-M3 revision 2.0) but KickOS ships no `mpu.cmake`
      backend for it, so it builds `KICKOS_HAVE_MPU=0` while still inheriting ARM's fallback
      `arch_mpu_min_region()` of 32 (`arch/arm/common/arch_mpu_min_region_default.cc`) -- costing 3,808 bytes
      of measured over-alignment on a part that enforces nothing. It is the third member of the class
      `stm32f103` and `stm32f302` just left, both of which now override to 0 in
      `arch/arm/chip/stm32f103/chip_stm32f103.cc` and `arch/arm/chip/stm32f302/chip_stm32f302.cc`.
      **PARKED by the maintainer for a concrete
      reason: the physical Arduino Due unit is dead** (`docs/reference/boards.md`), so nothing on this
      chip can ever be witnessed -- do not pick it up expecting to validate it. The class itself is
      handled by the region-encoding item under M4.5.5 above, which is where a third encoding mode
      would land.
- [ ] **Cut `bluepill-c8`'s 8 KiB heap carve: the board is predicted to fail `hello`'s second spawn
      by 96 bytes.** This is a **MODEL PREDICTION, not a witness** -- the board has no physical unit
      and can never be flashed. Arena 6,560, minus idle 512 and root 2,048, leaves 4,000 against the
      4,096 that two 2,048-byte stacks need. The cause is the carve rather than the part: 8 K
      `.userheap` (`arch/arm/chip/stm32f103/stm32f103.ld:27`) where `f302nucleo` now takes 2 K, plus the
      board raising ROOT/USER to 2048 over the chip default of 1024
      (`boards/bluepill-c8/configs/base/defconfig:9`, `:11`). Full arithmetic and its
      provenance are already in `docs/reference/boards.md`; the fix is cutting the carve. The
      prediction is worth acting on because the same model called all three `f302nucleo` silicon
      outcomes correctly -- `hello` two threads, `stress` pass, `selftest` spawns refused.
      **The boot-arena link assert cannot catch this**: `arch/common/boot_arena.ld.h` replays the
      idle and root stacks only, never the N user stacks a spawning app needs.
- [ ] **About 270 `path:N` doc citations cannot be verified by any gate, and two of two spot-checks
      had drifted -- NEEDS A CONVENTION DECISION.** `tests/static/check_doc_names.sh` (landed, deliberately
      not wired into CTest) says so itself at `:54-57`: it strips the `:N` and never checks it,
      because nothing in the current spelling says WHAT should be at that line. The failure mode:
      a citation of `user/include/kickos/sys/abi.h:36` for the cpu-clock syscall resolves to a live
      but unrelated `KOS_SYS_IRQ_CLAIM`, where the real line is elsewhere. This is the
      reused-identifier class the project already knows is expensive, and a citation resolving to a
      live but unrelated thing is worse than one that dangles. Both originally-confirmed instances
      sat in `docs/design-m3-clock-select.md` and were deleted by the M4.5.9 trim rather than
      re-anchored, so the class is unchanged and only the two witnesses are gone. **The decision is the spelling**: if a citation carries the expected symbol
      (`arch.h:84 arch_cpu_clock_hz`), the gate can check it in about two lines; until then `:N` is
      decoration. This item scopes that work only -- fixing the ~270 citations belongs to the doc
      audit, and the two instances above are deliberately left as found.
- [ ] **`arch_reboot` should take a MODE, and the compile knob should gate the MODE rather than the
      seam. Owner: M4.6, after 4.5.4.** Decision recorded in full in
      `docs/design-unprivileged-root.md` section 9, under `### The reboot capability`. Today
      `int arch_reboot(void)` (`arch/include/kickos/arch/arch.h:44`) takes no argument and means
      bootloader entry specifically, with two callers -- `kernel/init/console.cc:293` inside
      `bootloader_handover`, and the `KOS_SYS_REBOOT` dispatch arm at
      `kernel/syscall/syscall.cc:390` -- and the whole thing sits behind `KICKOS_ENABLE_SELFTEST`.
      Four parts: a mode argument (at least a normal system reset and bootloader entry); a per-MODE
      `-KOS_ENOSYS` decline instead of a per-function one; the knob narrowed to the bootloader
      mode and renamed to match the sibling `KICKOS_SHUTDOWN_TO_BOOTLOADER` (`CMakeLists.txt:117`)
      rather than spelling one destination two ways (the proposed spelling is in that design
      section); and an authority bit on top of the knob for the bootloader mode alone. What it
      buys: no production in-kernel path can reset the chip today, which costs watchdog recovery, a
      fault-handler reset and a bring-up retry for no security reason, since a normal reset carries
      none of the bootloader risk. What it retires: `KICKOS_SHUTDOWN_TO_BOOTLOADER` becomes a policy
      on one seam instead of a parallel mechanism, and syscall 38 becomes a real production syscall
      taking a mode -- answering `-KOS_ENOSYS` for a mode the chip lacks and `-KOS_EPERM` without
      authority, instead of `-KOS_EINVAL` from a dispatch default arm -- which takes the
      configure-time `FATAL_ERROR` (`CMakeLists.txt:124`) and the `abi.h:62-64` compiled-out-arm
      annotation with it. The symptom that exposed the conflation:
      `arch/arm/chip/imxrt1062/chip_imxrt1062.cc:49` forward-declares `kpanic` inside a
      `KICKOS_ENABLE_SELFTEST` block, only because that chip's `arch_reboot` is a `bkpt` that must
      not resume -- a fundamental function's declaration behind a test flag.

## Found during the M4.5.3 stage-3 work (2026-07-29)

- [x] **The console driver cannot report its own bring-up failure, by any available means.**
      `k64uart_console_start` publishes the console before spawning the driver, so the driver runs
      with `ConsoleState::USER_OWNED`, where `console_emit` is `return; // DROP`
      (`kernel/init/console.cc:133`) and RTT is compiled out on this board. `kickos::emit` is worse
      than dropped: the driver's stdout cap index 0 IS the endpoint it was spawned to serve, and
      `endpoint_send` parks on `wq_block(e->send_waiters)` when no receiver is waiting
      (`kernel/syscall/syscall_ipc.cc:137`), with no `-KOS_EPIPE` escape because `recv_holders` is
      >= 1 for its own WAIT cap. So the driver would park forever instead of exiting. Measured on
      the code, not inferred. A spawn also cannot observe a failure inside the child's
      first instructions, so root reports bring-up success either way: the board goes dark with no
      evidence. This is why `kos_periph_enable`'s failure arm there is a comment rather than a
      report. The remedy is ordering, not a call-site swap -- publish only once the driver has
      proved reachability, or have root verify before publishing -- so it is a handover redesign
      **owned by M4.6.1**. Note the same drop applies to that driver's success line and to root's own
      `k64dspi_spi_start` error prints.
      **This is the DARK WINDOW, and it is narrower than "a published console hides diagnostics".**
      The window is between the publish and the driver actually serving cap 0 (`k64uart.cc:209`,
      `xmcuart.cc:179`); it is a property of the ordering and of this driver's own cap 0, not of the
      published state as such. An ordinary app on `printf` / `std::cout` reaches a published driver
      fine (see the Blockers list above), so nothing here is an argument against publishing a console.
      **This item is what the standing "SPI-service silicon halt" blocker actually was, and the halt
      itself is refuted on both boards** (2026-07-29, `aa084a9`, captures under
      `.session/logs/m453-spihalt/`). Neither service halts: `k64dspi` reads the LAN9252 `BYTE_TEST`
      signature `0x87654321` on attempt 1 against a fitted EasyCAT shield, and `xmcssc` passes
      config, single-byte, multi-byte, null-tx and transact. The plain no-RTT builds corroborate it
      off the halted target -- DSPI0 `SR=0xC2020303` with TCF set, `U0C1_CCR=0x0000C001` (MODE=SSC),
      both cores idling in `arch_idle_wait`, neither faulted. Per-board detail is in
      `docs/reference/boards.md`. The original record is best explained as a mis-summary of
      VCOM-only captures: the paired RTT captures of those same 2026-07-25 runs already show both
      services passing, and the register state above shows the work completing in an image carrying
      no RTT at all. What remains open is therefore visibility and ordering, not a halt.

- [ ] **Consider a diagnosis preset carrying `KICKOS_CONSOLE=both`** -- no board preset sets it,
      `-st` included; NO preset in the tree sets it at all, so a bench run has exactly one transport and
      a published console takes that one away from the app. That is how the phantom SPI halt above
      survived, and any future "the service goes quiet" diagnosis over VCOM alone will re-derive the
      same false conclusion. RTT is generic in the kernel, so `both` builds anywhere, but it is only
      readable where a probe can read target RAM -- the J-Link boards (`xmc4800-relax`, `frdmk64f`)
      are where it pays. Against it: flash, and the two 64 KiB boards have the least of it. The
      bench rule that holds regardless is recorded under *Per-board caveats* in
      `docs/reference/boards.md`.

- [ ] **Audit the whole fleet for the `-Os` clock-gate-then-configure lost write.** On K64F a
      `PIT_MCR = 0` store raced the `SIM_SCGC6` gate write and was **dropped** at `-Os`, fixed by a
      consumed read-back of the gate register (`127efb5`). The same lost-write pattern plausibly
      affects other chips' gate-then-configure sequences now the whole fleet builds `MinSizeRel`.
      Record while auditing that `(void)r32(...)` does **not** serve as a read-back and that
      `-Werror` correctly rejects it: a `(void)` cast of a volatile lvalue performs no access.
      Ranked inventory from a review, unguarded first: **`mk64f arch_pinmux_set`** -- gates a PORT
      then writes that PORT's `PCR` in the same function, write-once with no self-heal, and PORTD's
      `SCGC5` bit was never set on this chip before this milestone; **guarded since `aa084a9`**, and
      the A/B below measures the store landing either way, so treat it as closed rather than as the
      inventory's leading example; **`xmc4800 ccu4_clock_init`** --
      `CGATCLR0`/`PRCLR0` then `GIDLC`, write-once, and `GIDLC` is the monotonic clock's slice
      enable; **`xmc4800 usic.cc kernel_clock_enable`** -- it has the write/read-back/barrier idiom,
      but placed AFTER the first `KSCFG` write and protecting a different documented pipeline effect,
      so the first write into the newly ungated block is itself unprotected; **`imxrt1062
      gpt_clock_init`** -- `CCGR1` then `GPT1_CR`, partially self-healing, but its `SWR`
      software-reset step could silently no-op; the **STM32 family**
      `tim2_clock_init`/`usart2_init`/`arch_diag_led_init`/`arch_pinmux_set`; **`sam3x8e`** (unit
      retired); **`esp32c6 arch_diag_led_init`** (diag only, and that same file already uses an
      explicit `fence` for its `INTMTX` writes). Two are **SAFE for a reason, not by luck**:
      `rp2040`/`rp2350` `unreset()` polls `RESET_DONE`, a real hardware completion flag and a
      stronger pattern than a read-back; `rx72m` `MSTPCR` has substantial intervening work and uses
      the read-back idiom where its UM requires it. **Second hazard variant, unflagged so far:** a
      read-modify-write on a just-gated block can write back a **corrupted** register if the read
      returns stale data -- worse than a dropped store, which at least leaves the reset value.
      `usart2_init` and `arch_diag_led_init` do `MODER` RMWs. **The `k64dspi`/`xmcssc` dropped-mux
      lead is REFUTED -- do not re-run it.** The hypothesis was that a dropped `PCR` write on PTD1
      (SCK) left the pin at its reset mux while the service still reported "up", since nothing checks
      pin state. Measured 2026-07-29 as an A/B one commit wide across `aa084a9`'s read-back: the four
      pin-map rows read their programmed mux **in both arms** -- `PORTD_PCR1`/`PCR2`/`PCR3 = 0x200`
      (ALT2), `PORTC_PCR4 = 0x100` (ALT1) -- and `k64dspi` completes its LAN9252 `BYTE_TEST` round
      trip in both. So the naked store here is not being dropped in practice. **The barrier stays**
      and is not credited with fixing this: it closes a measured hazard for 6 bytes of flash, and the
      window it closes is the tightest instance of the class in the tree -- the disassembly shows
      exactly 1 instruction and 0 intervening bus transactions between the `SCGC5` gate store and the
      `PCR` store, tighter than the PIT failure that proved the class. **The rest of the inventory
      above is untouched** -- those sites are still unaudited; only this one hypothesis died. The
      halt it was chasing does not exist either (see the console-visibility item above).
- [ ] **`k64drv` cannot run under the flip, and is refused BY DESIGN rather than pending a seam.**
      Its PIT window is legitimately granted, but the AIPS `PACR` slot that would have to open for it
      (slot 55) spans the whole 4 KiB block, including the chained ch0+ch1 pair that carries
      `arch_clock_now`, so there is no table entry and `arch_periph_enable` answers `-KOS_EINVAL`.
      This is the opposite case from `f411spi`, which a seam did fix. `k64drv` is a diagnostic app
      (`KICKOS_ENABLE_SELFTEST` only, no CTest gate), so nothing goes red; decide whether it is
      retired or reworked onto a block whose slot is containable. **The third option is gone**: with
      the posture knob deleted in M4.5.6 there is no privileged-root posture to keep it as a
      diagnostic in, so this is now a two-way decision. `arch_periph_reg_write` does not help either
      -- an allowlisted `(base, offset)` still has to sit inside a window the caller holds, and the
      obstacle here is the bus gate's granularity, not the register's.
- [ ] **`f411spi` lost its high-speed slew configuration on `PA5`/`PA6`/`PA7`**, because the pinmux
      encoding field reaches `MODER`/`AFR`/`BSRR` but not `OSPEEDR` or `PUPDR`, so those pins run at
      the reset-default low-speed slew. `BR=/64` is ~1.3 MHz (84 MHz APB2 / 64, arithmetic from the
      tree); that the default slew carries that rate is **engineering judgement, pending a DS9716
      check** -- no line in this tree supports it, unlike the other electrical claims here. Worth
      reopening if a faster `BR` is ever wanted on that bus, which is what would need `OSPEEDR` back
      -- via the encoding, not a root MMIO write.
- [ ] **`cap_console_publish` has no owner check and no once-only guard.** It drops the kernel's
      existing stdout ref and re-points `g_stdout_target` unconditionally, so any caller that clears
      the authority gate silently steals a live console -- and `KOS_SYS_ENDPOINT_CREATE` being
      completely ungated means any thread can mint the endpoint to publish. `AUTH_CONSOLE` is the
      sole thing preventing it, and the guard is wanted independently of that bit: root itself holds
      it for the length of service bring-up.
- [ ] **The CPU/peripheral clock coupling is over-generalised, and the veto should be a notification.
      Owner: M4.6**, and a CPU governor depends on it. `cpu_clock_set` refuses outright while a
      userspace driver owns the console (`kernel/time/clock_select.cc`), because the kernel cannot
      re-derive a baud it no longer owns. That generalises from a biased sample: exactly **two** chips
      implement `arch_periph_clock_hz` and both are coupled (`chip_xmc4800.cc` fPERIPH = fCPU/2;
      `chip_mk64f.cc` `SystemCoreClock` or /BUS_DIV). A chip with an independent peripheral root has
      no backend at all, so the decoupled case has never had to be stated, and the assumption is baked
      into the seam's own contract wording ("retune the core/bus clock") -- on a chip with a dedicated
      CPU PLL there is nothing to refuse. Make the coupling a question asked of the chip, and notify
      affected services instead of vetoing. The console is not the only one: drivers size their
      divisors off `kos_periph_clock_hz` too.
- [ ] **The possession gate has no test distinguishing exact-base from containment.**
      `caller_holds_mmio_block` matches `r.base == base` exactly. Widening it to a containment test
      would pass both mutations `periph_enable_unheld` was checked against, so that regression would
      ship silently. Untestable in-env: the sim's `arch_mpu_region_encodable` is unconditionally
      false, so no DEV region can exist there. The only route is a hosted unit test that fabricates a
      `Thread` plus an `arch_mpu_region` array and calls the predicate directly. The harness now
      exists (`tests/unit/kfixture`); the arm is still owed.

## Needs hardware (bench time, not code)

Every item here is scheduled: it comes due in the M4.6.3..N witness pass, which is deliberately last
so that nothing before it waits on bench access.

- [x] **v6-M MPU programming has zero coverage anywhere. PAID 2026-08-10 by `picopi`**, the RP2040
      this item was waiting on: armv6m enforcement, a first for the project. Capture archived in
      `docs/archive/M4.7-M4.8.1_fleet_selftest_meas.md`; the three failing arms and the working
      armv6m fault reporter are in the picopi section below. QEMU still models no Cortex-M0+ or
      Cortex-M23, so this class stays silicon-only.
- [x] **M7 speculation class** -- already covered by validated Teensy silicon (the imxrt
      MPU-enforce hang record, `docs/design-teensy-mpu-hang.md`); recorded here so the gap list
      stays honest about what *is* already covered.
- [ ] **`arch_reboot`'s Teensy 4.1 half (`bkpt #251` -> HalfKay) has never run.** RP2350
      (pizero2350, BOOTSEL) is witnessed via `rebootdemo`, and the RP2040 half is now paid too,
      though not via `rebootdemo`: the picopi USB CDC console runs record
      `KICKOS_SHUTDOWN_TO_BOOTLOADER` surviving the fault path (`kickos_terminate` reaches
      `arch_reboot`), so a faulting picopi image returns itself to BOOTSEL/UF2. The Teensy path
      remains the least certain of the three: it is not vendor-documented, and on non-Teensy
      RT1062 hardware the `bkpt` faults instead.
- [ ] **ONE stage-4 per-app authority witness is left, and it is BLOCKED on board access, not on
      work: `f411spi` (F411-disco, PMSAv7).** Three apps were owed -- the others, `c6blink`
      (ESP32-C6, PMP NAPOT) and `rxdrv` (RX72M, RXv3), **were both taken in M4.5.6** at
      `270b6fa`/`270b6fa-dirty`, and `rxdrv` also ran the `kos_periph_enable` possession probe. The
      claim in each case is that a per-app `KICKOS_APP_AUTHORITY` declaration carries a board's OWN
      pin muxing on real silicon, and nothing substitutes for the board: the sim can never hold a DEV
      region (`arch_mpu_region_encodable` is unconditionally false there).
      This is the SAME debt the M4.6.3..N ledger carries, not a second one. Both places now use the
      stage-4-authority framing rather than the older "`f411spi` mux write": the mux write is the
      mechanism, the authority declaration is the claim.
      `rx72m`'s coupling to M4.5.5's region re-encoding is discharged: the one visit covered both.
      `f411disco` is a pow2-required backend that M4.5.5 does not move, so its capture is durable
      whenever taken.
      Since M4.5.4 merged without them, this is debt against `master`, not a merge gate.

## M3 -- landed so far (2026-07-20)
- [x] `sys_cpu_clock_hz()` read syscall; [x] per-task capability handle table (sem ABI) +
      authenticated-grant delegation; [x] priority-inheritance mutex (CAP_MUTEX). All on master,
      silicon-validated UNDER ENFORCEMENT on K64F (SYSMPU) + C6 (PMP). Design in Book ch.8.1 +
      `reference/architecture.md`.

Remaining M3 (to finish the milestone) -- gated flow (fable design review -> branch -> silicon):
- [x] **Writable user-pointer bound-check** at the syscall boundary (arch-neutral) -- landed
      ade1879 (`user_writable_ok`; clock_now retrofitted). A recv into an unchecked out-buffer was a
      privileged write oracle; the endpoint recv buf + badge-out reuse it.
- [x] **Endpoint/IPC object (CAP_ENDPOINT)** -- additive per the stage-I endpoint design
      (fable-reviewed, never filed as a doc; what shipped is narrated in
      `docs/book/endpoints-synchronous-ipc-by-rendezvous.md`): `SlotPool<Endpoint,N>` +
      `endpoint_refs` + `recv_holders` (struct field) + one `cap_resolve` case +
      obj_ref_inc(rights)/drop and obj_close_protocol (EPIPE-wake) arms;
      synchronous rendezvous, kernel-copied bounded payload, parks on the shared `wq_block`/
      `wq_pop_highest` primitive; send/recv/create syscalls 26/27/28 (recv gated on the writable
      check). Aliases/object-side badging DEFERRED (root-only; console needs one unbadged cap).
      Landed on master; SILICON-VALIDATED UNDER ENFORCEMENT on K64F (SYSMPU) + XMC4800 (PMSA),
      selftest 39/39 each incl. the HAVE_MPU-gated endpoint_bound + crossdomain (emulator qemu
      armv7m + qemu-riscv 37/37). Rest of the fleet build-only (only k64f/xmc on the bench).
- [x] **Console device handover** -- `ConsoleState{KERNEL_OWNED,USER_OWNED,RECLAIMED}` drop-routing,
      `console_tx_deinit` (USER_OWNED set last) + the B1 in-flight-writer drain, `kos_console_publish`
      (#29, privileged), stdout cap seated at index 0, `_write` probes `kos_send(0)` then falls back.
      Userspace polled XMC UART driver (`system/driver/xmc4800/xmcuart` + `consoledemo`). SILICON PASS on XMC:
      end-to-end app printf -> IPC -> userspace driver -> wire, under enforcement.
- [x] **Panic-path console reclaim** -- `arch_console_reclaim` per chip (XMC full in-window rewrite,
      KSCFG.MODEN-first; K64F uart0 + zero MODEM/C3/S2/IR/C7816), `kickos_isr_fault`->`kpanic_enter`
      funnel (all 6 arches audited safe), driver-death EPIPE-wake. SILICON PASS on XMC (scramble-then-
      panic: banner survives a driver-garbled UART; one intrinsic leading line-transient byte, doc'd).
      K64F reclaim built + reviewed, silicon-pending (no K64F console driver yet). Porting invariant in
      `reference/porting.md`.
- [x] **User-selectable CPU clock / low-power mode** -- `arch_cpu_clock_set` mechanism seam + syscall
      30 (privileged) + coherence tail (epoch re-anchor sole mult-writer, baud re-derive, timer re-arm,
      USER_OWNED refusal). SILICON PASS on XMC (144/48) + K64F (120/20.97): monotonic `now` across
      retune, ratio-correct timing, no fault. XMC full retune, K64F staged; other chips fallback-0.
      Policy -> future userspace power-manager/clock-tree service (roadmap). Read side already landed.
Silicon target for the handover: the CPU-side-MPU boards (XMC/RX/C6) where per-thread peripheral
isolation is real; K64F is coarse-AIPS (documentation, not enforcement).

- [x] **Teensy 4.1 (i.MX RT1062, M7) MPU-enforce hang -- ROOT-CAUSED AND FIXED @c072712.** M7
      speculation past the populated 8 MiB into an unbacked AHB slave, stalling forever with NO
      fault to report (NXP ERR011573 / Arm 1013783-B); fixed by the shared `kickos_arm_mpu_fixed`
      seam in `arch/arm/common/`, with the aperture wrap ordered BEFORE the I-cache enable.
      Enforcement selftest went from a hang to a full pass with a clean soak. Full record:
      `docs/design-teensy-mpu-hang.md` (LANDED); teaching is Book ch.7.6. Its residuals are tracked
      separately: D-cache default-on is done (below), "Option B" is the fleet-wide M8 item, and
      the reprogram-window / HFNMIENA bypasses are accepted in the design record.

Book + exploratory (M3-adjacent, not milestone-gating):
- [x] **Book chapter: the syscall mechanism** -- landed as ch.3.9,
      `book/the-syscall-path-trap-dispatch-return.md`. Slotted in part 3 (the trap/interrupt model)
      rather than 2.x: it needs 3.5's saved-state + deferred-switch vocabulary to say why the
      handler must not dispatch, and it is what part 4's per-ISA tour then instantiates. Chapter 0.2's
      dangling "see 3.5 for the trampoline" forward reference now points here. One correction to the
      brief: `read`/`open`/`socket` are not IPC clients, they are link-only libc bottom-edge stubs
      (only `_write` routes over IPC, to the caller's stdout cap, with the kernel console as
      fallback), so the chapter teaches the rule (a real one belongs to the server that owns the
      device) rather than claiming an implementation.
- [ ] **Exploratory spike: microkernel IPC performance** (M3 #4 -> M6). The Mach-era "IPC too slow"
      critique vs the L4/seL4 answer -- (a) fast SYNCHRONOUS IPC (direct switch to the woken
      receiver + register/bounded-copy; KickOS's sem_post already hands the token off and drives an
      immediate switch, so the fastpath shape exists) for control/RPC, and (b) shared-memory + async
      notifications (non-blocking) for throughput -- the M6 cross-core design
      (`docs/design-m7-smp.md`) already uses an SPSC ring + doorbell, exactly that shape.
      Survey the literature, map both to CAP_ENDPOINT (#4) + the M6 rings, recommend the
      control-plane-vs-data-plane IPC strategy + a micro-benchmark. Good deep-research candidate.

## Clock hardening (2026-07-20) -- clock off the debug-domain / narrow counters
Root cause: v7-M `arch_clock_now` used DWT_CYCCNT (core DEBUG power domain), sw-extended 32->64.
On K64F+XMC silicon DWT intermittently returns aliased garbage -> phantom 2^32 wrap -> clock
leaps ~35 s -> every timed wait strands (intermittent ~50-75%, silicon-only). Masqueraded as a
"test-5 stall" and invalidated this session's earlier single-run silicon claims. Fragility class
= narrow counter + sw wrap-extension (fails via a bad read OR a missed wrap). Fix = a wide,
reliably-readable, NON-debug free-running peripheral counter. Book ch.2.1 teaches it.
- [x] **K64F** 64-bit PIT -- SILICON 20/20 (+ mutex 10/10, under enforcement).
- [x] **XMC4800** 64-bit CCU4 (4 slices concat) -- SILICON 18/18; fixed fCCU WFI-gating (SLEEPCR).
- [~] **F411/F302** TIM2(32b), **F103** TIM2->TIM3 chained, **SAM3X** TC0 ch0(32b) -- on master,
      reviewed+fixed (f103 tear-discriminator; per-timer overflow-IRQ wrap observer; f411 APB1LPENR).
      **BUILD-ONLY, SILICON PENDING.**
- [~] **ESP32 (Xtensa)** 64-bit TIMG0 (UPDATE-latch) -- also fixes a latent CCOUNT WAITI-freeze.
      **BUILD-ONLY, SILICON PENDING.**
- RISC-V (CLINT mtime) + RX (CMTW): already sound, unchanged.

Silicon-test-later (fleet+Xtensa; `.session/*-clock*.patch` are backups):
1. idle-wrap observer: quiescent > 1 wrap period (51/67/59/102 s) -> clock still correct.
2. f103: soak across chain wraps -> no +59.6 s leap, no backward stall.
3. rate/monotonicity vs wall clock (2x error = wrong Hz); no backward step under IRQ load.
4. WFI keeps counting (f411 APB1LPENR; sam3x FSMR Sleep-not-Wait; Xtensa TIMG UPDATE-latch settle +
   DPORT ungate assumption -- the two things unverifiable build-only).
5. overflow lands in the chip clock ISR (NVIC TIM2=28/TIM3=29/TC0=27, RM-sourced).
6. debug-halt > 1 wrap period loses a wrap (DBGMCU freeze unset) -- bench artifact, not a bug.

Clock follow-ups (not blocking): arch_trace_now + KICKOS_BENCH still read raw DWT/CCOUNT (telemetry
may glitch on K64F/XMC -- tolerable, NOT the scheduler clock); ticks->ns epilogue duplicated ~7x
(hoist an arch/arm/common helper).

## M1 -- clocks (fleet audit 2026-07-09; detail in `docs/archive/M1_state.md`)

Every board's timing math is ACCURATE (no ESP32-C6-class constant bug survived the
audit). Remaining work is boards that never raise their PLL, so they run far below
capability and their benchmarks reflect a slow core. Each fix = raise PLL **and**
update `SystemCoreClock` in the same step so the ns<->tick math stays coherent.

- [x] **ESP32-WROOM: PLL bring-up 40 -> 240 MHz** -- DONE, validated on silicon 2026-07-09.
      6x confirmed by a SystemCoreClock-independent host-wall-clock spin (2203 ms @240 vs
      13020 ms @40); selftest 14/14, console clean at the recomputed 80 MHz-APB baud, 0.4 s
      beat coherent. No BBPLL lock bit on this chip -> hardened with a bounded RTC-slow-cycle
      barrier (esp-idf's mechanism) around the power-up + before the source switch.
- [x] **RP2040: PLL_SYS bring-up 12 -> 125 MHz** -- DONE, validated on FIRST SILICON
      2026-07-09 (the RP2040 port had never run on HW). selftest 14/14 at 125 MHz over
      UART0/GP0; 125 MHz confirmed by a fixed-spin interval (2573 ms/20M = 16 cyc/iter @125,
      physically impossible at 12); XIP survives the clk_sys switch (boot2 SCK=31.25 MHz
      risk resolved -- code runs from flash at 125). Watchdog `/12` tick kept on clk_ref=XOSC
      so the 1 MHz TIMER stays correct.
- [x] **SAM3X8E / Arduino Due -- port validated on silicon 2026-07-09** (selftest 14/14,
      84 MHz PLL, `-b` GPNVM1 boot-from-flash + physical-RESET flashing flow). Crystal-race
      fix (bounded `pmc_wait` + MOSCXTST margin + RC fallback) landed as part of bring-up.
      **UNIT RETIRED 2026-07-14** (removed from the available-HW list): the physical board
      developed a peripheral-I/O fault -- core + flash-controller + native USB (SAM-BA) all
      verified working, but PIO output (PB27 LED) won't toggle and the UART console is dead,
      even under a provably-correct bare-metal blink flashed via two independent paths -> HW,
      not KickOS. Likely marginal all along (the MOSCXTST margin is a documented `GUESS`).
      Port stays proven; this unit is not a reliable target. See `docs/reference/boards.md`.
- [x] **XMC4800 120 -> 144 MHz** -- DONE, validated on silicon 2026-07-09: selftest 14/14
      at 144 over the J-Link VCOM (ttyACM0); 144 confirmed by the spin ratio (1938 ms @144
      vs 2306 ms @120 = 1.19 ~ 144/120). VCO 288/K2DIV=2; flash WS=4 unchanged (already
      correct); baud recomputed for fPERIPH 72 MHz. 144 was not a hard sweet-spot after
      all: the USB PLL is separate/untouched and WS=4 already covers 144.
- [ ] *(optional perf)* STM32F411 84 -> 96/100 -- deliberate sweet-spot today; only if we
      want the true ceiling. F302 is HW-capped (Nucleo has no HSE crystal);
      C6/K64F/RX72M/F103 already at max; ESP32/RP2040/XMC now at max (silicon-validated).

## M1 -- ESP32-C6

- [x] **Diag-LED (WS2812B on GPIO8) via RMT.** DONE @d76d187 [DEAD HASH: resolves nowhere -- lost to an earlier history rewrite] -- RMT ch0 (20 MHz tick),
      routed to GPIO8, RGB-ordered (red = 0xFF0000), blinks the panic heartbeat;
      validated on silicon. (Bit-bang was infeasible -- GPIO write latency > the bit
      high-time; `rdcycle` traps on the C6.)
- [x] **selftest 10-14 pass/fail on silicon.** DONE -- all 14 PASS on silicon. Two real
      bugs fixed: (1) console rerouted from the native USB-Serial-JTAG (never delivers
      app output -- CDC host-draining gating + reset re-enumeration) to **UART0**, exposed
      as a stable COM port by the board's **CH343P bridge** (ttyACM0); (2) the inject
      doorbell programmed enable/type/prio/thresh into the **vestigial INTC/INTPRI block
      (0x600C5000)** -- the C6's real interrupt controller is the **PLIC (0x2000_1000)**;
      moved the config there and 10-14 deliver. (INTPRI keeps only the FROM_CPU trigger.)
- [x] **PMP NAPOT verified on silicon.** DONE -- a locked, no-permission BOUNDED 4 KiB
      NAPOT region correctly took a store-access fault (mcause=7, mtval=page) on the C6.
      So the M2 RISC-V NAPOT track is safe: only the *all-ones whole-space* NAPOT special
      case is unhonored (the M1 bootstrap already avoids it via TOR). Probe was throwaway.

## M1 -- hardware validation (batch when units are connected)

- [x] **blackpill** (F411 25 MHz HSE) + **f411disco** (F411 84 MHz) + **f302nucleo** (F302 16 K) +
      **bluepill** (F103 10 K clone) -- all HW-validated on silicon 2026-07-14 (blackpill/f411disco
      14/14 + bench; f302/bluepill 13/14, test 11 = RAM-size limit). Only **bluepill-c8** (genuine
      20 K F103) stays build-only -- a linker variant of the already-validated F103. (The 10 K
      `bluepill` clone has since been retired -- see docs/reference/boards.md; use `bluepill-c8`.)
- [x] **K64F revalidated on silicon 2026-07-15** (OpenSDA/J-Link): full selftest streamed
      in-order over the buffered console ring; bench re-confirmed 77 cyc / 641 ns switch (=> 120
      MHz), 160 cyc / 1333 ns IRQ-entry; fault-dump verified (UsageFault UNDEFINSTR -> HardFault).
      Its distinguishing feature -- the **SYSMPU** -- is the M2 enforcement backend, so K64F's
      formal M2 sign-off (per-task MPU trap) still lands there. Not an M1 gate; M1 was never a hole.
- micro:bit / nRF51 -- **QEMU-only; silicon bring-up not planned.** The nRF51 is discontinued
      (no silicon obtainable), so it stays an armv6m QEMU vehicle (`-M microbit`). A real-silicon
      port would also have needed an **RTC-based timer** (the nRF51 M0 has no SysTick).
- [ ] Panic/console review HW-checklist: RP2040 PL011 `TXRIS`-at-rest with FEN=0;
      ESP32 UART FIFO DPORT-vs-AHB alias; RX72M `SCR.TIE`-while-`TDRE` fires TXI. (All
      flagged HW-unverified in-code.)

## M1 -- fleet parity (audit 2026-07-09)

Capability audit across all arch/chip. Fleet is broadly uniform (every arch has a real
console, tickless timer, fault-register dump, inject-driven IRQ path, M2 MPU no-op).
Divergences worth closing for M1, most impactful first:

- [~] **mk64f diag-LED backend ADDED build-only @b5c5665 [DEAD HASH: resolves nowhere -- lost to an earlier history rewrite]** (RED PTB22 active-low) -- code gap
      closed; HW confirm folds into the M2 K64F SYSMPU bring-up (K64F is not an M1 gate, see above).
      **esp32(lx6) DONE** -- GPIO2 (silkscreen D2), validated with `blink` on hardware.
- [x] **IRQ default-mask posture unified** -- DONE @5da8a38 [DEAD HASH: resolves nowhere -- lost to an earlier history rewrite]: riscv/xtensa/sim now init their
      mask all-MASKED (matching ARM/RX); the reset contract is documented in `arch.h` (all
      lines masked at reset; a driver unmasks/irq_register-arms before use). Validated:
      selftest 14/14 on sim/qemu/qemu-riscv, no regressions.
- [x] **`arch_console_write_sync` uniformly bounded** -- DONE @9fd9623 [DEAD HASH: resolves nowhere -- lost to an earlier history rewrite]: stm32f103/f302/f411,
      rp2040, mk64f, esp32(lx6), sam3x8e all wrapped their unbounded TX-ready poll in a
      spin-then-drop guard (ceiling ~40-140 ms; esp32 200000). A wedged UART now drops bytes
      instead of hanging the panic path (the Due's solid-LED hang). fault_dump gates confirm
      a drained console still emits the full dump. (esp32c6/rx72m were already bounded.)
- [x] **ESP32-C6 real peripheral-IRQ path + buffered (ring) console -- DONE** (@cc4b236 [DEAD HASH: resolves nowhere -- lost to an earlier history rewrite],
      silicon-validated). The C6 was inject-doorbell only; added its first real device-interrupt
      path: UART0 TX-empty -> interrupt-matrix source (0x600100AC) -> a dedicated CPU int (30) ->
      `switch.S` `.Lextdev` -> `kickos_rv_ext_dispatch_dev` -> the console line's ISR. Level source,
      NO PLIC claim (clears by de-assert, like the doorbell). selftest 14/14 over the buffered
      console (2048-byte ring > total output => proves the ISR drains it), inject path intact.
      *(anytime coherence -- was mislabeled "M2"; it's interrupt plumbing, no MPU dependency.)*
- [ ] *(driver-era, anytime -- NOT M2)* RX `kickos_rx_default_irq` real-peripheral-IRQ demux --
      still a stub (RXv3, a different arch than the C6, so its own work; same concept). Injected
      lines pass selftest but a real peripheral IRQ drops. The C6 `.Lextdev` design is the riscv
      reference pattern. **When the 2nd real device line lands** (fable review finding 5): the
      arch IRQ mask must reach the controller for real lines -- add an `arch_rv_hw_mask` twin (or
      gate `.Lextdev` dispatch on `g_irq_masked` + disable the source), else a tier-1 driver's
      mask-until-ack and the spurious-handler mask silently fail to stop a level source (storm).
      Unreachable today: the C6 console (line 16) is permanently owned + self-gates via INT_ENA.

## M1 -- misc

- [x] RX72M `arch_irq_unmask`: replaced the `IPR index == vector` assumption with a
      vector->IPR source table (`vector_to_ipr` + `kIprMap`); IR/IER stay 1:1, only the
      shared IPR is remapped. Byte-identical for the vectors used today (SWINT/CMTW/SCI6),
      so no runtime change now; correct for driver-era device lines. RX72M re-validated on
      silicon 2026-07-09 (selftest 14/14, rfp-cli/E2 Lite flash, SCI6 console on ttyUSB0).
- [ ] *(dev ergonomics, small)* **debug-in-sleep**: set `DBGMCU` `DBG_SLEEP`/`DBG_STOP` under a
      `KICKOS_DEBUG` gate so SWD survives the idle `WFI` (no connect-under-reset dance to reflash
      a running board). A per-chip one-liner in `arch_init`.

---

## Later -- not M1

**Milestones are keyed to their THEME, not sequence** (audit 2026-07-14). **M2 = MPU /
memory-protection enforcement**, specifically. Work that merely follows M1 is not "M2" unless
it needs the MPU -- the object-pool refactor, worst-case-ISR-latency perf, `sys_cpu_clock_hz`,
and the real-peripheral-IRQ demux are orthogonal (anytime coherence / M3-substrate), tagged
below where they were previously mislabeled.

- **M2 -- MPU enforcement** fan-out: reference pair (RISC-V PMP/NAPOT + XMC v7-M PMSA) ->
  K64F SYSMPU -> RX -> tail; + the arch-independent security model (domains, per-thread
  private stacks, syscall-arg/user-pointer validation, pow2 region placement). See
  `docs/reference/architecture.md` / `docs/m2-readiness.md`.
- **Driver era -- unprivileged MMIO drivers + peripheral-isolation ceiling** (needs the M2
  grant seam; the drivers themselves are anytime coherence). Status in `docs/m2-readiness.md`
  (Driver era subsection) + the fleet peripheral-isolation matrix in
  `docs/reference/architecture.md`.
  - [x] **MMIO-grant mechanism (task #9)** -- DONE + committed 2026-07-16.
        `kos_thread_params.mmio_base/mmio_size` (grant-at-spawn), the
        `arch_mpu_region_encodable` arch seam (exact-cover, no rounding), privileged-only
        `thread_create_call` validation, `domain_for` appends MMIO as a never-shared capability.
        PLUS a Critical fix: an unprivileged `mem_base` grant is now arena-bounds-checked
        (closed a peripheral/kernel-SRAM self-grant escalation). See `docs/design-task9-mmio-driver.md`.
  - [x] **K64F first unprivileged driver (k64drv, PIT)** -- DONE on silicon 2026-07-16;
        added the `arch_fault_report_extra` hook (K64F decodes SYSMPU CESR/EARn/EDRn).
  - [x] **SYSMPU peripheral-gating question -- ANSWERED on silicon 2026-07-16:** SYSMPU does
        NOT gate AIPS peripheral-bridge accesses under user mode; the AIPS bridge PACR does
        (per privilege+master, per 4 KB slot, NOT per-thread). So **per-thread peripheral
        isolation is IMPOSSIBLE on K64F**; it holds on the CPU-side-MPU chips (XMC PMSA,
        RISC-V PMP, RX MPU). Hardware-ceiling docs DONE (`reference/architecture.md` matrix +
        `book/peripheral-isolation-and-the-hardware-ceiling.md`).
  - [~] **F411 canonical per-thread PMSA driver (f411spi, SPI1 loopback)** -- BUILT +
        fable-reviewed; **silicon-validation still PENDING** although the disco has been on the
        bench (2026-07-29): the loopback arm needs the PA7->PA6 jumper fitted, and under the flip
        the app faults in its bring-up shim (see the stage-2 findings section). Its PMSA claim is
        no longer the only one -- `xmcspi` proved granted-works/ungranted-faults per thread on PMSA
        silicon in 2026-07 -- so this is now the STM32-family reference rather than a fleet gap.
        `docs/design-spi-driver-stm32f411.md`.
  - [x] **K64F/DSPI driver (k64dspi, DSPI0 for the KickCAT ESC SPI PDI)** -- DONE on silicon:
        the polled-FIFO transport (~10 MHz) reached OPERATIONAL against a real LAN9252. Exported
        as the `kickos_k64dspi` lib (`<kickos/driver/k64dspi.h>`, source `system/driver/mk64f/k64dspi`)
        so an out-of-tree consumer links it. Within the K64F coarse-peripheral ceiling (window
        grant is documentation, not enforcement); microkernel invariant kept (driver in userspace).
  - [x] **C6 PMP SRAM enforcement DONE on silicon** (18/18 selftest under enforcement +
        mpu_fault cross-domain trap, 2026-07-17) -- the earlier blockers (all-SRAM image /
        gp-relative small-data / code-from-RAM) were resolved. REMAINING (peripheral side,
        follow-on -- NOT needed for SRAM enforcement): a **separate APM/PMS bus permission
        unit** defaults deny-user on peripheral targets and needs a one-time global open (not
        per-thread) on top of the PMP grant before a C6 userspace driver reaches a peripheral.
        See the C6 row in `docs/m2-readiness.md` + `docs/design-c6-driver.md`.
  - [x] **RX72M MPU DONE on silicon** (selftest 20/20 under enforcement + mpu_fault
        cross-domain trap, 2026-07-17). REMAINING: m2-review-followup #5 (RX rounds
        misaligned regions instead of skipping -- fail-closed drift, build-robustness).
        See `docs/m2-review-followups.md`.
  - [x] **MPU-commit / deferred-switch soundness race -- SUPERSEDED: the seam is fleet-wide.**
        `arch_mpu_apply` stashes at the switch decision and `kickos_arch_mpu_commit` programs from
        the switch epilogue after the physical swap, on every enforcing backend. Found on
        RP2040/armv6m under mutex-chain churn (selftest test 14 HardFault; cur/MPU=chA while chC
        physically ran), first fixed there (silicon 42/42 on the 50ms x300 repro) and generalised
        since. Record: `docs/design-mpu-commit-deferred.md` (LANDED); contract in
        `docs/reference/invariants.md` (`mpu-apply-on-every-switch-in`, `arch-switch-may-defer`).
- **[M4] level-trigger tier-1 bindings.** The tier-1 IRQ contract is now latch-and-coalesce
  (a raise on a masked line latches one-deep, redelivered at unmask -- edge-safe, no lost
  pulse). A LEVEL source needs the opposite at rearm: after the driver clears the device, a
  still-asserted line must NOT redeliver a stale latch. The seam is already in place --
  `arch_irq_clear_pending` (added with the coalesce fix) discards the latch; the M4 work is a
  per-binding trigger-type bit in `IrqBinding` (default EDGE) that, for LEVEL sources, makes
  the `irq_wait`/`irq_ack` rearm do `arch_irq_clear_pending(line); arch_irq_unmask(line)` (a
  genuinely-asserted level source re-latches on its own; a deasserted one stays quiet). NOT
  added now: no user/test drives a level binding yet (milestone discipline -- the API bit lands
  with its first consumer). Phantom-defense for level devices lives here too.
- **[M4, lands with bulk-rearm] identity-free coalesced redelivery on the software backends.**
  Today sim/rv32imac/xtensa/rxv3-soft carry a coalesced redelivery through ONE shared cell
  (`pending_irq` / `g_inject_line`) + one physical doorbell, clearing the per-line pending bit
  as it is rung -- so AT MOST ONE `arch_irq_unmask` with a pending redelivery may fire per
  IrqLock region (a second clobbers the first and loses an event). Safe today (register/wait/ack
  each unmask exactly one line per lock section), but a future BULK-rearm path (re-arm many lines
  under one lock) would violate it. Fix when that path lands: stop clearing `g_irq_pending` at
  ring time; have the doorbell dispatcher drain `g_irq_pending & ~g_irq_masked`, looping
  `kickos_isr_irq` over the set bits. Contract stated at the `arch_irq_unmask` decl (arch.h).
- **[anytime coherence -- NOT M2] object-pool mutualisation** -- DONE (step 1). The semaphore
  pool is a generational `SlotPool<T,N>` (slotpool.h); the thread pool is grouped into a
  tailored `ThreadPool` struct (thread.h) -- deliberately **not** SlotPool: thread liveness is
  intrinsic (`state==EXITED`) and its generation bumps at *reclaim* (so a future join-by-handle
  can still resolve a just-exited thread), genuinely different from the sem pool, so forcing
  one pool would be false-DRY. Full unification (a shared handle codec across sems + the M3
  capability store) waits for that genuine second SlotPool-shaped case. (No MPU dependency --
  was mislabeled "M2 handle table"; it's the M3-caps substrate + anytime coherence.)
- **[anytime coherence -- NOT M2] general freeing allocator.** `arch_ram_alloc` is a
  wholesale bump allocator (freed only at reset). Default thread stacks now reclaim via a
  single-size-class intrusive free list in `ThreadPool` (thread.h) -- the special case that needs
  no size metadata (one class == `KICKOS_USER_STACK_SIZE`, link stored in the dead block). A
  GENERAL multi-size-class freeing allocator for `arch_ram_alloc`/`kos_ram_alloc` at large would
  subsume this free list. Until then, only default stacks are reclaimable. The allocator work
  should also reclaim the per-allocation ALIGNMENT RUN-UP, which is dropped on the floor today --
  see the M4.5.1 item above (it is why boot-stack allocation order is load-bearing).
- **[anytime coherence -- NOT M2] user-pointer validation at the syscall boundary.** M2 is MPU
  *enforcement*; validating a user pointer is arch-neutral kernel logic that matters MORE at M1
  (no MPU to contain an OOB access -- see the `user-args-validated-at-boundary` invariant).
  Cheap parts DONE (fable code review): thread name copied into a bounded TCB buffer (fault path
  never derefs/`%s` a user pointer); `clock_now` out-pointer null+8-byte-alignment checked;
  `thread_create_call` stack `base+size` wrap checked; `SlotPool::resolve` rejects a dirty handle top
  byte. Remaining: copy-in the `kos_thread_params` struct via a checked read, and bound-check
  writable out-pointers (`clock_now`) + the `write()` buffer against the caller's granted region
  -- this last part wants the M1 region-ownership model pinned (privileged = whole arena,
  unprivileged = `mem_base`) so it rejects bad pointers without rejecting legit threads.
- **M3 -- capabilities + authenticated grants** (seL4-principled object model), **and
  user-selectable CPU clock / low-power mode** (needs explicit per-chip clock bring-up
  first, from the audit above).
  - [x] **Per-task capability handle table (sem ABI: global ids -> per-task caps)** -- DONE,
        silicon-validated under enforcement on ALL FOUR M2 mechanism classes: K64F SYSMPU,
        XMC4800 PMSA, RX72M RX-MPU, ESP32-C6 PMP -- each 21/21 selftest under enforcement
        (incl. domain_share / mmio_grant / confused_deputy + the close-while-parked sem test).
        `CapEntry` table embedded in the TCB (`cap.h`), single `cap_resolve` chokepoint
        (per-task cap-gen then global object-gen), rights WAIT/SIGNAL/TRANSFER each enforced at a
        real site, refcounted destroy-on-last-close, `KOS_SYS_HANDLE_CLOSE` (renamed from
        `sem_destroy`), authenticated-grant spawn delegation (subset-only rights narrowing,
        validate-before-claim, B1 handle==index-on-a-fresh-table deterministic placement).
        Reference: `docs/reference/architecture.md` + `invariants.md`; teaching: `docs/book` ch 8.1.
  - [x] **`sys_cpu_clock_hz()` syscall** -- DONE @638620d, already on master (build+sim/qemu verified). Read-only
    `KOS_SYS_CPU_CLOCK_HZ` via the `arch_cpu_clock_hz()` seam (mirrors `clock_now`), value
    returned in-register (no out-pointer), each backend reuses its CMSIS `SystemCoreClock`;
    sim returns 0. selftest `t_cpu_clock_hz` covers both branches; all 5 ISAs + sim build,
    runtime green on sim/armv7m/rv32imac. Read-side precursor to user clock-select below.
- **[anytime perf -- NOT M2] worst-case ISR latency (shorten interrupt-masked critical
  sections).** Scheduler/switch-path timing, gated on a worst-case-latency probe -- no MPU
  dependency (was mislabeled "M2"). The uniform bench surfaced that under sustained syscall
  load the kernel spends too long masked. Ranked plan (see `docs/archive/M1_state.md` section 3.1):
  - [x] **R2** -- armv7m: skip the redundant BASEPRI raise + DSB/ISB on nested IrqLocks
        (only the outer raise needs them). Landed `5ba57fd [DEAD HASH: resolves nowhere -- lost to an earlier history rewrite]`. Correct (ctests green) but
        **below the current bench's noise floor** -- see the measurement gap below.
  - [ ] **R1** -- thread a single `now` through switch_to->ktime_rearm->arch_timer_arm +
        arm_slice (kills the 3x arch_clock_now pileup per RR switch; on RX each is a
        nested lock + two 64-bit divides). Cross-arch signature change.
  - [ ] **R3** -- fold the min-delta clock read past arch_timer_arm's idempotency guard
        (so an unchanged-deadline re-arm reads the clock zero times). Combine with R1.
        R3b: add the idempotent-arm guard to xtensa.
  - [ ] **R6** -- xtensa: its cooperative switch runs INLINE under RSIL (masked), unlike
        the 4 other arches that defer the register save/restore to an unmasked handler.
        The one structural outlier; **high risk** (touches windowed-switch atomicity).
  - **Measurement gap (do first):** the current bench measures throughput + *best-case*
    IRQ entry (reporter injects while uncontended), NOT masked-span delay -- so R1/R2/R6
    are not demonstrable with it. Need a worst-case-ISR-latency probe (inject while a
    masked syscall span is in flight) to justify + validate these before landing R1/R6.
  - Note: the earlier **bench self-report starvation is already FIXED** by the
    reporter-as-root/woken-by-workload redesign (not a timer sleep).
- **Console device handover (driver era)** -- userspace UART/console driver takes the
  peripheral as a capability; kernel relinquishes it (`console_tx_deinit`), panic path moves
  to a kernel-retained transport. See `docs/reference/console.md` "Future".
- **[M4.x] Per-thread libc state via real TLS (local-exec).** No per-thread userspace storage
  exists today (newlib `--disable-threads`, threads share one flat image, only the kernel TCB is
  per-thread) -- so `errno` is a shared global, libc `malloc` is not thread-safe (`__malloc_lock`
  is a no-op stub; tracked as its own item in the M4.5.1 kernel-audit section above), and
  `thread_local`/`__thread` silently break. "Fully usable" needs these.

  **CORRECTED BY M5.2.1 PR 8, WHICH MEASURED IT: THESE ARE TWO MECHANISMS AND NOT ONE.** This item
  used to say real TLS fixes `errno` and that a `_REENT`-swap would leave `thread_local` broken.
  Both halves are wrong. `_REENT_THREAD_LOCAL` is off on all three pinned toolchains, `errno` is
  `(*__errno())`, `__errno` returns `_impure_ptr`, and 239 members of libc.a reference that pointer
  without ever calling `__errno()`. So TLS does not touch `errno`, and swapping `_impure_ptr` per
  thread fixes `errno` plus every other reentrant path at once.

  **THE `_impure_ptr` SWAP LANDED IN M5.2.1 PR 9**, on every board but the sim and per-board
  opt-in because it costs one `struct _reent` per thread SLOT out of the app window's heap pad: 512
  bytes on arm-none-eabi, 288 on riscv32-none-elf, 284 on rx-elf. The array is in `.appbss`
  (`user/src/newlib_reent.cc`), `struct Thread` carries the pointer, and `switch_book` plus
  `sched::start` store it into the one word libc reads (`&_impure_ptr`, or what `__getreent`
  returns on Xtensa). It is NAMING AND NOT ISOLATION: the window is granted R/W to every
  unprivileged thread, so a peer can still scribble another thread's `errno`. On by default in the
  `qemu` and `qemu-riscv` base variants, where `errnoprobe` boots it.

  WHAT THIS DOES NOT FIX, and neither does TLS. `_REENT_INIT_PTR` points every thread's
  `_stdin`/`_stdout`/`_stderr` at the ONE shared `__sf[3]`, so stdio buffering stays process-wide
  even where `errno` no longer is.

  **THE MASKED SPAWN WINDOW IS BOUNDED, AND SPLITTING THE LOCK NEEDS THE POOL CHANGED FIRST.**
  The audit calls the interrupt-masked TLS/reent initialisation in `spawn_masked` unbounded.
  It is not, and one of the two bounds landed in this milestone: the TLS copy is at most
  `KICKOS_TLS_STRIDE` minus the ABI bias, which `KICKOS_TLS_FIT_ASSERT` now refuses at LINK
  time; `kickos_reent_init` writes one `struct _reent`, 512 B on arm-none-eabi, 288 on
  riscv32, 284 on rx; the cap loop is bounded by `KICKOS_MAX_SPAWN_GRANTS`. So the window is
  bounded by two link/config constants and nothing an app grows at runtime. The LATENCY
  concern is still real: a board with a full stride of `thread_local` masks on the order of
  2.5 KB of stores.

  **THE REENT TERM IS GONE AS OF M6.2 T5b.2, and NOT by either design priced below.** The kernel
  now primes a slot at its own FIRST SWITCH-IN, so the write happens under the switch's lock and
  never under a spawn's, for every thread rather than for a reused slot alone (the boot seeding
  loop and the pool's `reent_stale[]` went with it). It was moved because a per-process space
  makes a spawn-time write land in the SPAWNER's frame, not to buy latency. What is left in the
  masked spawn window is the TLS copy and the cap loop, so the numbers this section asks for are
  still the ones to take before touching the lock.

  THE OBSTACLE IS NOT THE LOCK, IT IS `ThreadPool::release`. It undoes a claim in exactly two
  cases: an `EXITED` slot, or `i == next - 1`, and its own comment records why that is safe --
  a fresh bump slot is "always the last one UNDER THE SPAWN LOCK". A two-phase publish breaks
  that invariant by construction: a second spawner can claim a slot while the first is
  initialising unmasked, so the first slot is no longer last and `release` either leaves a
  permanent hole (`alloc` only ever revisits EXITED) or un-bumps somebody else's. So the
  change is: give the pool a real free discipline for a claimed-but-unpublished slot, THEN
  split the window, THEN add the reclaim so a spawner slain mid-construction frees the child
  it was building. That third part is what the current single lock buys and must not be lost.

  THE OTHER DESIGN AVOIDS THE POOL AND HITS THE MPU INSTEAD. Have the CHILD seat its own TLS
  and reent on first run: both write the child's own memory, so nothing needs to be masked and
  the spawn lock keeps its transaction. The kernel would pass a trampoline as `entry` and
  carry the real entry and arg in the TCB, which needs no arch change (armv7m already enters
  `entry` directly with lr = kickos_thread_return; only lx6 has a trampoline of its own). It
  does not work as stated: that trampoline runs UNPRIVILEGED for a user thread, so it cannot
  read `user_entry` back out of the TCB. Carrying the pair in the child's own stack above the
  TLS block would work and is the version to price next.

  RECOMMENDATION: neither change should be made without the numbers, because the premise the
  audit gives for making them is wrong. Measure the masked window on a board with a real
  `thread_local` template before touching the spawn path; if it is the few hundred bytes the
  fleet's templates imply, the risk of either design exceeds what it buys.

  **RX HAS NO CI ARM, AND THE BLOCKER IS THE TOOLCHAIN'S DISTRIBUTION.** Every other backend
  has a job in `.github/workflows/ci.yml` (`qemu-arm`, `qemu-riscv`, `qemu-riscv-mpu`,
  `xtensa` as a build gate, `build-boards` for the ARM silicon boards); rx72m appears in
  none, and `build-boards` is arm-toolchain only. The three RX presets DO run in the local fleet
  sweep, which takes every visible configure preset (`tools/sweep_host_gates.sh`, 56 of them on
  2026-08-29), so the gap is CI and not coverage. Adding the job is one composite
  action on the model of `.github/actions/xtensa-toolchain`, which fetches a pinned public
  release asset -- and that is what RX lacks: the installed compiler is Renesas GNURX
  14.2.0.202511 (`rx-elf-gcc (GCC_Build_e13a947a1) 14.2.0.202511-GNURX`), whose distribution
  is not a stable public tarball URL the way the Arm, Espressif and RISCstar ones are. So this
  needs a decision: a mirror to pin, or RX stays locally-verified only and the README says so.

  **M5.2.1 AUDIT: THE MALFORMED-SP BLOCKER IS STILL OPEN, AND THE OBVIOUS FIX DOES NOT WORK.**
  The four refusal paths (armv7m/armv6m `bad_psp`, `kickos_rx_bad_usp`, the rv32imac guard)
  terminate the system rather than the offending thread. Containment itself is available and
  the reasons the code gives for not using it are stale: `arch_ctx_redirect` rebuilds onto
  `ctx.kernel_sp - KICKOS_KERNEL_STACK_SIZE` at the block TOP, so it needs neither a
  trustworthy frame nor a safe SP, and with no block seated it rebuilds at the top of the
  thread's own stack from TCB bounds. A slay-and-resume was written against armv7m and
  MEASURED to fail: a second refusal follows the first, `PSP=0x200007e0 under stack_lo`,
  right after a containment that itself reported correctly. NOT the guard rejecting a
  kernel-block PSP -- `switch.S` already admits `[kernel_sp - SIZE, kernel_sp)` under
  `#if KICKOS_KERNEL_STACKS`. Resuming the SLAIN thread from the fault path is what invites
  the question at all; resuming a DIFFERENT thread avoids it, the dying thread's registers
  being discarded either way. Two further traps found while writing it:
  `kpanic_enter` masks this core's IRQs and never restores them, so it must not run on a path
  that resumes; and `check_pspguard.sh` greps a banner printed BEFORE the outcome, so a
  contained-but-hung image passes on the 20 s `QEMU_TIMEOUT` -- the witness has to assert the
  system went on and exited, not that the banner appeared.

  **PR 9 DOES NOT CLOSE THIS ITEM.** Four pieces of the runtime-consumer scope it was opened for
  are still open, and each is a separate decision rather than more of the same swap:

  - **malloc locking.** `__malloc_lock`/`__malloc_unlock` are still no-ops, so multi-threaded
    `malloc` corrupts the arena. Tracked as its own item in the M4.5.1 section; the reent swap does
    not touch it, `__malloc_sbrk_base` and the bin array living in libc's own statics.
  - **Heap-break serialisation.** `_sbrk` (`user/src/newlib_sbrk.cc`) moves one process-wide break
    with no lock, so two threads growing the heap at once hand out the same page.
  - **C++ exception state.** The unwinder's per-thread state (`__cxa_eh_globals`) is reached
    through `__cxa_get_globals`, which is neither `_impure_ptr` nor a `thread_local` here, so a
    throw crossing a switch is not covered by either mechanism.
  - **Safe reclaim.** A reused slot is re-initialised at the next spawn rather than run through
    `_reclaim_reent`, so the per-reent mprec/asctime scratch a `strtod`/`ctime` caller allocated is
    never returned to the arena. `_reclaim_reent` cannot simply be called on the death path: it
    CLOSES stdio, and every thread's `_stdin`/`_stdout`/`_stderr` point at the one shared
    `__sf[3]`, so reclaiming one thread's state would tear down the whole image's. A reclaim needs
    a newlib-internal-aware sweep of the scratch lists alone, run from a kernel-to-user call on the
    death path PR 7 reworked.

  `thread_local` itself is DONE (PR 8): a block carved off the low end of each thread's own stack,
  with a per-arch thread pointer. Not `TPIDRURW`: M-profile has no such register, so the kernel
  provides `__aeabi_read_tp` (defined in neither libc.a nor libgcc.a) deriving it from SP.
  RISC-V `tp` and Xtensa `THREADPTR` are written by the kernel at resume; RX has neither and falls
  back to emutls. Local-exec model (fully static / no dlopen -> offsets fixed at link). First sibling
  of this family LANDED (M4.3): the `_write` stdout re-probe -- deleted the process-global sticky
  `g_stdout_probe` (per-invocation classify against the calling thread's own cap 0; no per-thread
  storage needed for it).
- **M7 -- multicore (the AMP-versus-shared-kernel question closes PER CLASS; see the OPEN section of the spike).**
  This heading previously read "AMP first on RP2040, SMP-BKL endgame on RP2350" and attributed
  that verdict to the spike. The spike does not contain it and `roadmap.md` says the opposite
  ("not two AMP instances"), so the three records disagreed. Settle it before writing SMP code.
  The deciding measurement is TAKEN: 53 percent of a call/reply round-trip is inside `IrqLock`
  on `esp32c6-wroom` (floor 43 percent), which Amdahl-bounds a two-core big lock at 1.31x --
  see `docs/design-m5-ipc-fastpath.md` 3.0.4. Design spike:
  `docs/design-m7-smp.md` carries the cross-core IPC invariants, the
  per-chip hardware mechanics, the SMP candidate ranking + staged model and the
  SMP-is-per-chip-capability constraint. Candidate
  ranking by the real gate (inter-core atomic + arch-switch maturity): **RP2350 BEST** (M33
  LDREX/STREX enable fine-grained; also 2x Hazard3 -> prove SMP on ARM and RISC-V of one chip),
  **RP2040 big lock FIRST but not capped there** (armv6m has no exclusives, but FreeRTOS V11 ships
  a dual-core RP2040 port carrying TWO locks over two SIO spinlocks and no atomic RMW at all, so
  spinlock COUNT and hold time are the real bounds, not exclusives; what IS unreachable there is
  any lock algorithm needing atomic exchange, so no CLH-style FIFO fairness),
  **ESP32 LX6 last** (S32C1I CAS exists but windowed ABI is hardest; unblocked now
  that the fresh-thread-start bug is fixed at 700ec98, still gated on the model proven on M-profile
  first). Staged: (1) big-kernel-lock SMP first (correct on every dual-core, single-core build
  byte-identical), (2) fine-grained only where exclusives exist (RP2350), (3) LX6 after. The spike REVISED the earlier
  "SMP-only, NOT AMP" call below: ARMv6-M (M0+) has no atomics (no LDREX/STREX; the SIO bus is
  non-atomic too), so RP2040 SMP is capped at coarse Big-Kernel-Lock forever -- AMP (two
  core-private kernels + IPC) is the better FIRST step there, and fine-grained lock-free SMP is
  reachable only on RP2350 (M33 exclusives / Hazard3 A-ext). AMP + IPC and the invariant
  refactors are the near-term items; the SMP-BKL plan (one kernel image across cores) stays the
  endgame. Motivation: run the
  dual-core RP2040 (picopi) at 100% under a single KickOS. Biggest architectural axis on the
  roadmap -- it reworks the *foundation*, not a feature: the whole kernel's mutual exclusion is
  `IrqLock == arch_irq_save` ("interrupts off => exclusive"), which is a single-core-only
  guarantee (masking IRQs on one core does nothing to another). Plan:
  - **Step 1 -- Big Kernel Lock.** Redefine `IrqLock` as "disable *local* interrupts + take one
    global spinlock." Centralised, so it's a redefinition of one class, not a 200-site audit;
    every existing critical section keeps working, kernel is SMP-*correct* (coarsely). This
    line used to say "for a 2-core MCU this likely already gives ~2x"; it is MEASURED at
    **1.31x** (`docs/design-m5-ipc-fastpath.md` 3.0.4, 53 percent of a round trip inside the
    lock), and that is before any contention. Per-core run-queues + finer locks are therefore
    not a later *optimisation* but where most of the payoff actually is.
  - **RP2040 specifics:** M0+ has **no atomics** (no LDREX/STREX) -> use the **SIO hardware
    spinlocks** (32 in the SIO block) for the lock; launch core 1 via bootrom/SIO-FIFO
    (`chip_rp2040.cc` already notes the core-1 milestone + the single-core `TIMELR/TIMEHR`
    latch); per-core SysTick + per-core tickless state.
  - **Already seam-ready:** the `KICKOS_*_BARRIER` publish seams (console_tx / rtt) are the
    fence-injection points -- flip to real fences on the SMP build. Keep centralising `IrqLock`,
    structs-over-globals, no ad-hoc masking -> keeps this a redefinition, not a rewrite.
  - Fits the seL4 endgame (seL4 ships a big-lock SMP variant). See `roadmap.md` (M6).
  - **AMP-first on RP2040 (an OPTION, not a spike verdict -- the spike does not contain one).**
    Two core-private `Kernel` instances. The `KICKOS_MULTI_INSTANCE` per-instance seam in
    `instance.h` was described here as "the ~80% substrate", and then as a dead hole that would not
    compile if enabled. **Neither reading holds any more**: the knob is real (`Kconfig`, the root
    `CMakeLists.txt`), the selector is a thread-local in `include/kickos/instance_local.h`,
    `arch/sim/sim.cc` carries the guarded per-instance state, and
    `tests/integration/check_sim_multi_instance.sh` is a registered gate. The two symbols this
    entry named as used-but-undeclared are in no source file in the tree. It is still single-core
    work, and `docs/design-m4.8.2-host-unit-tests.md` still says it is not a prerequisite for a
    Kernel fixture. What IS real substrate is that all shared state already sits in one
    `struct Kernel` behind one accessor, `kernel()`.
    Re-key on SIO CPUID instead of host-TLS. Each core keeps its own run queue + `IrqLock`==PRIMASK, so NO mutual-exclusion
    refactor: AMP de-risks the shared mechanics (core-1 launch, IPC, console arbitration) that
    SMP also needs, and sidesteps the no-atomics problem entirely.
  - **Cross-core IPC -- required for AMP; none exists today** (`Semaphore`/`Mutex` are intra-core
    only). Design in `docs/design-m7-smp.md`: a per-direction SPSC ring in a shared-SRAM
    window (one writer per index + `DMB` ordering -> no lock, no atomics needed on M0+) with the
    SIO 8x32 FIFO used only as a doorbell (write a tag, raise `SIO_IRQ_PROCn`). API = a `Channel`
    (ring + a `Semaphore` in the receiver's kernel) exposed as `KOS_SYS_chan_{open,send,recv}`;
    blocking `recv` parks on the local run queue via `sem_wait`, the peer's SIO ISR drains + wakes
    via the already-ISR-safe `sem_post`. New arch surface is small: `arch_cpu_id`, `arch_dmb`, and
    an `arch_ipc_notify`/`arch_ipc_drain` doorbell pair (so RP2350 SIO-v2 doorbells back the same
    API). The one genuinely-new isolation decision: a fixed `.shared_ipc` region (pow2 for PMSA)
    granted R|W in BOTH cores' MPU sets -- the ONLY cross-core-writable memory; everything else
    stays per-core-private, preserving the per-core-MPU isolation the AMP verdict rests on.
  - **Three single-core invariants to refactor (either path)** -- `IrqLock`==PRIMASK (local-only
    masking; -> BKL or per-core), the single global current-thread/run-queue (per-CPU), and the
    unsynchronised console + boot-on-one-core + single `arch_mpu_apply`. The arch globals
    `g_arch_current`/`g_arch_next` (+ rv32imac `g_isr_depth`/`g_clint_msip`) are the shared
    prerequisite that gates even AMP.

## Pre-M4 perf: caches / flash accelerators (fleet audit 2026-07-22)

Per-chip audit (each vs its RM; see `CONTEXT.local.md` for the local RM set): does the HW have a
software-controllable cache/accelerator, and do we use it? Binary, not "fast enough".

- [x] **RX72M: enable the 8 KB ROM cache** (pre-M4) -- DONE (5ab2575). `rom_cache_enable()` in
      `chip_rx72m.cc`: after clock-up, `ROMCIV`=1 + bounded poll, then `ROMCE.ROMCEN`=1 (not
      PRCR-gated; 16-bit access). Silicon-validated UNDER ENFORCEMENT: selftest 43/43 + soak 389,
      and the enforce bench went 46772 -> 15405 ns/sw (~3.0x, the flash-instruction-fetch win).
      Caveat carried forward: invalidate after any future flash self-program (auto-invalidated at
      reset today). RM sec 64.4.1/64.4.2/64.7.1.
- [x] **Teensy M7: enable the D-cache** (pre-M4) -- DONE. Silicon-validated (selftest 43/43 +
      a ~38 M-switch soak under enforcement, a measurable throughput win) and made the imxrt
      default (`KICKOS_IMXRT_DCACHE ON`, `arch/CMakeLists.txt`); enabled via
      `kickos_armv7m_dcache_enable()` in `chip_imxrt1062.cc` arch_init. Safe today (single-core,
      no DMA); the coherency obligation arrives with M4-era DMA (non-cacheable DMA pool or
      per-buffer clean/invalidate) -- carry this into the M4 driver work.

Fleet re-validation follow-ups (from the 2026-07-22 M3-branch gate; see `docs/archive/M3_raw_meas.md`):
- [x] **WROOM (Xtensa LX6) soak wedge -- FIXED (700ec98).** Was pre-existing (master), Xtensa-only.
      Root cause: `arch_context_init` started fresh threads via a fabricated `retw` into a trampoline
      with NO `entry` instruction (phantom window frame, garbage caller-linkage); a worker running
      entry->run->EXIT with no block walked WindowBase around the 64-AR/16-slot file until it collided
      with the phantom frame -> spill garbage -> branch into stack -> silent halt (boundary ~4 = file
      size / per-thread window use). Fix: start fresh threads via the `rfe` path with a real `entry`
      prologue (FreeRTOS/NuttX-canonical); COOP block/resume untouched. Fable-reviewed SOUND;
      HW-validated on WROOM (soak 25/25, selftest 41/41 regression, bench baseline). This also
      unblocks the ESP32 LX6 SMP path.
- [x] **RP2350 (Cortex-M33) ARMv8-M PMSAv8 MPU backend** -- DONE (e2179da). {base,size,attr} seam ->
      RBAR/RLAR base+limit + MAIR indirection; strong `kickos_arch_mpu_commit` override on the shared
      deferred-commit seam (K64F precedent); compile-time-gated so the v7-M/v6-M fleet is byte-identical.
      Fable-reviewed SOUND; silicon-validated on RP2350: selftest 43/43 under enforce, `mpu_fault` clean
      cross-domain MemManage denial, bench + soak 411+ no fault. RP2350 now enforces. (Advisories A-D
      below are the non-blocking follow-ups.)
- [ ] **[post-M4] Port the Thread-Metric benchmark suite to KickOS** -- so we can compare honestly
      against FreeRTOS / Zephyr / ThreadX / PX5 (all run Thread-Metric). Run all contenders on ONE
      board at ONE fixed clock, MPU-on-both-sides where applicable, reporting core/clock/MPU/flags +
      the exact "what is a switch" definition. Published raw-switch figures put KickOS's bracketed
      switch (~66-83 cyc M4/M7) in the ChibiOS band -- but every public number is no-MPU/monolithic,
      so only a like-for-like suite run is defensible. (Zephyr's ~468-524 cyc coop figure looks
      inflated by default-config/methodology, not the kernel -- the suite run would settle it.)
- [ ] **RP2350 v8-M backend advisories A-D (fable review, non-blocking hardening).** From the
      PMSAv8 backend review; none block first enforcement, all are build-robustness / fail-closed
      drift.
      (A) **Fail-closed on non-32-exact regions** in `arch_arm_pmsav8.cc` commit -- mirror rxv3's
          per-region `arch_mpu_region_encodable` check and SKIP (not round) an unencodable region,
          since `__kickos_appdata_start` abuts kernel `_ebss`.
      (B) **Alignment ASSERT** `ASSERT((__kickos_appdata_start & 31) == 0)` in `rp2350.ld` (and add
          the same to `mk64f.ld` -- same latent edge).
      (C) **`DREGION >= kMaxPendRegions` boot check** in `kickos_arm_pmsav8_init` (read
          `MPU_TYPE.DREGION`, do not hard-code 8; fail loud if the budget does not fit).
      (D) **Comment nit** `arch_arm_pmsav8.cc:45-46` / `regs_v8m.h:36-37` -- the PRIVDEFENA-background
          note overstates: a MATCHED region's AP also bounds privileged access.
- [ ] **Skip-if-unchanged MPU-commit optimization (post-M3, fleet-wide perf).** The per-switch
      `kickos_arch_mpu_commit` reprograms the MPU + issues DSB;ISB UNCONDITIONALLY every switch
      (measured ~2.3x throughput cost on RP2350 enforce vs mpu-off). Skip the reprogram + barriers
      when the next thread's region set is unchanged (same-domain switch / region-set generation
      compare). Helps EVERY enforce board. Note the SMP caveat already flagged in
      `docs/design-rp2350-mpu-armv8m.md`: any such cache must be per-core (or omitted) under M6, not
      a shared static.
- [ ] **ESP32-C6 enforce-bench ns-scaling** (measurement-only, not M3). `cyc` counts correct; ns
      ~8x high because `rdcycle` traps on the C6 so the bench samples an MMIO counter whose rate
      differs from `SystemCoreClock`. Also RP2350 bench `irq` reads a bogus 1 cyc (irq-probe not
      wired for the M33). Per-chip bench-instrumentation cleanup, not a kernel bug.
- **No gap (already accelerated), for the record:** STM32F411 ART (ICEN|DCEN|PRFTEN + 2WS,
  `chip_stm32f411.cc:171`); STM32F103/F302 prefetch buffer (M3/F3 have no I/D cache in HW);
  K64F FMC cache+speculation on by reset default (`PFB*CR=0x3004001F`); XMC4800 PMU buffers
  default-on + WS set (`chip_xmc4800.cc:373`); RP2040 XIP cache on by bootrom; ESP32-C6 cache
  fronts external flash only -> irrelevant to KickOS's HP-SRAM execution.
- **RP2350 (deferred M4): XIP cache on by reset + bootrom-invalidated -> NO enable needed** (unlike
  the M7). No Device anti-speculation wrap either -- the M33 isn't speculative and the QMI
  bus-ERRORS (not stalls) on unbacked reads. For the PMSAv8 backend, carry: (1) bound the RX
  region to actual code extent (RLAR arbitrary limit, no pow2 pad) -- the M7 "bounded code"
  lesson; (2) set `XIP_CTRL.NO_UNCACHED_*`/`NO_UNTRANSLATED_*` so mirror-window aliases
  bus-error (saves MPU/SAU regions); (3) MAIR NORMAL-WBWA on the flash region so the cache
  serves hits under enforcement; (4) invalidate-by-address after any future flash program.
  Fold into `docs/design-rp2350-mpu-armv8m.md`.
- Common caveat for ALL the flash caches/buffers: they are NOT coherent across a flash
  program/erase -- any future in-field flash-write/OTA path must invalidate the relevant
  cache/speculation buffer. Not a live risk (KickOS is a fixed flash image today).

## M4.5.x -- foundational tightening

M4.5.x tightens the foundation BEFORE more complexity lands on it. The driver era adds gates,
drivers and controller backends; every one built on a layer that is about to be rewritten is paid
for twice. Less is more: each pass below should end with fewer lines than it started.

## M4.5.9 -- comments and the design tier

Touches nearly every file, so it runs after M4.5.8 merges.

- [ ] **Comment purge.** Keep the fact, cut the chronicle. `--` is a DETECTOR: a comment needing a
      clause chain is already phrased wrong, so rewrite or delete it. Repunctuating to `;` keeps the
      bad sentence and hides the signal.
      **There is no `--` count gate, and there will not be one** (decided 2026-07-31). A zero gate
      is not reachable: a large share of surviving `--` comments are load-bearing, so the only way
      to drive the count down is the repunctuation that destroys the detector. A counting gate would
      then read the tree as clean while every bad sentence survived. The rule stays sweep-on-touch
      plus no new clause chains.
      **Open, and wanted: one ubiquitous mechanism that enforces house style across code, docs AND
      build files.** `clang-format` was already decided against as a gate, and `uncrustify` is not a
      fit either; neither reaches markdown or CMake, and a formatter cannot see the rules that
      actually drift (braced `case` bodies, spelled operators, ASCII, narration). Needs a design
      pass, not another per-rule script.
      Narration is not explanation: the reason a thing is so is one line and stays, the story of
      reaching it is history and git holds it.
      **A comment that turns out to be the only protection for something is a MISSING GATE.** Write
      the test. `virt.ld` is the model: the `qemu-riscv` gate stops the esp32 assert being copied
      there, not the comment saying so.
- [ ] **Categorize the design tier.** 34 docs, 13,163 lines, against Book 27/7,613 and Reference
      10/7,484 (re-measured 2026-08-16; the earlier 29/10,797 figure had drifted): the tier authoritative for nothing is the largest, and most of it describes landed
      work. Per doc, teaching goes to the Book, the contract to the Reference, and the remainder is
      a short decision list (decisions, why each alternative fails, any falsifier). Nothing left
      means delete it.
      A design doc is neither Book nor Reference. It records decisions for unsettled work, so it
      does not teach and does not restate the contract.
- [ ] **Gate defects, root-caused rather than rewritten.** The four binary-introspection gates go
      GREEN on a broken tool: a pipeline given unexpected input prints nothing, and an
      absence-assertion reads nothing as clean. Verified instances, each a live defect.
      `check_oot_export_mcu.sh` never sets `LC_ALL=C`, so a translated `Machine:` heading fails it
      FALSELY; it reports a broken `readelf` as "did NOT relink", blaming a missing
      `INTERFACE_LINK_DEPENDS`; its `ls *.ld | head -1` silently picks one of several.
      `check_kernel_ctor_placement.sh` runs six bare `nm` pipelines under `#!/bin/sh`, which has no
      `pipefail`, so every exit status is discarded; its own failure diagnostic at line 138 uses
      gawk-only `strtonum`/`and`/`compl`, so a green run never discovers the diagnostic is broken;
      its three address sets are compared as `printf '%x'` STRINGS with nothing asserting the
      formats agree. `check_seam_defaults.sh` guards `UND` but not `ABS`, where `$(($3 + 0))`
      evaluates to 0 and can match section 0; its `TARGET_OBJECTS` split at line 68 is unquoted, so
      a glob character or a space in a build path inventories a different file as an empty one.
- [ ] **Missing gates the purge surfaced.** The rule this milestone runs on is that a comment which
      is the only protection for something is a MISSING GATE, so the sweep was asked to report them
      rather than delete them. Every one below is a real constraint held by prose alone. None was
      removed; each needs a test, and none is a comment problem.
      *Kernel*: the `KICKOS_MIN_STACK_SIZE` per-arch floor is never checked against the deepest
      `exit_current` chain, so a wrong override overflows only on thread exit, on hardware.
      `wq_confirm_resume` requires the lock released BEFORE `wait_result` is read; inlining the read
      under the lock compiles clean and passes on the sim, whose switch is synchronous, and races on
      ARM. `ThreadPool` stack harvest must happen only once the exited thread is provably off-CPU.
      `domain_for` requires `caller_authorized` resolved by the CALLER, never read from
      `sched::current()` inside. `irq_register`'s clear-then-enable order matters only on ARM and RX,
      which are default-masked; sim and riscv would never catch a reorder. `console_tx`'s
      prime-the-pump applies per chip family and a refactor dropping it hangs TX on real
      edge-triggered hardware only.
      *ARM*: `arch_diag_led_init` depends on `uart0_init` having opened the PORTB gate earlier in
      `arch_init`; reordering silently drops the PCR store. `chip_mps2` losing its
      `kickos_arm_pmsav8_init` reference still links and silently writes RASR-shaped values into
      RLAR. `sam3x8e`'s `tc_clock_init` ordering BusFaults a static ctor calling `ktime_now`, and its
      `MOSCXTST` window is a guess pending Due silicon. `imxrt1062`'s MPU-before-cache order
      manifests only as a silicon hang, and that chip has no board or QEMU model. `MODEM_TXCTSE`
      without real CTS wiring makes the polled writer wait forever. `switch.S`'s FP-frame save order
      is covered by `static_assert` for the scalar offsets only, not for `{s16-s31}` before
      `{r4-r11,lr}`.
      *RISC-V and Xtensa*: the C6 all-ones-NAPOT quirk, why the bootstrap entry uses TOR, is not
      reproducible under QEMU. `arch_pinmux_set` writes the GPIO-matrix out-sel before the
      kernel-owned-pin check, so dropping the matrix stage bypasses console and LED pin protection.
      APM denial does not trap the way PMP does, so a bad region hangs instead of faulting.
      *RX*: enabling SCI6 `SCR.TIE` while `SSR.TDRE` is set is EXPECTED to raise TXI6 and has never
      been confirmed on silicon; if wrong, the console TX drain never re-arms after idle. An earlier
      `CMWSTR.STR` readback guard raced at full switch speed and starved the far deadline whenever
      the CPU never idled; nothing stops it being reintroduced.
- [ ] **A registered arm that proves the gates still fail.** Point each gate's tool at `/bin/true`,
      then at a stub emitting a translated heading, then at a truncated capture. Each must exit
      non-zero with a TOOL error, not with the assertion's own diagnostic. Exit codes are read
      unpiped, because `grep -c` exits 1 on zero matches and kills an `&&` chain before its
      diagnostic prints. Without this arm the hand-placed landmark stays a habit of whoever
      remembers, which is what let M4.5.8's eight stacked regressions pass.
      A full Python rewrite of these four was proposed and DECLINED: it moved 762 lines to 585
      while churning `check_riscv_no_smalldata.sh`, already the soundest of the four, and its
      "before M4.6" sequencing rested on M4.6 adding introspection gates, where M4.6.1 and M4.6.2
      add boot and TAP arms riding `tests/lib/gate.sh`, out of that proposal's scope either way.

## Found taking the M4.7.8 payload measurement (2026-08-06)

- [ ] **The call/reply sweep is PRECISE but not ACCURATE, so it cannot accept or reject a change
      of a few percent.** Measured on `xmc4800-relax` silicon, enforcing, one bench variant, 8 B
      round trip. `master` `de2801d` sits at 37997-38152 ns across five builds padded with 0, 4, 20,
      68 and 260 bytes of `.text`, so **layout moves it by 155 ns, 0.4 percent** and the instrument
      is stable per image (the same binary re-flashed is byte-identical). Against that band the
      milestone's own points are 38290, **36047**, 40070 and 39814 at the tip: a spread of 3767 ns
      that layout cannot explain. **The middle point is 2000 ns FASTER than `master` while strictly
      adding code to the path**, which no amount of added work produces, so the number is not a
      per-round-trip cost. Two candidate causes were tested and REFUTED: code layout (the padding
      sweep above) and the deadline cancel that now runs on every wake (removing it made the tip
      slightly WORSE, 40064).
      **The surviving explanation is that the sweep does not measure one thing.** A round trip takes
      the endpoint FASTPATH when a receiver is already parked and the SLOWPATH when the caller parks
      first, and those cost very differently; a small scheduling shift changes the MIX rather than
      the per-path cost, which fits stability per binary, insensitivity to padding, and swings that
      do not track work added. **Fix before trusting it: have the sweep report its fastpath and
      slowpath counts**, so a reading is interpretable instead of an average over an unknown mix.
      Until then the tip's +4.6 percent against `master` is UNEXPLAINED, not established, and the
      argument that the untimed path did not grow is a code reading: two stores at a park, two at an
      unpark, and one comparison against `KOS_TIMEOUT_NONE`.
- [ ] **An `endpoint_call` / `endpoint_recv` kernel signature crossing FOUR arguments was tried and
      REVERTED, because it bought nothing measurable.** Both gained a fifth parameter in M4.7.8 and
      an ARM AAPCS fifth word is passed on the stack, which is visible in the prologues
      (`ldr.w r9, [sp, #64]`) and in a caller-side `str r2, [sp, #104]`. Holding them at four (the
      call taking the lengths already packed by its timed stub, the recv carrying a flag beside a
      `cap_len` that needs only nine bits) removed exactly that traffic and moved the sweep from
      40070 to 40313, i.e. not at all. It also put a bit inside a user-supplied word that userspace
      must never set, which the untimed arm then has to mask defensively. Recorded so the idea is
      not re-derived as an obvious win; revisit only with an instrument that can see it.

## Found taking the M4.7.7 payload measurement (2026-08-06)

- [ ] **`esp32-wroom` reports impossible elapsed times, so its clock read is not trustworthy.**
      The `bench` call/reply sweep at tree `bcb94ff` returned 9 ns and then 0 ns per round trip
      over 20000 calls at the 16 B and 128 B steps, with 1853 ns at 8 B, while its 32/64/256 B
      steps are self-consistent at 75.0 ns per byte and agree with `xmc4800-relax` and
      `frdmk64f` on 9 cycles per byte. So the IPC path is sound on this board and the TIMG0
      read behind `arch_clock_now` (`arch/xtensa/chip/esp32/chip_esp32.cc`) is what returns a
      stale or non-monotonic value: 0 ns across 20000 syscalls is not a slow clock, it is the
      same value twice. Nothing gates it, because no in-env suite reads the clock twice around
      a known interval and asserts the delta grew. A driver-era item: the same read backs every
      timeout a driver takes. Log `.session/logs/m477-esp32-wroom-bench.log`.
- [ ] **Every `kickos_bench_*` helper is a kernel function called directly, so the bench app
      cannot measure anything on a board with an MPU or a PMP.** They are plain calls, not
      syscalls, so they run at the caller's privilege, and each reads kernel `.data`
      (`SystemCoreClock`, the switch accumulators) or a peripheral (`arch_clock_now`). Root has
      not been privileged since `m4.5.1` (`0171b75`), so the first call faults: witnessed as
      `ccu4_ticks` refused at `MMFAR=0x4000c470` on `xmc4800-relax` and `kickos_bench_core_hz`
      refused at `0x1fff0038` on `frdmk64f`, both AFTER the payload sweep, which is why the
      sweep is unaffected. On a board with no unit the reads succeed instead, so the throughput
      and cycle metrics are reachable only there. The `masked_hold` model is already deleted
      rather than repaired, its answer being available from the sweep's slope. What is left is
      a choice for whoever wants the cycle metrics back on an enforcing board: measure inside
      the kernel at boot, or expose the counters through the syscall ABI.

## Found in the M4.7.7 ten-angle review (2026-08-06)

- [ ] **The `bench` app has no execution gate on any board and compile coverage on exactly one
      arch.** `boards/qemu-riscv/configs/bench/defconfig` is the ONLY provisioning in the fleet
      that sets `CONFIG_KICKOS_BENCH=y`, and `user/apps/common/CMakeLists.txt:141` builds the app
      only under that flag, so `qemu-riscv-bench` is the one preset that compiles it. CI then runs
      `ctest -R seam_defaults` against that build dir (`.github/workflows/ci.yml:201`) and nothing
      anywhere registers an `add_test` for the binary, so the app is never EXECUTED by any gate on
      any board. A compile break on armv7m, armv6m, xtensa or RX therefore reaches nobody, and a
      break in the app's own sequencing (its call/reply sweep spawns and joins two peers per step
      against a pool that is 3 slots wide on three boards) reaches nobody at all. Cheapest first
      cut: one more `bench` variant on a board with a machine model plus an `add_test` matching
      the sweep's own output lines, since a bench that prints no measurement is the failure shape.
- [ ] **`cmake/cap_table.cmake` hand-mirrors `KICKOS_THREAD_SLOTS`' `+1` with nothing
      cross-checking it against the C macro.** `kickos_cap_table_resolve` computes
      `math(EXPR _pool "${_threads} + 1")` and comments that it MIRRORS
      `kernel/include/kickos/config/system.h`; if the C side ever changes shape the two silently
      disagree. Bounded today: `_pool` reaches only the configure-time `message(STATUS)` text and
      the `_kickos_cap_slab` arithmetic behind it, never `cap_width.h`, so a divergence yields a
      wrong RAM diagnostic and not a wrong binary. The tree already has the fix shape: the
      structural cap constants (`KCAP_RUN_OFF_POOL`, `KCAP_CHUNK_TARGET`,
      `KICKOS_CAP_FIRST_DYNAMIC`) are declared in `cmake/cap_geometry.cmake` and forwarded to C
      through the header generated from `kernel/include/kickos/config/cap_width.h.in`, so one side
      owns each value. Forwarding the slot count the same way leaves `system.h` deriving the macro
      from the generated header instead of restating `+ 1`.

## M4.8.1 -- every driver gets a class, per the driver-model ruling

The driver era resumes here, and this is the first thing it fixes. `docs/design-m4-driver-model.md`
states the ruling: **"The class is the primitive; the service is a thin thread composed on top of
it. Never the reverse"**, and "A consumer that cannot afford an IPC round-trip links the class and
calls it". In practice the ruling is inverted for **every driver in the tree**, so this is one
fleet-wide conversion and not a per-peripheral fix.

The intended shape, which nothing currently implements: a **struct plus free functions**, a C-like
object holding its own instance state, which the service THREAD instantiates. A consumer that is the
sole user of a bus links the object and calls it, paying no dispatch; the service exists for the
shared case only.

- [x] **Inventory: 10 drivers, 0 exposed a driver object.** SUPERSEDED by the M4.8.1 conversion:
      the classes are `user/include/kickos/driver/`, and one generic `bring_up` over a per-chip POD
      descriptor replaced the twelve per-(class x chip) services. Record:
      `docs/design-generic-driver-service.md`.
- [ ] **The class leaves are not the class.** `arch/*/chip/*/class/` is real but holds
      register-logic fragments: `dspi_class.h` exposes one function, `dspi_rx_count(base)`. That
      satisfies the Rule 6 seam's "a real leaf and a real consumer each"; it is not a driver.
      Keep the leaves stateless and freestanding as they are -- the class object is a layer above
      them, not a replacement.
- [x] **Convert every driver, and make the shape the fleet's semantic.** DONE fleet-wide in M4.8.1,
      UART, SPI and USB alike (`docs/design-generic-driver-service.md`). The standing part: a new
      driver arrives class-first from then on, and a service that invents API the class does not have
      is the defect to watch for.
- [ ] **The class API must be COMMON per peripheral kind, not per chip.** A client wraps it once and
      stops thinking about the hardware -- that abstraction is the whole point of a kernel plus a
      userspace service layer, and `roadmap.md` already sets it as M4's objective: prove the
      console/UART, gpio, pinmux, clock/power and bus APIs are "genuinely vendor-neutral, not
      accidentally shaped around one vendor". So `spi_transfer(obj, ...)` reads the same against
      XMC USIC and K64F DSPI, and only construction names the chip.
      **Done for SPI, and it was the ruling inverted exactly.** The neutral API used to be
      reachable only over IPC: the old client wrapper was chip-agnostic and took an endpoint
      capability, while the per-chip headers beside it (`xmcssc.h`, `k64dspi.h`) exposed only a
      service start hook, so the only way to get the neutral API was to pay the dispatch. It is now
      `user/include/kickos/driver/spi.h`, the class, with `system/driver/xmc4800/xmcssc/spi_usic.cc`
      and `system/driver/mk64f/k64dspi/spi_dspi.cc` as local engines, `user/lib/spi_proxy/` as the
      proxy over the wire, and `user/include/kickos/sys/spi_service.h` reduced to a transport that
      calls the same class a local consumer links. Still open for the other driver types below.
- [ ] **The unit of commonality is the DRIVER TYPE, and the taxonomy is layered.** One API per
      peripheral kind, not one universal API and not one per chip:
      **SPI**, **I2C**, **UART**, **USB host**, then **per USB device class** (CDC-ACM first, the
      others as they arrive) layered on the host controller rather than beside it, and equally
      **timer**, **PWM** (open: its own type, or a timer with a capture/compare mode), **one-wire**
      and **GPIO**. That list is examples, not an enumeration: **the rule is general to every driver
      type the fleet grows.** A backend that cannot express its hardware in its type's API is the
      signal that the API is shaped around one vendor -- which is the discovery M4 exists to make,
      so treat a genuine misfit as a finding rather than forcing it.
- [ ] **Drivers STACK, and a high-level class must not care whether its bus is local or a service.**
      An accelerometer on SPI is its own class, in its own type; what it needs is a BUS, and that bus
      may be either a local SPI class instance (in-process, no dispatch) or the SPI service over an
      endpoint. So a stacked class is written against the bus type's API and never against a
      concrete backing.
      **This is what the 1:1-serialization requirement actually buys**, and the reason to keep it
      strict: the remote implementation is a PROXY in the ordinary RPC sense, with the identical
      signature to the local object, so substituting one for the other is a build choice and nothing
      above it changes. Let the two drift and every stacked driver has to know which it is talking to.
      The symmetry is also the drift test: the proxy marshals into `kos_call`, and the service thread
      on the other end unmarshals and calls THE SAME local class a local consumer would have linked.
      A call the proxy has and the class does not, or a service that does something the class cannot,
      means the 1:1 property is already broken.
      **DECIDED: the substitution is compile-time, one API with several implementation `.cc` files
      and CMake selecting one.** No function-pointer indirection, no metaprogramming -- the choice is
      known when the image is built, so it costs nothing at runtime and keeps a direct call. For SPI
      that is three implementations of one header: the per-chip local ones (USIC registers, DSPI
      registers) and ONE proxy whose bodies marshal into `kos_call` on the service endpoint. The
      proxy is per BUS TYPE, not per chip, because it speaks the chip-agnostic wire protocol.
      **Two axes select it, and only the first follows the board.** Which chip is a board fact, like
      everything else keyed on `KICKOS_BOARD`. Local-versus-remote is a SYSTEM COMPOSITION fact:
      alone on the bus means local, sharing it with another consumer means remote, and that differs
      per image with the same chip and the same driver source. So it belongs to the CONSUMER TARGET
      rather than to a global macro -- one image may legitimately have one consumer local and another
      remote -- which is the shape `KICKOS_SERVICE_LIST` and `KICKOS_INIT_PROVIDER` already use.
- [x] **Console is a COOKED UART, not a type, and the tree HAD that inverted.** Fixed by the M4.8.1
      conversion: the raw type is `user/include/kickos/driver/uart.h`, five calls, in-process, and
      console composes on it, so a client wanting bytes rather than a console gets them without a
      service. Record: `docs/design-generic-driver-service.md`.
- [ ] **The class must NOT cook. Cooking is the service's job.** Policy in the primitive is what
      makes a primitive unreusable: a class that expands CRLF forces every consumer wanting raw
      bytes to un-cook or thread a flag, and it stops being the plain device. So the UART class
      moves bytes and nothing else; the console SERVICE owns the line discipline.
      **This leaves two cookers, which is correct and not duplication to collapse.** The kernel
      keeps its own in `kernel/init/console.cc` for the pre-handover console and the panic path,
      because a panic cannot call a service. The rule that makes both right: cooking lives with each
      CONSUMER that needs it, never in the device layer. Collapsing them would either put policy
      back in the class or make the panic path depend on IPC.
- [x] **Done before the endpoints gained more consumers**, which is why M4.8.1 preceded the USB work
      rather than following it: with no class methods the wire protocol IS the API, and extracting a
      class later would mean deriving it from its own transport.

## Found reconciling the reference documents with the tree (2026-08-25)

- [ ] **RX owes a `pspguard` witness, and the app as it stands cannot give it one.**
      `user/apps/common/pspguard/CMakeLists.txt` returns immediately unless `KICKOS_ARCH` is
      `armv7m` or `armv6m`, and `main.cc` moves SP with ARM asm and `#error`s anywhere else, so the
      PSP-bounds class is machine-checked on two backends of the four that carry it. On rxv3 the
      refusal sites are `.Lsys_bad_usp`, `.Lpendsw_bad_usp` and `kickos_rx_bad_usp`
      (`arch/rx/rxv3/switch.S`); the fourth, `.Lsvc_nokstack`, is structurally unreachable there --
      `thread_create` seats a kernel block on every pool thread and idle is privileged and runs
      `arch_idle_wait` alone, as the site says itself -- so nothing in the tree reaches the REFUSE
      side of the trusted-stack guard on RX. An RX prober needs its own SP-moving arm, and rx72m has
      no emulator, so the witness is a bench step whichever way the app is written.
      **AND THE SP-MOVING ARM ALREADY EXISTS, so what is owed is a bench step and not an app.**
      `user/apps/common/faultsurvive/CMakeLists.txt` builds `faultsurvive_kwrite` and
      `faultsurvive_misalign` on rxv3 as well as rv32imac, and both enter `int #1` with a USP the
      thread chose, which is the entry `.Lsys_bad_usp` guards. What is missing is a run: the
      `add_test` calls for those two are guarded on `rv32imac`, and the board list the file
      registers against holds no RX preset. The arms an RX image can be flashed with today land
      elsewhere -- `faultsurvive_ovf` overflows the stack and the MPU denies the store, and
      `faultsurvive_off` traps through `mvtipl`, a privileged instruction, so it takes the fault
      path where `kickos_fault_frame_trusted` does the checking. So `kickos_rx_bad_usp` is compiled
      into every RX image carrying fault isolation and reached by nothing that runs.
- [ ] **`usbcdcwit` is built by no default configuration of any board.**
      `user/apps/common/usbcdcwit/CMakeLists.txt` returns unless `KICKOS_SERVICE_LIST` matches
      `_usbcdc$`, and no board defconfig or `Kconfig` sets such a list. The three that exist --
      `kickos_services_picopi_usbcdc`, `kickos_services_pizero2350_usbcdc` and
      `kickos_services_teensy41_usbcdc` (`system/CMakeLists.txt`) -- are reachable only from a
      `-DKICKOS_SERVICE_LIST=` on the configure line, so the app compiles in no preset, no local
      fleet sweep and no CI job, and a change that breaks it is invisible until somebody remembers
      the flag.

## Two `-st` presets stopped linking at M6.3's R2.2, FIXED (2026-08-29)

Root-caused at the MECHANISM and not at the two images, the scope ruling that set them aside having
rested on a premise (a pre-existing `master` overflow) that measurement removed.
`docs/reference/boards.md` carries the board-facing version under *The two `-st` presets and the
per-app build stamp*.

- [x] **What R2.2 cost the whole fleet, measured rather than inferred.** Isolated by reverting ONLY
      the stamp mechanism in a tree at the branch HEAD (`user/include/kickos/app.h`,
      `user/src/root_entry.cc` and the `kmain` half that reads them) and rebuilding, which removes
      every confound the other 33 commits carry: **+104 B `.text` and +24 B `.data`, against
      -32 B `.bss`, on EVERY ONE of the 33 apps of `bluepill-c8-st`**, so +128 B of flash and
      roughly nothing of RAM per image on every board in the fleet. Not the 230-byte `.data` growth
      the earlier record implies: `.data` moved by 24 bytes, and the flash was `.text`.
      The named cost is `kickos_app_build_raw`, 27 B of WRITABLE `.data` charged to the image
      twice (once as the flashed initialiser, once as the RAM the reset path copies it into), plus
      `kickos::app_build_stamp` and its `kMonths` table inlined into `kickos::kmain`, which grew
      708 -> 952 B. What it replaced was one weak `kickos_app_build_stamp()` in app text, 140 B
      merged across the app's TUs with a 28 B `.bss` buffer and a 37 B month table.
      **The whole app-stamp FEATURE costs 336 B of flash and 28 B of RAM per image at the branch
      HEAD**, measured against a tree with the stamp removed outright.
- [x] **The fix is the mechanism, and every board is lighter than it was at `master`.** The app's
      half stays DATA, which is the property R2.2 was taken for and which
      `tests/static/check_riscv_kernel_apphalf.sh` enforces; what goes is the runtime reformat.
      `kickos_app_build_raw` becomes `const char kickos_app_build_time[]`, so it lands in
      `.rodata` and is read where it was flashed, and the banner prints C's own
      `"Mmm dd yyyy HH:MM:SS +zzzz"` spelling instead of converting it to `yyyy-mm-dd` at every
      boot on every board. Recovered: **288 to 292 B of flash and 24 B of `.data` on every image
      on every board**. Against `master` the same images are 64 to 72 B larger in `.text` and
      32 B SMALLER in `.bss`, that residual being M6.3's `arch_aspace *` parameter threading
      through `kaccess_from_user`, `kaccess_to_user`, `endpoint_send/recv/call/reply`,
      `cap_out_deliver`, `console_write_user`, `user_panic` and `reent_seat`/`reent_prime`, which
      a board with no translating backend pays for a pointer that is always null there.
- [x] **Both presets link.** `selftest` part 1, the largest of the three images, ends at
      **65,484 B of 65,536 on `bluepill-c8-st`, 52 free**, and **65,500 of 65,536 on
      `f302nucleo-st`, 36 free**. `master` stood at 128 and 112 free, so the two boards have
      always been within a few dozen bytes of the ceiling and neither figure is generous.
      **TOOLCHAIN-DEPENDENT**: taken with `arm-gnu-toolchain-15.3.rel1`, where `ci.yml` pins
      `15.2.rel1` and reads a few bytes higher on the same tree. A CI link is the only thing that
      settles whether 36 bytes survives the pinned compiler.
- [x] **The three-way `selftest` split was BADLY BALANCED and is now sized against the image.**
      Part 1 was 65,484 B while `selftest_p2` was 48,496 and `selftest_p3` 48,308, so the two
      smaller parts sat on about 16.7 KiB of free flash each while the first had 52 bytes. The
      boundaries are the two `#undef TAP_ADD` lines in `user/apps/common/selftest/main.cc` and
      nothing had ever sized them; the arm COUNTS do not predict the sizes.
      **MEASURED BEFORE MOVED.** On `bluepill-c8-st` the base image with no arm registered at all
      is 30,108 B, and the 102 arms of that posture cost 71,912 B between them: a per-arm sweep
      (one relink per cut point) puts them between 52 B (`cap_reply_bound_slow`) and 4,140 B
      (`bus_device_slots` + `uart_service`) and 5,236 B (the eleven `irq_*` arms, one `#if` block
      and so one indivisible unit). The ideal third is 30,108 + 23,971.
      **Region 1 now ends after `call_timeout_reply` and region 2 after `cap_reply_slot_reuse`**,
      taking the counts from 49 / 22 / 31 to 37 / 26 / 39 in the `-st` posture:

      | image | `bluepill-c8-st` | `f302nucleo-st` |
      | --- | --- | --- |
      | `selftest` | 54,976 B, 10,560 free | 54,992 B, 10,544 free |
      | `selftest_p2` | 55,332 B, 10,204 free | 55,356 B, 10,180 free |
      | `selftest_p3` | 52,012 B, 13,524 free | 52,020 B, 13,516 free |

      The base twins are 54,796 / 49,768 / 46,280 and 54,832 / 49,800 / 46,324, so the thinnest
      part on any of the four is 10,180 free bytes against the 36 and 52 it was.
      **Coverage is unchanged and was proved rather than argued**: the union of the arm NAMES the
      three `microbit` parts report is the same 102 before and after, with no name in two parts,
      and the concatenation is byte-identical, so the arms also still run in the same ORDER.
      `microbit`'s three gates keep their `EXPECT_SKIPS`/`EXPECT_PARTIALS` sets unchanged -- its
      arena base moves with whichever statics a part elides, and part 3's rose 256 B without
      changing an outcome. Moved with the boundaries: the `_tap_arms_p*` clauses and their
      comments in `user/apps/common/selftest/CMakeLists.txt` (`call_reg_fastpath` into part 2,
      `reboot_priv` and `periph_reg_write_mask` into part 3), the per-part records in
      `docs/reference/boards.md`, and the two `selftest_p2`-is-the-fattest-image claims in
      `docs/reference/porting.md`, which named the wrong part even before this: `selftest_p3` has
      the highest `__kickos_ram_start` on all four split presets, and the `bluepill-c8-st` span is
      3,040 B rather than 4,224.
- [x] **Both presets contribute a trap-stack measurement again.** They are declared in
      `tests/static/trap_redzone_roots.txt` and the gate failed on the same link inside its own
      scratch tree for as long as the overflow stood. Re-measured after wiping
      `/var/tmp/kickos-trap-redzone-*`.

## Found taking the M6.3/M6.4 authoritative witness, 2026-08-29

- [ ] **`tools/sweep_image_gates.sh` SKIPS `qemu-x86_64` BY NAME AND ASSERTS NOTHING ABOUT IT,
      ON A BOX WHERE ITS TEN IMAGE GATES PASS.** `emulator_for()` in that script maps the MPS2
      set and `microbit` to `qemu-system-arm`, `qemu-riscv` to `qemu-system-riscv32`,
      `qemu-riscv64` to `qemu-system-riscv64` and `qemu-arm64` to `qemu-system-aarch64`, and has
      no row for `qemu-x86_64`. An empty answer means "boots natively", which the script accepts
      only for board `sim`, so the x86_64 board falls into the clause below it and is recorded
      `SKIP ... board qemu-x86_64 has no emulator machine; 10 image gate(s) need silicon`. It is
      not silicon-only: `qemu-system-x86_64` is on this box and a plain
      `ctest --preset qemu-x86_64` runs those ten gates green.
      **The tool predicted this failure and caught it in the shape it predicted.** Its header says
      the emulator map is spelled out rather than assumed, and that "a drift between that map and
      `cmake/kickos.cmake` surfaces as a skip this tool did not predict". It surfaced exactly so.
      What it costs: the sweep's DONE sentinel can only be written by DECLARING
      `SWEEP_EXPECT_SKIP=1`, so the declaration mechanism launders a runnable board into an
      expected absence -- an instrument reporting clean over a corpus it never read, which is the
      class this milestone met repeatedly.
      **NOT FIXED HERE, deliberately, and it is a decision rather than a deferral.** Adding the row
      is one line, but whether the sweep's serialised runner drives the OVMF path the way the
      board's own ctest ladder does is unmeasured, and a wrong row produces a red sweep that is an
      instrument fault rather than a finding. Editing a sweep script during the run that sweep is
      the witness for would also invalidate it. Decide the row, then re-take the image sweep.

## External audit of the boot-path guard, 2026-08-29 (fourth pass)

Three HIGH findings, all against the machine-mode prologue in
`arch/riscv/chip/virt_rv64/startup.S` that the third pass added. The guard replaced no check at
all, so none of these is a regression; two of them say the guard can PASS on a machine the
supervisor cannot run on, which is the one failure it exists to prevent. **ALL THREE HOLD AND ARE
FIXED, and a fourth defect found while checking the first is fixed with them.** Held against the
RISC-V Privileged specification rather than against the report, and one prescribed remedy is
REFUSED as the weaker of the two options. `qemu-riscv64` and `qemu-riscv64-sv48` are 52 of 52 each
after the pass, `console_reach` green on both, `check_aspace_sigdiff.sh` still exit 2 at 35 against
36, and `trap_redzone` unmoved on the three presets that carry a zero-slack figure (`frdmk64f`
EXITK 576, `rx72m` EXITK 620, `esp32c6-wroom` NESTED 768 and EXITK 720).

- [x] **The effective-PMP probe assumed a translation regime it never established. HOLDS, FIXED.**
      `mstatus.MPRV` with `MPP=S` makes the access TRANSLATED as well as protected as a supervisor
      one (Privileged ISA 3.1.6.4), and RISC-V defines NO reset value for `satp`: section 3.4 lists
      what reset establishes, `satp` is not in it, and everything unlisted is UNSPECIFIED. So the
      probe ran under whatever regime the machine arrived in.
      **MEASURED ON THIS HART**, with a temporary M-mode instrument printing over the `virt` UART:
      `satp` reads `0x0000000000000000` out of reset here and `mstatus` reads
      `0x0000000a00000000`, so on QEMU the probe was accidentally Bare all along. That is a
      property of this emulator and not of the architecture.
      **AND THE FAILURE IS A WRONG REFUSAL, NOT A WRONG PASS**, because PMP is checked on the
      physical address a walk produces and no translation can mask a denial. Handed an inherited
      `satp` of Sv39 over an unwritten table, the probe takes `mcause=0xd`, a load PAGE fault, and
      the port refuses with `KickOS: PMP denies the supervisor accesses this port makes ...` on a
      hart whose PMP is perfect: a boot brick with the wrong diagnosis.
      Fixed by writing `satp` Bare and reading it back, clearing `mstatus.MPV` and reading it back,
      and executing `sfence.vma zero, zero` before the probe. **THE FENCE IS THE PMP WRITE'S AND NOT
      THE `satp` WRITE'S**: a hart with page-based virtual memory may cache PMP verdicts alongside a
      translation, the identity mappings Bare and machine mode use included, so M-mode must issue
      `SFENCE.VMA` with `rs1 = rs2 = x0` after writing the PMP CSRs (Privileged ISA 3.7.1), while
      changing `satp.MODE` needs no fence of its own (Privileged ISA 12.1.11).
      **WHETHER THE FENCE CHANGES THE PROBE'S RESULT HERE: IT DOES NOT.** Measured both ways by
      revoking the grant immediately before the probe, with and without the fence: identical, load
      access fault `mcause=0x5` in both. QEMU synchronises its own PMP writes, so the fence is in
      the code because the architecture requires it and no run on this bench can tell whether it is
      there. That is the same standing the fresh-map invalidate has.
      Mutations, both directions. FIRES: an inherited Sv39 `satp` with the Bare write removed gives
      the PMP message at `mcause=0xd`; the Bare readback aimed at a hart that will not take Bare
      gives `KickOS: satp did not read back Bare ... satp=0x800000000008000f`. STILL BOOTS: the same
      inherited `satp` WITH the Bare write present boots to the banner, and the unmutated tree is 52
      of 52 on both postures.

- [x] **`mstatus.MPV` was never cleared, so the drop could land in a GUEST. HOLDS, FIXED. NOT IN
      THE REPORT.** Found while checking the finding above. With the hypervisor extension present,
      `MPRV` is governed by `MPV` as well as `MPP` (Privileged ISA, Table 49): `MPV=1` with `MPP=S`
      is a VS-level access under TWO stages of translation. The same field decides where the `mret`
      at the end of the file lands, HS-mode or VS-mode, and in VS-mode `vsatp` governs rather than
      the `satp` this file wrote. `MPV`'s reset value is UNSPECIFIED like `satp`'s.
      **THE EXTENSION IS PRESENT ON THIS HART AND THE FIELD IS NOT WRITABLE**: `misa` reads
      `0x80000000001411ad`, bit 7 set, and `mideleg` written 0x222 reads back 0x1666 with the
      hypervisor bits forced on; but `csrs mstatus, 1 << 39` leaves `mstatus` at
      `0x0000000a00000000`. So the hazard cannot be produced on this bench and the clear is unfired
      here. It is in the code for the hart where the field is live.
      Mutations, both directions. FIRES: the guard aimed at `mstatus.SXL`, a field this hart holds
      read-only non-zero, gives `KickOS: mstatus.MPV did not clear ... mstatus=0x0000000a00000800`
      over the polled UART. STILL BOOTS: forcing `MPV` before the clear, with and without the clear,
      both boot, which is the measurement that shows the field is read-only zero here rather than
      the clear being unnecessary.

- [x] **The both-zero PMP case assumed every OTHER entry was OFF. HOLDS, FIXED.** Entry 0 reading
      OFF matches nothing, and the LOWEST-NUMBERED matching entry decides (Privileged ISA 3.7.1.3),
      so a higher entry the platform mandated at reset (section 3.4 permits exactly that) can permit
      the single DRAM word the probe touches and deny the image, the tables, the UART and the rest
      of DRAM. The probe then reports a grant that does not exist.
      **THE MACHINE THE OLD GUARD PASSES IS A SILENT HANG, WHICH IS WHY THIS ONE MATTERS MOST.**
      Modelled by forcing entry 0 OFF and configuring entry 8 as an 8-byte NAPOT grant over the
      probe word: the decode accepts, the probe succeeds, the port drops to supervisor mode, and the
      first instruction fetch takes an access fault with no console output, no finisher word and no
      diagnosis. `qemu-system-riscv64` is killed by the timeout.
      **THE RECOMMENDATION IS THE CENSUS AND NOT THE FOOTPRINT PROOF, AND THAT IS A DECISION.** The
      report offered "characterise all implemented entries" or "prove the complete required code,
      data, table and MMIO footprint" and called the second the strong answer. It is the WEAKER one.
      PMP granularity is four bytes and an entry may name any NAPOT or TOR sub-range, so a footprint
      proof degrades to a bigger SAMPLE: probing the image's ends, the tables and the device page
      still leaves every gap between them unproven, and it costs MMIO accesses on a path with no
      console. The census is a PROOF and costs eight CSR reads. On RV64 the eight even-numbered
      `pmpcfg` CSRs hold the configurations for all 64 entries and the odd ones are illegal there
      (Privileged ISA 3.7.1), so with every byte in them zero no entry matches any address, and the
      one word's answer IS the whole space's answer by the priority rule. The zero-PMP hart the
      third pass deliberately admitted stays admitted; what is refused is the narrow case the
      finding names, entry 0 OFF with another entry configured.
      **AND THE READ COSTS NOTHING ON THIS HART**: `pmpcfg2`, `pmpcfg4` and `pmpcfg14` all read
      `0x0` rather than raising an illegal instruction, measured, so the census does not itself
      depend on a 64-entry hart. On a hart that hid them the read would land in `.Lmtrap`, which is
      a named refusal rather than a hang.
      The decode's other leg is UNCHANGED and its comment corrected: where entry 0 stands and covers
      the whole space it OUTRANKS every other entry, so the other seven bytes of `pmpcfg0` really
      are irrelevant there. They were being ignored on the both-zero leg too, which is where they
      are not.
      Mutations, both directions. FIRES: that hostile hart with the census present gives
      `KickOS: PMP entry 0 reads OFF while another entry is configured ... other pmpcfg=0x1b` over
      the polled UART and the finisher FAIL word, `qemu-system-riscv64` exiting 132 rather than
      being killed; with the census removed the same hart hangs silently. STILL BOOTS: every entry
      reading zero with nothing else configured still reaches the probe and gives the third pass's
      own `KickOS: PMP denies the supervisor accesses this port makes ... mcause=0x5`, so that leg
      is unchanged, and the unmutated tree is 52 of 52 on both postures.

- [x] **`MEDELEG_REQUIRED` omitted breakpoint cause 3 on an argument about the current image.
      HOLDS, FIXED.** The exclusion read "3, which nothing in this image executes", which is a
      discharge held by CALLER BEHAVIOUR, the class this milestone overturned three times out of
      four. `EBREAK` is in the base ISA, so a valid unprivileged application may execute one at any
      time; undelegated, it terminates in the machine handler with the whole system instead of
      killing its thread.
      **MEASURED BOTH WAYS ON THIS HART** with an `ebreak` in a `hello` thread. Bit 3 delegated:
      `=== THREAD FAULT === thread 'ping' killed, system continues, PC=0x40000052 scause=0x3`, and
      the other thread runs on. Bit 3 held read-only zero with the third pass's required set: the
      guard PASSES, the board boots, and the same `ebreak` prints
      `KickOS: machine-mode trap before the drop ... mcause=0x0000000000000003 mepc=0x40000052` and
      dies there. `medeleg` reads back 0xb5ff on this hart, so bit 3 is delegated and the
      requirement costs nothing here.
      Bit 0 stays excluded on a reason that IS an ISA property rather than an image one: with the C
      extension every branch target in this image is 2-byte aligned.
      Mutations, both directions. FIRES: bit 3 forced read-only zero gives
      `KickOS: medeleg dropped an exception this port depends on: bits 2, 3, 8, 12, 13 and 15 ...
      medeleg=0xb5f7 required=0xb10c`. STILL BOOTS: unmutated, 52 of 52 on both postures.

- [x] **Causes 4 and 6 are neither required nor excluded now: they are MEASURED. HOLDS, FIXED,
      AND THE PROBE IS THE INTERESTING HALF.** Whether a hart raises a misaligned-access exception
      at all is a PMA and therefore per REGION (Privileged ISA 3.6.2), so no register answers it and
      neither blanket answer is right: requiring 4 and 6 refuses the commoner machine, excluding
      them leaves an unprivileged misaligned access terminating in machine mode on the one that
      raises. **The old text cited Privileged ISA 3.1.8 for the "should hold them read-only zero"
      claim; 3.1.8 says nothing about misalignment and the citation is corrected to 3.6.2.**
      The prologue performs a misaligned load and a misaligned store on the DRAM word the probe
      above just proved, under a vector that RECORDS `1 << mcause` and RESUMES rather than refusing,
      and holds `medeleg` against what it recorded. **It cannot brick a hart that resolves
      misalignment in hardware**: such a hart never enters the vector, the accumulator stays zero and
      the check is skipped entirely. The store puts back the four bytes already there, so a hart
      that completes it changes nothing.
      **THE VECTOR RESUMES AT A LABEL AND NOT AT `mepc + 4`**, because the C extension makes the
      probe's own encoding a build-time question and `mepc + 4` the wrong answer for half of it.
      **MEASURED ON THIS HART: NEITHER ACCESS TRAPS.** The accumulator reads `0x0`, so QEMU's `rv64`
      core resolves misalignment in hardware and the whole check is invisible on this board. That is
      the direction that had to be witnessed first.
      **WHAT THE PROBE STILL DOES NOT REACH** is the per-region half of the PMA: it samples DRAM,
      which is where an unprivileged thread's misaligned accesses land, and says nothing about a
      hart that resolves misalignment there and raises on a device page.
      Mutations, both directions. FIRES: aiming both probe accesses at an unbacked physical address
      makes them raise causes 5 and 7, and with those two bits held read-only zero the boot gives
      `KickOS: this hart raises an exception for a misaligned access and medeleg does not delegate
      it ... medeleg=0xb55f required=0xa0`. STILL BOOTS: the same two raising accesses with the hart
      delegating 5 and 7 boot to the banner, which is what witnesses that the vector RESUMED rather
      than hanging; and the unmutated tree is 52 of 52 on both postures.

## External audit of the RV64 backend, 2026-08-29

Five findings against `arch/riscv/rv64imac/aspace_rv64imac.cc`, `arch/riscv/rv64imac/arch_rv64imac.cc`
and `arch/riscv/chip/virt_rv64/startup.S`, held against the RISC-V Privileged specification rather
than against the report. **THREE HOLD AND ARE FIXED. ONE HOLDS AS A PORTABILITY DEFECT WITH NO LIVE
BUG ON THIS BENCH and is fixed anyway. ONE IS A DECISION ALREADY TAKEN and is NOT changed.**
`qemu-riscv64` and `qemu-riscv64-sv48` were 50 of 50 each after the pass and the selftest 133 of
133, one arm more than before; `qemu-arm64` was 40 of 40, the seam having taken nothing. **Those
three ladder totals are DATED to this pass and every count in the items below with them**: the
fleet-wide `whitespace` gate landed later the same day and lifted every board by one, and
`console_reach` lifted the same three by one more -- and `qemu-x86_64`, which this sentence left
out because it was not one of the three the pass measured -- so the registered totals read 52, 52
and 42 now (`ctest -N`, 2026-08-29). **A full run reads 52, 52 and 42, all green.** This paragraph
said `console_reach` was red on the panics still reachable from the console route, which was true
at `517449e5` and stopped being true at `58b43d62` and `d4977780`, the two commits that closed all
four doors; the sentence was not moved with them. The selftest figure DID move and is 134, the plan
line reading `1..134` on all three.

- [x] **Duplicate acquire holds were not release-order safe. HOLDS, FIXED.** Each acquire of one
      (space, page) claimed a window slot of its own while a release names the pair and no
      individual hold, so a release surrendered the FIRST match and could unmap a pointer another
      caller still held. Fixed by keying a slot on (space, page) with a REFERENCE COUNT: repeated
      acquires answer one stable pointer and the mapping stands until the last release, whatever
      order the releases arrive in. `arch/include/kickos/arch/arch.h` already admits this shape,
      counting a hold per call "unless the backend counts references", so nothing at the seam
      moved.
      **NO CALLER IN THIS TREE TAKES TWO SIMULTANEOUS WINDOW HOLDS OF ONE PAGE**, which is why the
      defect was latent: `access_copy` releases its two ends in reverse order but its pages are
      pool frames, which acquire answers by ADDING the kernel window's delta and never through a
      slot, and `op_span` holds two DISTINCT pages. Only a device frame spends a slot here.
      Witnessed by the new arm `aspace_acquire_dup` (`KOS_ASPACE_OP_ACQUIRE_DUP`), which takes two
      holds of one device page, surrenders one, and requires the next page acquired to be answered
      with a different address. With the slot-per-call pool restored the word reads 0x4 against
      0x7: STABLE clear (the two acquires gave two pointers) and DISTINCT clear (the next page was
      answered with the address the surviving hold still named).
      **WHAT THE DEPTH FLOOR MEANS ON THIS BACKEND CHANGED, and that is stated rather than left to
      be inferred.** The pool is still `ARCH_ASPACE_ACQUIRE_MIN` slots and the assert is unchanged,
      but the six now counts DISTINCT pages per core rather than outstanding calls. The scenario the
      six was measured off needs FOUR distinct pages, `ep_copy`'s two ends being the same two
      (space, page) pairs two of the outer holds already name, so the floor is not tightened by the
      change and is not loosened either.
- [x] **Fabricated contexts wrote a reserved `sstatus.UXL`. HOLDS as a PORTABILITY defect, NO LIVE
      BUG ON THIS HART, FIXED.** `.Lrestore` writes `sstatus` WHOLE from the frame and
      `arch_context_init` built the word out of SPIE and SPP alone, so every fabricated frame wrote
      UXL = 0. UXL is WARL at 33:32 with the `misa.MXL` encoding, where 1 is 32 and 2 is 64 and
      nothing else is legal (Privileged ISA 12.1.1.1, Table 11), and a hart answering an illegal
      write with the RV32 encoding takes every U-mode fetch and effective address modulo 2^32.
      **MEASURED ON THIS HART BEFORE AND AFTER, and the measurement is the whole reason to call it
      portability and not a live bug.** `qemu-system-riscv64 -M virt` on the default `rv64` core
      reports UXL = 2 at reset, answers a written 0 with 2 and a written 3 with 2 -- and answers a
      written **1 with 1**. So the field is genuinely writable here and the old code was safe only
      by QEMU's choice of substitute for the reserved value.
      Fixed by carrying the RV64 encoding in every fabricated frame, in `arch_context_init` and in
      `arch_fault_redirect_to_exit` alike, and by seating and reading the field back in
      `kickos_rv64_init` after the privilege probe, refusing by name where it does not stick.
      Positive control: with the seat changed to the RV32 encoding the board prints
      `=== RISC-V S-TRAP (sstatus.UXL is not RV64) ===` and dies, which is also the second witness
      that this hart really does take UXL = 1.
- [x] **`arch_aspace_map` validated only the FIRST physical page. HOLDS, FIXED.** The start address
      was tested against the PPN field and `map_into` then incremented per page, so a range whose
      LAST page reached past the field had its later leaves silently truncated onto low physical
      memory while every write reported OK. Fixed with `phys_range_ok`, which checks the byte count,
      the 64-bit end and the exclusive end's fit in the PPN width BEFORE any entry is written, so
      the seam's "a failed map installs no partial mapping" still holds by not starting.
      Measured with a temporary probe over a two-page map at `2^56 - 4096`. Unfixed: the map
      returned OK, page 0 answered `2^56 - 4096` and page 1 answered PHYSICAL FRAME 0. Fixed: the
      map returned `ARCH_ASPACE_EINVAL` and both pages answered not-mapped. Note the second half of
      the unfixed answer: a low-half page mapped onto frame 0 also breaks
      `arch_aspace_frame_at`'s "0 means not mapped", which `arch.h` states is exact here.
- [x] **THE SAME HOLE WAS IN THE armv8a BACKEND. HOLDS, FIXED, AND THE ARM SHIPPED WITH IT.**
      `arch/arm64/armv8a/aspace_armv8a.cc` tested `(pa & ~DESC_OA_MASK) != 0` on the start address
      alone and `map_into` masks each leaf's output with `DESC_OA_MASK`. Fixed with `phys_range_ok`,
      asked before the first table is edited, against the width `oa_bits()` derives: the narrowest of
      `ID_AA64MMFR0_EL1.PARange`, `TCR_EL1.IPS` and the 48 bits the 4 KiB-granule descriptor's output
      field can encode. **IPS is what the walker enforces** -- an output above the size that field
      configures takes an Address Size Fault rather than translating -- and PARange bounds what may
      legally be programmed there, so neither alone is the answer. A reserved encoding reads 0 and
      refuses every range.
      **THE UNFIXED MEASUREMENT DIFFERS FROM RV64'S AT THE IMPLEMENTED WIDTH AND MATCHES IT AT THE
      FIELD.** Two pages at the top of the implemented physical space (`2^40 - 4096`, this core
      reporting PARange = IPS = 40): the map returned OK, page 0 answered `0xfffffff000` and page 1
      answered `0x10000000000` -- an output address the machine does not implement, NOT frame 0, so
      `arch_aspace_frame_at`'s "0 means not mapped" survives that case here. Two pages at the
      descriptor field's own top (`2^48 - 4096`): the map returned OK, page 0 answered
      `0xfffffffff000` and page 1 answered **PHYSICAL FRAME 0**, which is the RV64 shape exactly and
      breaks that rule. Both are refused now.
      Witnessed by `KOS_ASPACE_REFUSE_PHYS_EXTENT`, a run of two pages based on the last page the
      port's own `arch_aspace_model` says the machine can output, with both pages required absent
      afterwards. Mutation, the whole-range check restored to a start-only one
      (`phys_range_ok(pa, 1)`): `qemu-arm64`, `qemu-riscv64` and `qemu-riscv64-sv48` each read the
      refusal word as `0x7f` against `KOS_ASPACE_REFUSE_ALL` = `0xff` and `aspace_refusals` goes red.
      **THE x86_64 BACKEND'S OWN GUARD IS UNWITNESSED AND CANNOT BE ARMED HERE.** `CHIP_Q35` does not
      `select HAS_ASPACE`, so `KICKOS_HAVE_ASPACE` is 0 on `qemu-x86_64`, the whole probe is compiled
      out and no arm reaches `arch_aspace_map` on that board. Its `phys_range_ok` was left exactly as
      the sibling pass wrote it; the same start-only mutation leaves all 36 tests green, which is the
      measurement of the gap rather than of the code. The bit is a null there until the board earns
      the select.
- [ ] **NOCACHE and DEVICE are still accepted where no PTE attribute carries them. RAISED AGAIN,
      NOT CHANGED, AND THIS IS A DECISION.** `docs/design-m6-mmu.md` R2.2 already argues the
      position and the audit's prescribed remedy is wrong as stated. Refusing outright is wrong for
      DEVICE: with Svpbmt absent the attribute is the physical address's PMA, which for an MMIO
      address IS I/O, so a backend refusing DEVICE would refuse a type the platform does deliver.
      Refusing NOCACHE alone has a live caller: `grant_nocache_admissible`
      (`kernel/include/kickos/grant.h`) asks this member on every translating board, so every
      `KOS_MEM_NOCACHE` self-grant and every `kos_task_create` carrying it would answer `-KOS_EPERM`
      here, and `task_handoff_readback` -- which drives both consumers at `KOS_MEM_NOCACHE` with no
      capability skip in front of it -- goes red. The honest answer differs by PHYSICAL ADDRESS and
      the member takes none, which is the gap F8 records. Two things a decision would have to
      settle: whether the answer moves to a per-CHIP seam beside `arch_mpu_nocache_support`, which
      F8's own rule points at and which is a signature move in the milestone whose deliverable is
      the signature diff; and whether the honesty claim in `memtype_known`'s comment, which reasons
      about "this board", may live in an ARCH file every future rv64imac chip compiles.
- [x] **Boot accepted any non-zero PMP configuration. HOLDS, FIXED.** The readback tested
      `pmpcfg0 != 0` and never decoded the entry. Every PMP CSR field is WARL and may be read-only
      zero (Privileged ISA 3.7.1), so a hart that narrows the A field or drops a permission leaves a
      NON-ZERO `pmpcfg0` describing an entry that grants nothing. Fixed by decoding cfg byte 0 --
      L clear, R and W and X set, A either NAPOT or TOR -- and by testing `pmpaddr0`'s READBACK for
      a run of ones through the top implemented bit rather than comparing it to the value written,
      the register holding bits 55:2 of which not all need exist. TOR is accepted beside NAPOT
      because entry 0's TOR lower bound is zero (Privileged ISA 3.7.1.1), so both modes cover the
      whole implemented space with that address, and a hart implementing only one of the two is a
      hart this port still runs on. Both zero is still PMP unimplemented and still passes.
      **THE REFUSAL IS OBSERVABLE AND IS NOT A HANG**: `.Lm_refuse` emits over the polled UART and
      writes the finisher's FAIL word, so the gate goes red in half a second.
      Mutation, writing `pmpcfg0 = 0x07` (RWX set, A = OFF), which this hart accepts verbatim:
      with the OLD check the boot sails past the readback and dies at the `mret` with the generic
      `machine-mode trap before the drop` message blaming an unimplemented CSR or a bad physical
      address; with the decode it refuses at the readback naming `pmpcfg0=0x...07` and
      `pmpaddr0=0x003fffffffffffff`. The TOR leg is exercised too: writing `pmpcfg0 = 0x0f` boots
      clean, so that branch is live rather than dead.
- [x] **The decoded grant still INFERRED absence from a zero readback. HOLDS, FIXED, AND THIS HART
      DEMONSTRATES IT.** The decode above accepted `pmpcfg0` and `pmpaddr0` BOTH reading zero as
      "PMP unimplemented, every access permitted". Every PMP field is WARL and a legal implemented
      entry may read as zero, in which case A is OFF and, once one entry is implemented, every
      S-mode access that matches no entry FAILS (Privileged ISA 3.7.1). One readback, two opposite
      effects, and no amount of further decoding separates them.
      **MEASURED ON THIS HART rather than argued**, `qemu-system-riscv64 11.0.3 -M virt` on the
      default `rv64` core, with a temporary M-mode probe printing over the `virt` UART: writing the
      real NAPOT grant reads back `pmpcfg0=0x1f pmpaddr0=0x003fffffffffffff` and a supervisor-checked
      load AND store both succeed; writing zero to both CSRs reads back `pmpcfg0=0x0 pmpaddr0=0x0`,
      the exact readback the old code accepted, and the same load and store BOTH FAULT. So QEMU
      implements the specification's rule and the accepted case is the denied one here.
      Fixed by measuring the EFFECT instead of decoding a description of it: `mstatus.MPRV` with
      `MPP=S` makes a machine-mode load and store translated and protected as supervisor ones
      (Privileged ISA, `mstatus.MPRV`), so the prologue reads and rewrites the DRAM word the probe
      above just proved, under a `mtvec` of its own, and refuses by name if it faults. The decode
      stays for the case it does answer, an entry that stands and grants too little, and its
      both-zero leg now falls through to the probe rather than accepting.
      **ONE ADDRESS STANDS FOR EVERY ADDRESS** on the two legs that reach the probe: the decode
      accepts only an entry covering the whole space, and a hart on the both-zero readback has every
      entry OFF or none at all. Reset leaves every `A` field OFF, so the denied case denies
      instruction fetch too, which `MPRV` does not reach; `satp` is untouched there, so the probe's
      supervisor access is Bare.
      **THE SECOND HALF OF THE FINDING HOLDS AND IS NOT FIXABLE AS STATED.** A TOR entry 0's upper
      bound is `pmpaddr0 << 2`, and `pmpaddr0` holds bits 55:2, so the widest TOR range entry 0 can
      name is `[0, 2^56 - 4)` and the top four bytes of the physical space are outside ANY TOR grant.
      Narrowing support to NAPOT would drop harts that implement only TOR; the comment claiming both
      modes "cover the whole implemented space" is corrected instead, and no address this board uses
      is in those four bytes. **This is a decision and not a fix.**
      Mutations, both directions. FIRES: both CSRs written zero boots to
      `KickOS: PMP denies the supervisor accesses this port makes: ... mcause=0x5 mepc=0x8000018e`
      over the polled UART, the finisher FAIL word, and `qemu_riscv64_selftest` red in 0.06 s, so the
      refusal is observable and is not a hang. STILL BOOTS: the unmutated NAPOT grant is 51 of 51,
      and the TOR leg (`pmpcfg0 = 0x0f`) is 51 of 51 too, so that branch stays live code. The
      `MPP` leg is live as well: seating `MPP=M` prints
      `KickOS: mstatus.MPP did not take supervisor ... mstatus=0x...1800` and dies there.
- [x] **The map editor's extent check stopped at the ARCHITECTURAL width. HOLDS, FIXED.** The
      sibling fix above tests the range against the PPN field, which is 44 bits over a 4 KiB granule
      in EVERY RV64 mode: a 56-bit output that is the ARCHITECTURE's figure and not the machine's. A
      narrower implementation takes an ACCESS fault when the leaf is walked, which is later, on
      another thread, and is not a refusal.
      **MEASURED ON THIS HART**, in machine mode over physical addresses with the same temporary
      probe: `2^32`, `2^35`, `2^40` and `2^55` each take a load access fault, while DRAM base plus
      112 MiB and the high PCIe MMIO window at 16 GiB both read. Four samples and not a sweep, so
      what they show is that a leaf above the board's map is BACKED BY NOTHING here rather than
      exactly where the machine stops; every address the BOARD names is below 4 GiB.
      **RISC-V PUBLISHES NO REGISTER FOR IT AND HAS NO WORKING REGISTER TO PROBE**, which is why
      this figure comes from the CHIP and not from the arch: `KICKOS_RV64_PHYS_ADDR_BITS` in
      `boot_layout.ld.h`, handed to `kickos_rv64_aspace_boot` beside the kernel window's range.
      The arch's own 56-bit constant is DELETED from `rv64_paging.h` in the same change, so the
      width is stated once; the PPN field's own bound stays as the round-trip test it already was,
      which names no constant. `arch_aspace_model` reports the chip's figure, so the RV64 model line reads
      `granules 0x1, 16 ASID bits, 32 PA bits, verdict 0x7` where every dated record before this
      reads 56, and the seam arm bases its range on that report so it moved with it.
      **CONSISTENT WITH THE SIBLINGS, DIFFERENT SOURCE.** All three take the narrowest of what the
      machine implements, what the walker is programmed to output and what the descriptor encodes.
      x86_64 reads MAXPHYADDR from CPUID; armv8a takes the narrowest of `ID_AA64MMFR0_EL1.PARange`,
      `TCR_EL1.IPS` and the descriptor's output field. RISC-V has no register for the first, one
      output width for every mode, and a field bound the round-trip already holds, so the board
      supplies the only term the hardware will not.
      **THE FIGURE IS A BOARD CLAIM AND IT IS NARROWER THAN THIS MACHINE. This is a decision.** 32
      is the extent the board's map DESCRIBES; the machine also backs the 16 GiB PCIe window, which
      this port names nowhere and cannot reach without a device tree it does not parse. Refusing
      above the described extent is the safe direction, and the alternative figure is 35, which
      QEMU derives from the RAM size and would move with it.
      Mutations, both directions. FIRES: with the platform clause dropped and the model still
      reporting 32, a two-page map at `2^32 - 4096` is ACCEPTED, the refusal word reads `0x7f`
      against `KOS_ASPACE_REFUSE_ALL` = `0xff` and `aspace_refusals` goes red. Page 1's output is
      `0x1_0000_0000`, which the 44-bit PPN field encodes faithfully, so it is an address the
      machine does not implement rather than frame 0: the armv8a shape at the implemented width, not
      the RV64 shape at the field. STILL PASSES: unmutated, `qemu-riscv64` and `qemu-riscv64-sv48`
      are 51 of 51 each.

## External audit of the M6.2 branch, 2026-08-26

Report at `~/.cursor/projects/home-leduc-projets-KickOS/canvases/m6-2-branch-audit.canvas.tsx`.
Its two merge-gate items are FIXED (the donated-frame lifetime, and `region_mode` deriving geometry
from allocation order). These are the rest. **The five CORRECTNESS items and the two SMP-readiness
items are FIXED (2026-08-27), each with a witness whose mutation fails it**; the four performance
items and the DRY item are a separate pass and are untouched.

**Correctness**
- [x] **A NORMAL re-grant of a NOCACHE mapping returns success and changes nothing.** FIXED: where
      the mapping CARRIES the type (`grant_memtype_programmed`), the short circuit is now decided by
      `user_range_typed_ok` in BOTH directions and never by rights alone. On the region family the
      descriptor is REPLACED rather than stacked (`MpuSet::add_enforced_retyping`), so one block never carries
      two types with the range checks reading the first and the hardware obeying the last, and a
      refused re-type puts the old descriptor back. Witnessed by `self_grant_retype` (qemu-arm64),
      which drives NORMAL -> NOCACHE -> NORMAL and reads `KOS_ASPACE_OP_MEMTYPE_AT` after each; with
      the check restored to its NOCACHE-only form the NORMAL-over-NOCACHE leg fails. Recorded in
      `user/include/kickos/sys.h` (kos_mem_self_grant) and `docs/design-m6-mmu.md`.
- [x] **After root dies, a later process becomes the mutable data template.** FIXED with an
      immutable pool-backed snapshot, taken on the HOME'S WAY OUT rather than at the first copy.
      Its frames come off the pool while root is seeded (so no balance window sees them leave) and
      `aspace_release` fills them from root's still-standing mappings before clearing `g_data_home`.
      While root lives the source is unchanged, which is load-bearing: `irq_driver` and its peers
      read a global root writes AFTER its ctors, and freezing the snapshot early broke them
      outright. A seed reaching a lost home with no snapshot behind it is refused, and no process
      other than root is ever the template. Witnessed by `process_data_template`, which stages the
      loss of the home, moves root's own word, and reads a later process's copy back; without the
      snapshot clause the later process answers root's FRAME and root's LIVE word. Recorded in
      `docs/design-m6-mmu.md` section 3.4 and `kernel/include/kickos/aspace.h`.
- [x] **Reentrancy seating dereferences low-half user addresses directly.** FIXED by stating the
      distinction and asserting it: `aspace_seated_for` answers whether THIS CORE's translation root
      is the thread's own space, and the switch path (both `switch_book` and `start`) primes and
      seats nothing when it is not. A privileged spawn resolves to the kernel domain, which carries
      no space, so this is the posture rather than an edge; idle needs no prime, which is the whole
      reason it was survivable. Witnessed by `reent_seating`: bit 1 counts app-half writes made
      while unseated, counted inside `reent.cc` where the guard is NOT, so removing the guard trips
      it; bit 0 is the positive control that the posture is reached at all, counted in
      `aspace_activate_for`. Recorded at `root-unprivileged-idle-alone-privileged`
      (`docs/reference/invariants.md`) and in `docs/design-m6-mmu.md`.
- [x] **`aspace_frame_token` releases acquisitions that may have failed.** FIXED: each acquire is
      checked before anything is released, and the reference hold is released on the second
      acquire's failure path. Every acquire in `aspace.cc` now goes through one `acquire_page` /
      `release_page` pair, so the pairing is a property of two functions rather than a rule each
      call site keeps; the same pass removed a leaked hold in `data_copy` (a null `dst` beside a
      live `src`). Witnessed by `aspace_acquire_balance`, which reads a new selftest counter of
      unpaired releases after taking a token for an unmapped page; the old release-before-check
      order reports 1 unpaired. Recorded beside `arch_aspace_acquire` in
      `arch/include/kickos/arch/arch.h`.
- [x] **`VirtualRanges::overlaps` computes `base + pages * granule` with no overflow defence.**
      FIXED: it now runs the same checked extent arithmetic `reserve` does and FAILS CLOSED, a
      wrapping window overlapping everything the list could name, so a caller that has not checked
      its own arithmetic is refused rather than admitted on a wrapped end. `vrange.h` states that it
      has no precondition. Witnessed by the host arm
      `VRange.overlaps_fails_closed_on_an_extent_whose_arithmetic_wraps` (sim), over both a wrapping
      byte count and a wrapping end; without the check both legs answer false.

**SMP readiness, and M7 cannot start on top of these**
- [x] **`g_current` is process-global rather than per-core.** FIXED as
      `g_current[KICKOS_NUM_CORES]` keyed by `arch_cpu_id()`, which folds to a literal at one core,
      matching `g_rv_trap_stack`'s precedent; T9's `armv8a_percpu` could not host it, `vectors.S`
      spelling that block's one displacement as a literal and `kernel/mem/aspace.cc` being portable
      code. `aspace_release` now clears the cell on EVERY core and rewrites only the running core's
      root. **NO BEHAVIOURAL WITNESS EXISTS AT ONE CORE** and none is claimed: the change is
      byte-equivalent there. What holds it is a `static_assert` on the cell count, the whole
      aspace suite as the no-regression witness, and the `cpu_id_fold` gate, which caught the first
      spelling of it. Classified in `docs/design-m7-state-inventory.md` and stated in
      `docs/design-m6-mmu.md` section 3.4b.
- [x] **`frame_pool_ptr` reads allocator bitmap state without the lock its siblings take**, and
      `g_data_home` has no ownership or publication model. FIXED both. `frame_pool_ptr`,
      `frame_pool_free` and `frame_pool_refused` take the pool's `IrqLock` (nesting-safe, and the
      seed path already holds one); `frame_pool_init` and `frame_pool_total` are the two documented
      exceptions. `g_data_home` now has an owner and a publication rule: published by the first
      seed, read only by `data_copy`, and cleared by the release of that space AFTER the snapshot
      beside it is frozen, so every read after that point resolves to the snapshot. Both are stated
      in `kernel/include/kickos/frame_pool.h` and classified in
      `docs/design-m7-state-inventory.md`. **NO BEHAVIOURAL WITNESS AT ONE CORE** for the lock
      itself: nothing in the tree allocates or frees frames from an interrupt, so there is no
      single-core interleaving to construct. The publication half is witnessed by
      `process_data_template`.

**Performance, measured by the audit**
- [x] **Process creation keeps interrupts masked across image cloning**: about 104 KiB of memcpy and
      roughly 57 page-invalidation sequences inside one `IrqLock`, four synchronising instructions
      per page. Worst-case interrupt latency then grows with app data size, and it dwarfs the
      syscall-frame latency T7 already owes a number for. A space not yet published can be seeded
      without masking globally.
      HALF FIXED, and the other half is REFUSED with its reason. **The invalidations are gone
      outright**: nothing tags a translation, so `write_ttbr0` drops the whole low half on every root
      change, and a space whose root this core has never installed holds neither a cached entry nor a
      cached absence. `invalidate_page_if` takes that answer from `installed_here` (TTBR0_EL1 against
      the space's own root), asked once per `arch_aspace_map`/`unmap` call. Measured inside the
      `IrqLock` of ONE `kos_task_create` on `qemu-arm64`: **62 page-invalidation sequences before
      (248 synchronising instructions), 0 after**; the running space's own widening still pays every
      page of it. Elided above one core, a sibling's TTBR0 not being readable here. Witnessed by
      `map_tlbi_elided` over the new `KOS_ASPACE_OP_MAP_TLBI`; two mutations fail it, and the
      positive control is load-bearing because eliding UNCONDITIONALLY is invisible to every other
      arm (QEMU models no stale entry, which is T8b's own negative-control result).
      **The 32-page copy stays masked deliberately, and the audit's reasoning does not reach it.**
      Everything a seed WRITES needs no global mask, but the SOURCE is root's LIVE static data, and
      the mask is what makes those 32 pages one point-in-time snapshot of it: with interrupts on, a
      user IRQ handler or a sibling thread of root's task can write an app global between two pages
      and the child starts on an image no writer ever saw. Nothing in the suite would show it.
      Removing the mask needs an immutable template with a fault handler behind it, not a lock
      change. A second blocker sits under that one: the copy is inside `spawn_masked`'s
      function-scope lock, and a spawner slain in an unmasked gap never returns, so a slot claimed
      and an image seeded there have nothing left to free them. Recorded at T11a in
      `docs/design-m6-mmu.md` and beside `data_copy` in `kernel/mem/aspace.cc`.
- [x] **`-mcmodel=large` is applied to whole targets to reach a few cross-half symbols**, costing
      3832 bytes of kernel text (78998 to 75166 recompiled small), on a fixed 1 MiB budget and on the
      scheduler and syscall paths' I-cache. Scope it to the translation units that name those
      symbols, or publish linker-filled pointer cells.
      FIXED as `kickos_split_image_tu(<target> <sources>...)`, scoped to the **six** TUs that name
      the app's half: `kernel/init/kmain.cc`, `kernel/thread/reent.cc`, `kernel/mem/aspace.cc`,
      `kernel/domain/domain.cc`, `arch/arm64/armv8a/arch_armv8a.cc` and
      `arch/arm64/chip/virt_arm64/chip_virt_arm64.cc`. No pointer cells: no new ABI surface was
      needed. The list is DERIVED from two sweeps over a fully small tree, and both are needed
      because the classes differ in loudness: the reach class fails the link (7 truncations, all
      `R_AARCH64_ADR_PREL_PG_HI21`), while `mem/aspace.cc` and `domain/domain.cc` hold only weak
      externs whose GOT references LINK CLEANLY and break nothing until something else forces the one
      `.got` to be placed. Whole-target cost was 4076 bytes of archive text over a fully small build;
      naming the six recovers 2724 of it. Measured on this tree, `qemu-arm64` selftest link:
      archive text 81092 -> 78292, ELF `.text.init`+`.text` 82368 -> 80032. In a
      selftest-OFF build the whole pass is 75539 -> 73308.
      **The kernel still calls no app text**, which is what T5b.3's witness rests on: 1864
      kernel-half branches swept, 0 into `.apptext`, with the same range test over `.apptext` itself
      firing 3224 times as the positive control. `kernel_runtime` stays green.
      **The silent GOT class now has a gate**: `kernel_got`
      (`tests/static/check_kernel_got.sh`) refuses any `_GOT` relocation in the three archives, with
      the relocation count as its positive control. Dropping `kernel/mem/aspace.cc` from the list
      turns it red and names the four symbols while the link and the runtime suite stay green, which
      is the whole point. **`qemu-arm64` is therefore 33 tests, not 32.**
- [x] **The frame bitmap is scanned one bit at a time under the lock.** A host micro-benchmark of the
      same scan shape measured word-wise at 19.5x the shipped 8 MiB pool and 75x to 86x at 128 to
      512 MiB. Keep the bitmap and first-fit, skip whole words, and count trailing zeros.
      FIXED. The scan reads a `size_t` at a time and takes the lowest free bit with a count of
      trailing zeros; the bitmap and first-fit are unchanged. **Allocation ORDER is unchanged and
      that is measured, not argued**: old and new return the same offset from base in every shape,
      every pool size and both optimisation levels.
      **THE FIRST WORD-WISE DRAFT WAS A REGRESSION ON THE PATH THAT NEVER SCANS.** `release` parks
      the hint on the frame it freed, so the common case tests one bit and answers; routed through
      the two-pass word loop it measured 2.1x to 2.4x SLOWER, unchanged by the aliasing fix since
      that path never reaches the word load. A hint test ahead of the loop returns it to parity,
      and both paths exit through one `take_one`, so a single place still turns an index into an
      address. Measured at 8 MiB, the size a real pool resembles: no-scan path at parity (one cell
      1.13x faster), scan-heavy 11.28x at `-O2` and 15.72x at `-O3`.
      The tail word needs no upper mask: bits at and past the frame count read as ALLOCATED, and
      the assembling loop stops at the bitmap's own byte length rather than reading past it.
      Witnessed by new host arms in `tests/unit/frame/frame_alloc.cc` over the wrap pass and the
      tail word.
- [x] **Per-domain range bookkeeping is 43.6 percent of the kernel instance**: `VirtualRange` 24
      bytes, `VirtualRanges` 776, times 20 domain slots is 15520 of 35592. Narrow `pages` and
      `rights` after a static proof, and compile `Domain::regions` out of translating domains.
      FIXED. `VirtualRange` 24 -> 16 bytes (`pages` `uint32_t`, `rights` `uint8_t`), `VirtualRanges`
      776 -> 520. **Neither width is assumed.** Three `static_assert`s in
      `kernel/include/kickos/vrange.h` say the widest value each setter can be handed ROUND-TRIPS
      through its field, and the setters refuse rather than truncate: `reserve` refuses above
      `VR_MAX_PAGES`, `grant` refuses a rights word carrying a bit the entry cannot hold. Both
      refusals are witnessed by new host arms
      (`VRange.reserve_refuses_a_page_count_the_entry_cannot_hold`,
      `VRange.grant_refuses_a_right_the_entry_cannot_hold`), each failing when its refusal is
      deleted. `Domain::regions` is `KICKOS_DOMAIN_REGIONS`, which is 1 where a backend translates:
      the kernel singleton's whole-arena descriptor is the only one any domain there records, and
      `domain_region_at` now debug-asserts the bound. `region_count` is a byte.
      **Measured on `kickos::detail::g_instance`: 35752 -> 27112 bytes**, of which 5120 is the range
      lists (20 x 256), 3360 the region arrays and 160 the region counts. Per-domain range
      bookkeeping falls from 43.4 percent of the instance to 38.4.

**DRY, with the audit's own caution attached**
- [x] Power-of-two checks are identical in `frame.cc` and `vrange.cc`; frame-run free loops are
      identical in `aspace.cc` and `ustack.cc`; `pages * granule` with its overflow check recurs in
      `vrange.cc`, `aspace.cc` and `aspace_armv8a.cc`. **Share the checked extent arithmetic and NOT
      the loops**: the traversals differ in bypass and exact-attribute semantics, and a flag-driven
      merge of security-sensitive range walks would be easier to misuse than the duplication.
      SHARED, in `include/kickos/extent.h` rather than under `kernel/` or `arch/`: both layers reach
      it and neither includes the other's headers, which is the same shape
      `include/kickos/units.h` already has. It carries `is_pow2` and `extent_end`, the latter
      FAILING CLOSED on a byte count that overflows a pointer and on an end that wraps. It is
      C++-only, so it stays out of the `c_headers` corpus and drags nothing into any
      `extern "C"` include closure. The two identical frame-run free loops became
      `frame_pool_free_run`, in the pool's own header beside `frame_pool_alloc_run`: byte-identical
      bodies, no parameter added, and the pool's refusal counter stays the only accounting.
      **DELIBERATELY LEFT DUPLICATED**, and the audit's caution is the reason: `VirtualRanges::find`,
      `covers` and `overlaps` are three traversals of ONE array with three different admission rules
      (any live entry, `Granted` plus a rights superset, any live entry with a fail-closed extent),
      and the map editor's leaf walks in `aspace_armv8a.cc` differ in break-before-make and in
      attribute handling. Unifying either set takes a flag over a security-sensitive traversal.
      `aspace_armv8a.cc`'s `range_ok` DOES now use the shared `extent_end`, which was the third site
      the audit named, and it keeps its own alignment and low-half bound on top; `aspace_refusals`
      reports `bits 63 of 63` across the change, which is what says the refusal set did not move.
      `kernel/mem/aspace.cc` had no `pages * granule` of its own to share: its arithmetic is the
      BYTES-to-pages round-up (`bytes > SIZE_MAX - g`) and the page alignment of a linker extent,
      both different computations, and its share of this item was the frame-run loop.

## Found while witnessing T5b.3 (2026-08-26)

- [ ] **`appdata_no_kernel` does not run on the one board that splits its image.** The gate is
      registered under `KICKOS_HAVE_MPU` and keys on `__kickos_appdata_start/_end`, which
      `virt_arm64.ld` does not define. So the guard against a kernel archive silently landing in the
      app's low window is absent exactly where that window is now **the only memory EL0 can reach**,
      and a kernel object landing there would be an isolation hole with no gate over it. The script's
      own `ASSERT(_ebss > _sbss)` catches total selector failure and nothing narrower. Enabling it
      needs the two bounds plus a SECOND window pair for `.apptext`, the EL0-executable one, which
      the existing gate has no concept of. Pre-existing since the app moved low; T5b.3 is what made
      it matter.

## Found while witnessing T5b.2, and predating it (2026-08-25)

- [x] **`errnoprobe` HANGS on `qemu-arm64`.** Root-caused to the armv8a backend, not to the probe:
      `arch_switch` latched BOTH `from` and `to` into a single deferred-switch request, so a second
      reschedule inside one ISR (`ktime_on_timer` wakes every expired sleeper in one sweep) left
      `from` naming a thread the scheduler had merely published. The IRQ exit then saved the
      interrupted frame through that wrong context and lost the real one. The M-profile and RISC-V
      backends ignore `from` and save `g_arch_current`; armv8a now has the same cell,
      `kickos_armv8a_ctx_current`, re-seated by the IRQ exit. The gate is registered
      (`qemu_arm64_errnoprobe`), the four arms reach their verdicts, and the failure was NOT
      errnoprobe-specific: any two threads whose deadlines expired in one timer sweep corrupted a
      context on this arch.

## Found while witnessing T5, and predating it (2026-08-25)

- [ ] **`fault_dump` HANGS under a non-default service list.** Configured with
      `-DKICKOS_SERVICE_LIST=kickos_services_simuart`, the ctest `fault_dump` gate times out: the
      fault dump prints correctly and the process then never exits. Reproduced on a clean worktree at
      `364aea7a`, so T5 did not cause it. **It is invisible to both sweeps by construction**, nothing
      in `sweep_host_gates.sh` selecting an alternative provider, which is why it survived this long.
      A dump that completes and then fails to terminate is a drain or an exit-path defect rather than
      a reporting one.

## M6.4 x86_64: what the `qemu-x86_64` port owes

Each item is a residual the port left standing on purpose, not a defect it hid. The board's own
caveats and its validation status are in `docs/reference/boards.md`.

- [ ] **The RAM arena is ONE contiguous conventional span, so a machine whose memory straddles
      4 GiB loses the smaller side.** `pick_arena` (`arch/x86/x86_64/entry_x86_64.cc`) merges
      adjacent conventional runs, does not assume the map is sorted, clamps a run straddling the
      1 MiB floor, and then takes the largest single span. At `-m 8G` the largest span is the one
      ABOVE 4 GiB, so 511,373 pages (24.5% of the conventional memory the map names) below the
      boundary go unused. **A multi-run arena is kernel-side work**, not a fix inside this pick:
      `arch_ram_alloc` and the frame pool are both written against one base and one size.
- [ ] **The boot path's stack depth is unbounded and unguarded.** `kickos_x86_64_landed`
      (`arch/x86/x86_64/landed_kernel_x86_64.cc`) moves the kernel onto a 128 KiB image-owned
      static in `.bss`, which is what takes an overrun off firmware's stack and out of the type-7
      Conventional run sitting directly below it. Nothing measures the depth and nothing guards the
      low end. The size matches what UEFI 2.11 section 2.3.4 makes firmware supply, so the switch
      cannot be a regression on that count, which is the whole of the argument standing behind it.
- [ ] **The adopted translation root lives in `EfiBootServicesData`, memory UEFI says the OS may
      reclaim.** This port never reclaims boot-services memory: `conventional_span` takes type 7
      alone, so nothing it hands the allocator can overlap the root, and the regime `arch_init`
      adopted stands for the life of the image. That is an UNSTATED PREMISE rather than an enforced
      one. Nothing asserts it, and the first code that frees boot-services memory pulls the live
      root out from under the map editor.
- [ ] **`arch_cpu_clock_hz` is 32 bits wide in the seam, so a part above 4.295 GHz is reported
      CLAMPED rather than exactly.** The measured timestamp-counter rate is 64 bits
      (`apic_tsc_hz`), and `arch/x86/x86_64/arch_x86_64.cc` clamps to `0xffffffff` rather than
      truncating, which is wrong in the direction a caller can reason about. **Widening it is a
      seam change**: `arch_cpu_clock_hz` is declared in `arch/include/kickos/arch/arch.h` and every
      backend and every caller moves with it, so it does not belong to this board.
- [ ] **The i8254 reference timebase is MANDATORY on this board and its rate is HARDCODED.**
      `calibrate` (`arch/x86/x86_64/apic_x86_64.cc`) measures both the local APIC timer and the
      timestamp counter against it, because no CPUID leaf on this processor model reports either
      rate, and there is no second source in the tree. A channel 0 that does not advance is refused
      loudly, so the failure mode is not silence; a channel 0 driven by a different crystal is,
      because `pit_hz` is the literal `1193182` in `arch/x86/chip/q35/chip_q35.cc` and every
      nanosecond conversion is scaled by it.
- [ ] **The xAPIC MMIO window is dereferenced without being mapped or checked, and it is the
      default leg.** `apic_write` and `apic_read` take the MSR path only under `g_x2`; the
      fall-through stores to and loads from `g_mmio + reg`, the address `IA32_APIC_BASE` named.
      The file's own header says why that address is not safe to assume: F8 has it that the adopted
      map covers exactly what the UEFI memory map DEFINES, and the local APIC window is not in it.
      Nothing tests the mapping before the first access, and no gate reaches the leg on a processor
      model that reports x2APIC.
- [ ] **`swapgs` in the trap path keys on the FRAME's cs, so an NMI or a machine check taken in
      either swap window runs on the USER gs base.** `arch/x86/x86_64/trap_x86_64.S` swaps on
      `testb $3` against the pushed cs at entry and against the frame's cs at the resume, which is
      right for an ordinary trap and for a frame some other entry built. It leaves two windows in
      which the pushed cs no longer describes which base is live. **Harmless only because no
      handler dereferences gs**, which is exactly what SMP per-CPU data in the trap path would
      break, so this is a prerequisite of M7 on this arch rather than a standing nit.
- [ ] **TSC invariance is never checked.** `CPUID.80000007H:EDX[8]` is not read anywhere, so
      `clock_now` rests on a rate measured once at `apic_init` and assumed constant. On the
      emulator it is; on a part whose counter varies with the core clock or stops in a deep state,
      every deadline after the change is wrong by the drift and nothing reports it.

## M6.2 closing sweep: two debts the pass could not discharge (2026-08-26)

Both are recorded in `docs/design-m6-mmu.md` beside the obligation they belong to. They are here so
they are known debts rather than forgotten ones.

- [ ] **T7's OWED LATENCY MEASUREMENT, and there is no instrument to take it with.** T7 makes the
      compact-SVC-frame decision wait on an aarch64 round-trip figure: the exception frame is 800
      bytes and a syscall moves about 1.6 KiB with interrupts masked. No such figure exists anywhere.
      The blocker is the rig: `boards/qemu-arm64/configs/` holds a `base` variant alone, so there is
      no `KICKOS_BENCH` image for the board, and `tools/bench/bench-fleet.sh` does not list it.
      **Standing up a bench variant for this arch is a step of its own** and belongs with M8's
      instrument, where the comparison it feeds lives. Until it is taken, no compact-frame decision
      may be argued -- which is what T7 froze.
- [ ] **`qemu-arm64` declares no SERVICE LIST, so F10's real consumer never runs on the one board
      that translates.** F10 makes `drv::bring_up` the gate for the whole allocation ABI and says in
      terms that no selftest arm substitutes for it. It runs on region boards and against host fakes,
      where nothing translates and the same-address rule is vacuous. `task_handoff_readback` is the
      substitution, and it is a good arm that is not the gate. Porting a service list to this board
      is its own step; M6.2 records the gap rather than closing it.

## M6 -- the per-thread-privacy restatement, owed WITH the behaviour (2026-08-24)

`docs/design-m6-mmu.md` freeze F9 moves the portable contract: a task's grants are sibling-visible, a
thread-scoped grant guarantees access to its HOLDER, and portable code may not rely on sibling
denial. MPU backends keep per-thread denial as a documented strengthening; a translating backend maps
task-wide with grant AUTHORITY still thread-local.

**Deferred by the user to land WITH the T5 code change, not before**, because every statement below is
true of the tree as it stands and rewriting them early puts the Reference and the code in
disagreement the other way round. The list is here so the deferral is tracked rather than
remembered.

- [x] **`docs/reference/invariants.md` -- four sites.** `dev-window-access-is-thread-scoped-its-lifetime-task-scoped`
      and `dev-window-exclusivity-is-bounded-by-the-silicon` both argue that a task-wide window would
      hand registers to a peer that never asked, which is exactly what a translating backend does; the
      private-stack clause of `mpu-apply-on-every-switch-in`; and `privileged-write-seam-possession-and-allowlist`,
      which is REPAIRED rather than qualified once possession becomes thread-local authority instead
      of a walk over reachable regions. `tls-carve-sits-below-stack-lo` is the precedent for the
      wording: it already says outright that it is naming and not isolation.
- [x] **Four kernel comments claiming a sibling cannot reach another thread's stack**, in
      `kernel/include/kickos/domain.h`, `kernel/thread/thread.cc` (two) and `kernel/syscall/syscall.cc`.
      `kernel/init/fault.cc` already admits the sharing and needs nothing.
- [x] **`docs/reference/architecture.md` -- two statements** that per-thread private stacks stop a
      sibling scribbling another's.
- [x] **The Book.** `privilege-is-three-axes-not-one-bit.md` teaches the private stack as a
      guarantee. `whoever-stacks-the-trap-frame-owns-the-bounds-check.md` already frames a sibling's
      access as a legitimate granted mapping, which is the telling to converge on.
- [x] **The ABI headers.** `user/include/kickos/sys.h` states the `-KOS_EPERM` non-holder contract for
      the periph seam and describes the self-grant budget as per-thread. These stay TRUE if and only
      if possession becomes thread-local authority, so they are the check on that work rather than
      text to soften. **CHECKED at T5: both stay true with no edit.** The non-holder contract is
      now read off `Thread::dev_base`/`dev_size`, and the self-grant budget is still
      `KICKOS_MPU_MAX_REGIONS` on every backend that has landed; F10 is what makes the budget
      clause evaporate under paging, and that is not this step.
- [x] **Do not touch the two board-registered arms.** `periph_enable_unheld` and
      `periph_reg_write_unheld` (`user/apps/common/selftest/main.cc`) assert a non-holder is refused
      from a thread other than the holder, and they must keep passing on every backend. They are the
      executable check that authority stayed thread-local; a change that needs them relaxed has
      widened possession by mistake. **CHECKED at T5 and untouched**: both pass on every backend,
      and both FAIL when the possession helper is mutated to answer for a caller holding nothing,
      so they discriminate on the record and not on the region set.

## MMU-era groundwork quick wins (from `docs/design-mmu-era-exploration.md` section 5)

Cheap seam/groundwork changes worth making WHILE the M4-M6 code is written, so the MMU era does not
force a breaking rewrite. Ordered by leverage, as recorded. QW-2 has LANDED (`kaccess_from_user` /
`kaccess_to_user`, `kernel/syscall/syscall_mem.cc`) and is not repeated here.

- [x] **QW-1. Give `Domain` an opaque backend field instead of a bare region array.** LANDED at
      T5: every read outside `domain.cc` goes through `domain_region_count`/`domain_region_at`,
      and `Domain::space` (an `arch_aspace*`, compiled only where the backend translates) is the
      backend field beside them. The accessor half is what kept the representation swap to one
      file, which was the whole reason to do it early.
- [ ] **QW-3. Keep the shared-IPC ring contract PHYSICALLY addressed from day one.** When the IPC
      ring lands (`docs/design-m7-smp.md`), specify that ring control words and slot
      references are offsets or physical addresses, NEVER a pointer valid in one core's space, even
      though on RP2040 (homogeneous, one physical space) a raw pointer would work. It costs nothing
      there and is the exact property a heterogeneous A53/M7 pairing needs. Baking a VA into the
      ring on the homogeneous prototype would silently work until the first MMU peer, then break
      the wire format; getting the invariant into the design text is free, retrofitting it after
      apps depend on the layout is not. The same discipline is what
      `docs/design-m4-fable-review.md` finding 10 wants pulled into the M4 call/reply gate.
      **IT IS LOAD-BEARING FOR M6 NOW, and in the direction of it NOT having landed (2026-08-24).**
      `docs/design-m6-mmu.md` F10 contracts the reserve-then-hand-to-a-task idiom as a same-frame
      handoff at the SAME virtual address, and the reason it must be the same address rather than
      merely the same frames is that nothing guarantees a shared block's CONTENTS are
      position-independent: an address the donor computed can be sitting inside it. This item is
      exactly what would relax that, so landing it buys M6 the freedom to relocate a child's view,
      and leaving it open makes the same-address rule a requirement rather than a convenience.
      Either way it stops being cheap groundwork and becomes a dependency to state.
- [ ] **QW-4. Isolate the pow2/natural-alignment MPU shaping so a page allocator can sit beside the
      bump allocator.** `arch_ram_region_size` / `arch_ram_region_align` encode MPU-descriptor
      geometry into the ALLOCATOR. Flag them as "MPU shaping" belonging behind the same arch family
      switch that would later select frame allocation, with no behavior change, and do not let new
      callers assume "allocation size is always the MPU-rounded size" outside the allocator. The
      pow2 assumption already leaks (`domain_for` dedups on the rounded size) and each new leak is
      another site a frame allocator must reconcile.
- [ ] **QW-5. Confirm the cap/handle layer stays address-space-agnostic, and keep it that way.** A
      do-no-harm review rule, not a change: the per-task handle to per-kernel object-pool model
      (`cap.cc`) is ALREADY MMU-clean, and no cap/endpoint code should start keying on a physical
      address or a region base the way `domain_for` does. Object naming stays purely by
      handle/slot, never by address. Free, since it is the current design, but endpoint IPC is
      exactly the code tempted to stash a shared-buffer physical address in a cap, which would drag
      address-space assumptions into the one layer the MMU rewrite relies on being clean.
- [x] **QW-6. Reserve an `arch_aspace`-shaped hole in the arch seam doc, not the code.** `arch.h`'s
      header prose already frames the seam as "concepts, never mechanisms" and freezes
      `arch_mpu_region`. Add a NOTE that the MMU era introduces a PARALLEL `arch_aspace_*` family
      rather than reinterpreting the MPU seam, so a future porter does not try to overload
      `arch_mpu_apply` to mean "load a page table". A sentence of foresight prevents a wrong-shaped
      first MMU port.

## M7 -- multicore (SMP on the A53 port, AMP where cores are heterogeneous)

- [ ] **Give the converted fields their real ORDER.** M4.9.2 turned every cross-thread field
      into a relaxed `std::atomic`, which is a type change and nothing more: relaxed says
      nothing a second core will honour. What is left is deciding, per site, where an
      acquire or a release belongs. Three known residues: the 64-bit fields that had to stay
      `volatile` (a relaxed 64-bit load is a `__atomic_load_8` libcall), the six per-chip
      `_high`/`_last` clock anchors that `IrqLock` alone makes coherent, and
      the ring publication barrier, then still a consumer `-D` rather than a release
      store on the ring's now-atomic index. `docs/design-m7-smp.md` carries the reasoning.

## Gate-surface re-inventory (anytime coherence, not scheduled)

Parked by the user as "at M7" when M7 meant the seam rework; M7 is multicore now, and this is
orthogonal to every milestone rather than gated by one. The user's framing, 2026-08-21: the gate
surface grows exponentially and may not be worth its size.

- [ ] **Re-inventory the test-gate surface.** M4.5.9 root-caused the binary-introspection gates'
      silent-failure paths without rewriting them; whatever is still oversized then is this pass.
      Take the inventory against the surface as it is, do not pre-design it here. Baseline at
      M4.5.8: 23 shell scripts, ~2,000 lines, plus 5 Python checkers -- STALE, predates the `tests/`
      reorganisation: the shell scripts now live under `tests/static/` and `tests/integration/`,
      unit gates under `tests/unit/`, and the host unit layer is GoogleTest with per-case `ctest`
      entries rather than scripts. Re-derive the count when this pass runs; do not carry the old
      numbers forward as current.

## M8 optimizations (not scheduled)

- [ ] **RISC-V context-switch cost** (M8, fable-gated) -- the rv32 trap saves the full
      integer file (~60 stack words/switch vs armv7m's ~18); ~3.5x per-handoff, general to RISC-V
      (Hazard3 shares it, NOT C6-specific). Levers: (a) cooperative fast-path (callee-saved-only
      voluntary switch, ~2x, portable incl. C6); (b) optional Zcmp `cm.push`/`cm.pop` compile-gated
      path (Hazard3-only, code-size mainly). Prerequisite: fix the rv32 bench bracket (it currently
      excludes the save/restore). Full design in `docs/design-riscv-switch-cost.md`; roadmap
      "Later". Surfaced by the M3 C6 enforcement soak (C6 ~10.5k iters vs XMC ~33.9k, same window).

- [ ] **ARMv8-M TrustZone kernel-confinement backend -- opt-in, per-chip** (M8, fable-gated,
      needs the M4 service model + M6 SMP settled). The armv8-M-with-Security-Extension mechanism for
      kernel confinement: kernel/TCB in Secure state, apps in Non-secure. NOT per-task isolation and
      NOT an MPU replacement (MPU_NS still does all per-task work, same per-switch cost); it is the
      strongest armv8-M realization of "Option B" (confine the kernel), layered ON TOP of Option B,
      not instead of it. Buys a hardware TCB boundary (NS-privileged cannot touch Secure memory) + a
      PSA-style secure-services partition for roots-of-trust that fits the capability-gated-services
      model. Machinery: SAU/IDAU partition, secure-gateway veneers + S/NS call ABI, banked SPs, NVIC
      ITNS interrupt targeting, a separate Secure build/link. Per-chip capability -- M23/M33/M55/M85
      MAY implement it, detect + fall back to Option B alone; RP2350's M33 is a concrete target (also
      the PMSAv8 + SMP target). Security/assurance play, not perf. The M6 dependency is mechanical.
      The MPUs and the SAU are both banked per core, so a TrustZone SMP story must set up the
      S/NS partition on each core separately.
- [ ] **Confine the trusted kernel with an explicit MPU map ("Option B") -- FLEET-WIDE hardening**
      (M8, fable-gated, per-arch). Today privileged/kernel execution runs UNCONFINED on each
      backend's permissive background; a kernel wild pointer rides it silently instead of faulting.
      Option B removes that background so even the kernel is confined and a stray kernel access
      FAULTS (defense-in-depth / debuggability -- catch our own bugs early; NOT a security boundary,
      the kernel is trusted). This is NOT a bug fix anywhere -- the M7 speculation stall is already
      closed by "Option A" (wrap the leaky external Normal bands, keep PRIVDEFENA;
      `docs/design-teensy-mpu-hang.md`); no other arch has that stall. Per-arch mechanism:
        - armv7m/armv6m PMSA (XMC/F411/RP2040/microbit): drop PRIVDEFENA + region-0 4 GiB
          Strongly-ordered/no-access/XN floor + explicit kernel regions (code RX, RAM RW, periph
          Device). M0+ is region-tight (8 descriptors).
        - K64F SYSMPU: restrict RGD0 (today supervisor-full) + explicit supervisor RGDs.
        - RISC-V PMP (C6): LOCKED PMP entries (bind M-mode too).
        - RX-MPU (RX72M): restrict the supervisor region set. Xtensa (WROOM): N/A (no MPU).
      Cost: forks the fleet-wide "privileged = background" contract every board rests on (incl. the
      armv7m non-pow2-arena-drop path) -- needs a per-arch fable pass + probe-ful bring-up.

## M4.8.1 re-witness after the generic-service rework (2026-08-10)

The six-board pass at `e21167b6` is SUPERSEDED: a witness is valid for a TREE, and the rework
replaced every service bring-up in the tree. Retaken at `1c250bad` over the remote bench
(`.session/logs/m481r-*.log`).

| board | class witnessed | plan | result |
| --- | --- | --- | --- |
| `rx72m` (RXv3, RX MPU) | RX MPU, and `rxsci` itself | `1..95` | 95 ok, 0 not ok, enforce |
| `xmc4800-relax` (PMSAv7) | PMSAv7 | `1..95` | 95 ok, 0 not ok, enforce |
| `picopi` (PMSAv6, armv6m) | **armv6m enforcement, a FIRST** | `1..95` | 92 ok, **3 not ok**, enforce |
| `esp32-wroom` (LX6) | no-unit | `1..91` | 91 ok, 0 not ok, off |
| `f302nucleo` (ring-only) | ring-only | `1..51` + `1..40` | 51 + 40 ok, 3 + 7 skip, 0 + 4 partial |

**`rx72m` is the load-bearing row.** `rxsci` went 332 lines to 110 and is the outlier that decides
whether the descriptor's variation points are right -- three threads, two lines with different
per-thread rights, a relay holding no grant, and a spawn before the readiness barrier. It passes on
RXv3 silicon under MPU enforcement, and there is no RXv3 emulator anywhere, so nothing else could
have said so.

**picopi's 3 failures are PRE-EXISTING and this pass proves it**: the same three arms with the same
asserts fail identically at `a1220233` (before the rework) and at `1c250bad` (after). See the
armv6m section above.

**NOT WITNESSED WHEN THIS WAS WRITTEN, AND WITNESSED SINCE** -- at `367497c2` and after, recorded
further down this same section and in `STATE.md`'s fleet tables. `frdmk64f` and `esp32c6-wroom` were
absent from the bench at the time, so SYSMPU and PMP NAPOT had no witness of this tree; both have
one now, and so does `rpusb`, which has since run on `picopi` as well as `pizero2350`. The paragraph
is kept for what it says about the RISK of shipping a converted driver unwitnessed, not as a
statement of coverage. Both are converted drivers
(`k64uartirq`, `k64uart`, `k64dspi`, `c6uart`) and one of them carries a deliberate behaviour
change: `k64dspi` now panics where it used to `exit(-1)`. That change is build-only. `rpusb` is also
unwitnessed -- it builds and links for both `pizero2350` and `picopi` but has never run since the
conversion, and its own console-reclaim premise is a named open gap.

## picopi USB CDC console hard-faults, on BOTH trees, at DIFFERENT sites (2026-08-10)

**RESOLVED in this same section: 11 consecutive clean CDC runs after the PendSV pair fix.** The
header states the symptom as it was found; the diagnosis and the fix follow below, and they are why
the section is kept -- chasing this is what surfaced the PendSV race that had been miscategorised as
a flaky arm.

First time `rpusb` has ever run on an RP2040; every prior CDC witness is `pizero2350` (RP2350).
Preset `picopi-st` with `-DKICKOS_SERVICE_LIST=kickos_services_picopi_usbcdc`, app `usbcdcwit`.

**What WORKS, and it is not nothing.** The device enumerates: `/dev/ttyACM0` appears **1.0 s** after
boot on both trees, so the descriptor tables, the chapter 9 request machine and the RP2040 USB clock
tree all come up. The kernel banner reaches the GP0 UART and then stops, which is the console
publishing to USB as designed.

**What FAILS: a HARD FAULT, and zero bytes ever reach the ACM.** Both in unprivileged thread
context (`(PSP)`, `IPSR == 0`), both AT A SYSCALL STUB:

| tree | PC | LR (caller) | R0 |
| --- | --- | --- | --- |
| `a1220233` pre-rework | `kos_call`, `syscall_stubs.cc:142` | `uart_call`, `usbcdcwit/main.cc:60` -- the APP | `0x0` |
| `aa38390a` post-rework | `kos_irq_notify`, `syscall_stubs.cc:317` | `usb::serve_one`, `usb_cdc_service.h:702` -- the SERVICE thread | **`0x2b`** |

**`0x2b` is 43, and `KOS_SYS_IRQ_NOTIFY = 43`** (`abi.h:119`). So `R0` holds the syscall number at
the moment of the fault: the `svc` escalated to a HardFault instead of dispatching. That is the
shape to chase, and it is an armv6m-specific one.

**DO NOT record this as "pre-existing, the rework is innocent".** Both trees fault, so a defect
predates the rework -- but the SITES DIFFER, and the post-rework one is in the SERVICE thread where
the pre-rework one is in the APP. The service must be up before the app calls, so the post-rework
fault is EARLIER. That is consistent with two different stories and this pass cannot separate them:
either one root cause surfacing sooner, or a SECOND service-side fault masking the first.

**First thing to check**, because it is the one the rework could plausibly have broken:
`kos_irq_notify` acts on the doorbell cap, and the doorbell is `caps[1]` of the service thread in
the new descriptor. A wrong cap INDEX would give `-KOS_EBADF` or `-KOS_EPERM` rather than a fault,
which is why `usb_cdc_service.h`'s `desc_ok` exists and why it is not obviously implicated -- but
the doorbell is exactly the object this syscall touches, so verify the index before looking wider.

**Two facts this pass establishes for free:**
- **armv6m's fault reporter WORKS on silicon**, which `STATE.md` records as having executed nowhere.
  The dump is legible: PC, LR, xPSR and R0-R3 plus R12. That gap is closed independently of the CDC.
- **`KICKOS_SHUTDOWN_TO_BOOTLOADER` survives the fault path** (`kickos_terminate` reaches
  `arch_reboot`), so a FAULTING picopi image still returns itself to BOOTSEL. picopi is fully
  self-serve even for images that die.

**Instrument note, and it cost a void measurement.** For a CDC run that knob cuts BOTH ways: it
rescues a faulting board, and it REMOVES THE DEVICE when the app ends normally, so an ACM read after
the fact returns 0 bytes from a stale node and reads as "not one byte reached the ACM". Open the ACM
the instant it appears, in the same shell as the flash.

## The PendSV pair fix is witnessed on both armv7m MPU classes (2026-08-10)

`367497c2` is silicon-proven beyond the picopi runs that found it. Taken under the `_uartirq`
service list deliberately, because the race needs a DEVICE IRQ preempting PendSV and the polled
default lists generate almost none.

| board | class | plan | result |
| --- | --- | --- | --- |
| `picopi` | armv6m / PMSAv6 | -- | **11 consecutive CDC runs, zero faults**, 5.4-5.8 KiB per run, against 3 faults in 4 before |
| `frdmk64f` | armv7m / **SYSMPU** | `1..95` | 94 ok, 1 not ok -- the pre-existing `thread_join` arm, IDENTICAL to its pre-fix baseline |
| `xmc4800-relax` | armv7m / **PMSAv7** | `1..95` | 95 ok, 0 not ok |
| `picopi` selftest | armv6m | `1..95` | 92 ok, 3 not ok -- the documented armv6m IRQ arms, unchanged |

So all three MPU backends that share the fixed path are covered. `rx72m` is RXv3 and does NOT
exercise it; `f302nucleo` has no MPU, so its region commit is a no-op.

**A LEAD, not a claim.** `xmc4800-relax` + `uartirq` reported `rr_interleave` failing BEFORE the fix
(`m481u`) and clean after (`pv-xmc`). One run each way proves nothing -- `rr_interleave` is already
recorded as unreliable under `uartirq` -- but the coincidence is worth chasing, because the bug just
fixed was precisely "a device IRQ preempting PendSV corrupts the switch", which is an ORDERING
corruption, and `rr_interleave` and `thread_join` are both ordering/timing arms that only misbehave
under interrupt load. If the marginality was partly THIS, it should now be reproducibly clean.
**The arch caveat that stops this being tidy:** `TODO.md` records the `rr_interleave` marginality on
`rx72m` + `uartirq`, and rx72m is RXv3, so the ARM fix cannot explain that one. Either there are two
causes or the RX case is the real one. Settle it with repeated `xmc4800-relax` + `uartirq` runs before
touching the arm.

**The first RP2040 CDC bytes ever.** Every prior CDC witness is `pizero2350` (RP2350). The console now
carries payload on an RP2040. The next defect is named in `367497c2` and is NOT fixed: `main` returns,
the bootloader teardown drops about 2.7 KiB still queued in the PUBLISHED console's TX ring, and the
app's own PASS line is inside that. The shutdown path drains only the kernel transport, and
`accepted=` counts ring acceptance rather than delivery.

**A separate latent defect, found and deliberately left alone:** `arch/arm/common/arch_arm_common.cc`
writes `SCB_SHCSR |= SHCSR_MEMFAULTENA` in code SHARED with armv6m. **SHCSR does not exist on
ARMv6-M** and there is no MemManage exception there; the comment above it is a v7-M statement. RP2040
reads it back as 0, so it is RAZ/WI and harmless today, but it is architecturally a reserved-SCS
access on a v6-M core.

## rr_interleave's ARM marginality had a ROOT CAUSE: the PendSV pair race (2026-08-10)

`TODO.md` carried `rr_interleave` as "not a reliable arm", and the earlier note said five trees gave
five orders, i.e. it was treated as inherently marginal and left alone. It was not inherent on ARM.

**`xmc4800-relax` + `kickos_services_xmc4800relax_uartirq`, six runs each side of `367497c2`:**

| tree | runs | `rr_interleave` failures |
| --- | --- | --- |
| `7bdf1067`, before the PendSV pair fix | 6 | **4** |
| `367497c2`, after it | 6 | **0** |

Every failure was arm 11 and only arm 11, at `main.cc:462`, `nth('B', 1) < nth('A', 2)` -- an
ORDERING assertion. That is exactly what the fixed bug corrupted: a device IRQ preempting PendSV
between its two reads of `g_arch_next` and the region stash, so a switch completed with one thread's
stack and another's regions. Under the `_uartirq` service list there is real interrupt traffic to do
the preempting, which is why the arm was marginal there and nowhere else.

**The RX half of the old entry does NOT reproduce, on either tree.** `rx72m` +
`kickos_services_rx72m_uartirq` is clean 6/6 (five runs at `367497c2` plus the earlier `m481u` run at
`7bdf1067`). rx72m is RXv3 and shares none of the fixed ARM code, so both trees are the same code
there -- and both are clean. So the recorded rx72m marginality either predates something already
fixed or was specific to the trees it was measured on, which is what "five trees give five orders"
literally says. **It is not evidence of a second live cause.**

**What this changes:** `rr_interleave` should be treated as a REAL arm again on ARM, not a known
flake. A flake label on an arm whose failure had a root cause is worse than no label -- it is what
kept a genuine scheduler race hidden, and the arm was doing its job the whole time. Any future
"marginal on silicon" verdict wants a rate measured on both sides of a change before the label goes
on.

**Caveat, stated because the numbers are small:** 4/6 against 0/6 is suggestive, not conclusive; a
Fisher exact on those counts sits around p = 0.03. The mechanism is what carries the claim, not the
counts -- an ordering arm failing under interrupt load, cured by fixing an ordering corruption caused
by interrupt load.

## thread_join is NOT a flake either: 80% on frdmk64f + uartirq, and nowhere else (2026-08-10)

Found by applying the `rr_interleave` lesson one board over -- measure a RATE before writing "flake".

| board + service list | runs | `thread_join` failures |
| --- | --- | --- |
| `frdmk64f` + `kickos_services_frdmk64f_uartirq` | 5 | **4** (7 runs total across the day: 5) |
| `xmc4800-relax` + `..._xmc4800relax_uartirq` | 5 | 0 |
| `rx72m` + `..._rx72m_uartirq` | 5 | 0 |
| `esp32-wroom` + `..._esp32_uartirq` | 1 | 0 |
| `esp32c6-wroom` + `..._esp32c6_uartirq` | 1 | 0 |

**80% is not marginality, it is a defect**, and it is K64F-specific and IRQ-service-specific: the same
board on its DEFAULT list (`k64uart` + `k64dspi`, polled) is 95 ok clean, and every other board is
clean under its own IRQ list.

**It is NOT the PendSV pair race.** That fix is in this tree and cured `rr_interleave`; this survives
it, so it is a different cause.

The assertion is `waited_us >= JOIN_PARK_US` at `main.cc:5395` against `JOIN_PARK_NS = 20000000`
(20 ms): the target sleeps 20 ms and the join must not return before that. So **the join returns
EARLY** -- either a spurious wake or a mis-measured elapsed time.

**The first thing to check, and why:** `CONTEXT.local.md` records that the K64F's **DWT is dead**, so
its cycle counter is unavailable and only wall-clock is valid. If `waited_us` is derived from
anything DWT-shaped on this board it would under-report and the arm would fail while the join
behaved correctly -- an instrument fault, not a kernel one. That distinction decides whether this is
a real early wake (kernel) or a bad measurement (test). Rule the instrument out before chasing the
scheduler, because a 20 ms park is long enough that a real early wake would be a serious IPC defect.

Note the discipline that surfaced this: it had already been A/B-proved identical before and after the
service rework, which made it easy to file as pre-existing and stop. Pre-existing is not the same as
harmless, and a rate is what tells them apart.

## exit() scope is CHIP-defined, not app-defined (ruled 2026-08-07)

The ruling: on an MCU a thread IS the unit of isolation, so a thread and a process are the same
thing and `exit()` in a thread ends that thread. On an A-class part with an MMU and real processes,
`exit()` from any thread ends the WHOLE PROCESS, which is what POSIX says. So the scope follows the
target's process model. It is a platform fact derived from the memory model, never a knob an
application author sets: an app that could choose would be choosing whether its peers die.

**Consequence for the tree today, and it inverts the open question in `m4.8.1: the services exit
through the C library`.** Every current target is the MCU case, so `exit()` must mean thread-exit
everywhere, and the cross ports do NOT deliver that. Disassembly of `exit` in the RX image: it calls
`__call_exitprocs` (the `atexit` / `__cxa_atexit` list), then loads `__stdio_exit_handler` and calls
it if non-null, and only then reaches `_exit` -> `kos_exit`. Both are IMAGE-global teardown, and
running image-global teardown because one thread ended is wrong even under thread-equals-process:
newlib assumes one process per image and KickOS puts many threads in one. Provably inert right now
(nothing registers an atexit handler and KickOS uses its own `kprintf`, not newlib stdio), and the
`.ld` `ASSERT(.fini_array empty)` does not cover it, because that assert governs STATIC registration
only. It goes live the first image that links real stdio, which the consumer-API principle
("standard `printf` / `std::cout`") invites.

`user/src/sim_exit.cc` already does the right thing and is the model: override `exit()` outright and
route it to `kos_exit`. Do the same on the cross ports. Overriding `exit` and not `_exit` is not a
style choice: glibc's `exit()` calls its own hidden alias, so an `_exit` definition is never reached
(measured when `sim_exit.cc` was written).

When the MMU era arrives (`docs/design-mmu-era-exploration.md`), the same seam flips rather than
grows a second mechanism: on a target with processes, `exit()` ends the process and the thread-scope
primitive stays `kos_exit`.


## THE DOC AUDIT (anytime coherence, runs parallel to M4.8.4 and the driver era)

**Why now.** The f302 close showed the failure mode: `docs/reference/boards.md` carried a WRONG root
cause in four places for weeks, and it had never been updated for the 2026-08-01 reframing either, so
it held TWO superseded stories at once while the bench quietly disagreed with both. A doc that is
wrong is worse than a doc that is missing, because it retires the question. `doc_names` cannot catch
any of this -- it validates that a path and an identifier RESOLVE, never that a claim is still TRUE,
and it reads TRACKED MARKDOWN ONLY, so the same citations in source comments, CMake strings and
workflow YAML rot invisibly.

- [x] **Sweep every doc for a claim the tree has since falsified. DONE.** The f302 reading was
      deleted from `boards.md` in all four places plus `flashing.md`, `design-m4.7.9`,
      `design-m4.6-irq-driver` and `roadmap.md`; every measurement kept as taken, and the one
      verbatim capture line fenced rather than edited. Three more superseded verdicts came out with
      it: `invariants.md` and `console.md` stated the PRE-2026-08-02 console reclaim (keyed on the
      endpoint's last receiver) as the contract, `design-m7-smp.md` asserted the retracted
      both-cores-WFI debug-bus story as chip fact, and the `arch_console_reclaim` body count was
      wrong in six files. `architecture.md`'s cap-slab figures were both one chunk high, and are now
      read off the `KickOS: cap table =` configure line instead of a formula.
- [x] **`docs/reference/architecture.md`: delete what is false, do not restructure. DONE.** The
      cancel reaching one park, privilege as a domain property, caller-owned kernel objects,
      `msg_recv`, the ThreadX preemption threshold, a thread-flag notification, PI as future work, a
      missing `kernel/task/`, and the per-task capability table at 24 sites. The task syscalls joined
      the thread-lifecycle surface. **Deliberate residue, left for the rewrite**: six positioning and
      prior-art sentences still say "per-task isolation" where the mechanism ones now say per-thread.
- [x] **`TODO.md`'s own stale strata. DONE for the class that was pure noise**: the section
      describing `M4.6.1-irq` as an unpushed unmerged branch is gone, replaced by a how-to-read
      header that keeps its one non-re-derivable fact: which backup refs the older hashes resolve
      against. The remaining STALE markers are self-annotated backlog and stay.
- [x] **`docs/design-c6-driver.md:34` -- NOT A DEFECT, and the finding itself was the false one.**
      That document DOES carry numbered open questions (`## Open questions / risks`, items 1-3), and
      item 3 is exactly "a timer-IRQ follow-on needs an arch/chip change", which is what line 34
      claims. The parenthesis stays. Worth keeping as the lesson: this audit's own named exemplar was
      a misreading, so a finding about a citation gets the cited document opened before it is filed.
- [x] **Gate or habit -- DECIDED 2026-08-13: the habit, plus one narrow gate fix, and NO widening.**
      The evidence for refusing the widening is measured. Identifiers: with `docs/` out of the
      oracle only TWO dead names surface tree-wide, both masked for one reason, and that is the
      one-line fix that landed in M4.8.4. Paths: swapping the corpus to source files resolves 544
      repo-rooted citations but leaves 269 file-relative ones and 51 unexpanded shell/CMake
      variables reporting, i.e. about 3% precision before any resolution work -- and the gate lives
      by "a checker that cries wolf gets disabled". Line numbers stay uncheckable by the gate's own
      argument. What actually caught everything above was READING, so the habit is: the per-milestone
      review gains one angle that re-reads every doc the milestone's diff touched and asks whether
      anything now says two things. The staleness convention was NOT adopted: most measured claims
      already carry their tree, and every failure here was a verdict with no tree to carry.

## M4.8.x TRIAGE: the 30 items the three milestones left, sorted by what they actually are

**Why this section exists.** A review finding has exactly TWO honest dispositions: **FIX IT, or ASK
FOR A DECISION.** Filing is not a third one -- it is what you do with class D below, and nothing else.
Across this file the practice was the opposite: 200 open against 207 closed, every review filing 10-18
while closing 1-3, and the pile is undifferentiated so an accepted trade and a latent defect read
identically at a glance.

**And do not reach for "no mechanism blocked the merge" -- there is no such mechanism to have.** A
gate is not an artifact: a ctest entry, a static check, even the arm that proves a fix can all be
DELETED, which is precisely why the gates in this tree are mutation-tested rather than trusted. The
discipline is the gate. Recording a defect in this file and merging anyway is a choice, never a
process outcome.

**So the rule for a future review: fix each finding, or raise it as a decision, before the merge.**
Only class D is filable.

**A -- LATENT DEFECT in shipped code (6). ALL SIX ARE NOW ADDRESSED, and TWO WERE MIS-FILED in ways
that changed the answer.** Kept in full because the mis-filings are the lesson:
- [x] **`sched::wake`'s guard reading a moved `kernel().current`.** Mechanism CONFIRMED, conclusion
      REFUTED: the DECISION is provably unchanged in every constructible case, because admission
      required `t->prio > sweeper->prio`, so the stale publication is `pick_next`'s own answer and
      `reschedule()` early-returns on it. Verified BOTH ways -- deleting the `dying` clause reds only
      the fresh-window arms, which is the machine-checked form of "dead in that window, and nothing
      depends on it there". Residue reclassified to class B below. Two things no filing said:
      `arch_switch`'s `from` is IGNORED by all three pending backends, so no context corruption is
      constructible; and the same double publication is reachable from a LIVE thread through
      `task_cancel_group` and the endpoint drain, with no dying guard involved -- the MORE reachable
      path.
- [x] **`sched::wake` resurrecting an `EXITED` thread.** `wake` now readies only a `BLOCKED` thread.
      Total form, because `EXITED` is `ThreadPool::alloc`'s free marker; all 14 call sites verified to
      pass a queue-popped or park-resolved thread. Gated, and the mutant leaks the slot visibly.
- [x] **The `CAP_IRQ` slot and console reclaim having no equivalent of the DEV window's early
      release.** Fixed: every `CAP_IRQ` entry is released in ONE masked window before the chunked loop
      opens its first gap, and `console_on_driver_death` now runs at the note inside that same window.
      Six mutants, all killed. **The filed reading was incomplete**: inertness needed priority AND
      chunk alignment, and on `esp32-wroom` the alignment condition does not exist at all because
      `arch_switch` is synchronous in thread context -- one grant-list line from live.
- [x] **The `extern "C"` anonymous namespace emitting UNMANGLED GLOBALS**, and the backend audit that
      was never done IS done: all 6 arch cores, all 15 chips, by construct scan AND by an `nm` class
      sweep. 19 symbols across four backends; the SEVENTH site (`chip_esp32c6.cc`, a plain missing
      `static`) is one only the `nm` sweep could see. Gated by `extern_c_linkage` on every board.
      One correction: under `-fno-common` a duplicate is a HARD ERROR, not a silent collision -- the
      real cost is a consumer's pure-C `main` breaking over a symbol it never asked for.
- [x] **`task_release` freeing the slot before `cap_teardown` so `c->task` dangles.** `exit_current`
      retires the name with the reference. The early release STAYS: `domain.h` depends on it.
- [x] **An explicit task's slot and domain never swept at the creator's death. FILED AS A LEAK; IT IS
      AN AUTHORITY ESCAPE.** `kill_tag_for_index` derives the tag from the pool slot, nothing clears
      `Task::creator_tag` at the creator's death, and a never-freed task has `gen == 0` so its handle
      is `index + 1` -- which the selftest itself notes is guessable. So the SUCCESSOR of a dead
      creator's thread slot passes `task_created_by` for the predecessor's groups: it can kill them,
      and it can `kos_thread_create` a child into one and hand that child the group's domain regions.
      `t_task_creator_gate` covers only a concurrently-live stranger. **This refutes the filed option
      "declare the hold the creator's for life" -- that IS the escape.** Fixed by
      `task_orphan_created_by` from `exit_current`, which is total over deaths where
      `ThreadPool::alloc`'s reclaim point is not.
      **GATED, and the gate is the HOST one rather than the selftest arm that was proposed.**
      `kernel/task/task.cc` joined `kickos_kseam`: it cost dropping two stubs and adding three
      (`domain_for`, `domain_ref`, `domain_release`) over a fake domain pool, and NOT the arena
      or granule seam, which only `kernel/domain/domain.cc` would have dragged in.
      `Fixture::task()` mints through the real `task_create` and `join_task` through the real
      `task_ref`, so the refcount, the hold and the slot free are the shipping ones.
      `tests/unit/taskdeath/creator_hold.cc`, 6 arms; three mutants killed -- deleting the sweep
      reds 5 of the 6, dropping its tag test reds the two that say a stranger's death changes
      nothing, and a `task_drop_hold` that never frees an empty slot reds the third.
      **What it does NOT reach:** the syscall refusals themselves, `syscall_thread.cc` being
      outside the seam. The `-KOS_EPERM` wiring is already on silicon through
      `t_task_creator_gate`, which differs from the successor case only in what the predicate is
      handed, so the composition is covered and only an end-to-end successor arm is not.

**B -- ACCEPTED WITH A MEASURED COST (3), and these are the ones that should have been raised as
decisions rather than filed. RAISED, and the ruling is that ALL THREE GET FIXED.** The ABI grows a
second verb: kill stays cooperative (0 = ACCEPTED, death at the next syscall ENTRY) and **SLAY** is
the forcible half, SIGTERM/SIGKILL. **The mechanism is NOT a reaper** -- no stranger ever runs another
thread's `cap_teardown`; the victim runs its own, because the seam rebuilds `next->ctx` inside
`switch_to` before `arch_switch`. Two premises died on the way: `arch_fault_redirect_to_exit` cannot
be reused (not relocatable to a saved context on any backend, and on armv7m and rxv3 it reads AND
CLEARS sticky fault status, so calling it off a fault destroys the reporter's evidence -- the seam is
`arch_context_init`), and the termination argument is NOT the RR slice timer, because the clock is
TICKLESS and an all-FIFO image with no sleeper has no periodic interrupt at all. The real argument:
on one core a target that is not the caller is never RUNNING, so READY and BLOCKED are total.
Four non-reorderable steps, zero `.bss` on each. Design record still owed to `docs/`.
- [x] **S1 DONE AND WITNESSED (M4.8.4 S1, TAG `m484s1`). `kickos_fault_below_stack` narrowed to a
      16-byte guard band (one RXv3 MPU region page) and the fault stub relocated to the top of the
      dying thread's own stack on all five backends.** The width WAS settled on the bench, as
      required: `mpu_fault`'s `0x13200` went from escalating to dying thread-scoped, and
      `faultsurvive_ovf`'s `0x121fc` still escalates. The two addresses proved separable, so the
      fallback (keep the stack-base test, accept the false positive) was not needed.
      `kernel/init/console.cc` now prints the faulting thread's stack range, which is what made the
      gap measurable; `faultsurvive_ovf` reports `its stack 0x12200-0x14200`, so the denied push is
      `stack_base - 4` and the band's 4-byte floor is a measured shape.
      Gated by `tests/unit/faultband`, 11 arms, band MEASURED off the predicate so retuning it does
      not rewrite them. Three mutants killed, including the `addr + BAND >= base` spelling, whose
      first arm was VACUOUS -- the discriminating case is a stack near the TOP of the address
      space, not the bottom, because the `addr >= base` early return fires first.
      Two defects found in the work itself: armv6m reached the stub with `b` (T2, +/-2 KB) and
      every picopi image failed to link (`R_ARM_THM_JUMP11` truncated), now a PC-relative literal
      plus `bx`; and the new program was refused by `test_labels` until declared, which is that
      gate working.
- [x] **S2: `bool cancelled` becomes `uint8_t cancel_kind` {NONE, KILL, SLAY}**, same offset, so
      `sizeof(Thread)` is unchanged by construction. Provably inert on its own.
- [x] **S3: the seam, the hook and the thread-slay syscall.** `KOS_SYS_THREAD_SLAY = 53`.
      `arch_ctx_redirect` on six backends, VOID rather than the design's `bool`: no backend can
      fail, and a silent decline would downgrade a slay to a cooperative kill with nothing able to
      report it. The gate on the `sched::wake` work turned out to be a gate on nothing -- the
      moved-`current` window is inside the kernel under an `IrqLock`, so no syscall can be issued
      from it and the self refusal is exact. Mutation confirms it (the design predicted that
      mutant would survive). `lx6` needs no extra line and the design's instruction for it is
      INVERTED. Two orderings the design does not have are load-bearing and mutation-proved: the
      caller parks BEFORE the victim's cancel, and samples its resume epoch before it too, because
      an inline-swap backend can run the victim to `EXITED` from inside that call.
- [x] **S4: the group form, the task-slay syscall.** `KOS_SYS_TASK_SLAY = 54` with
      `WAIT_TASK_EMPTY`, which costs FOUR arms and not the one the design prices (the exit sweep,
      `thread_abort_park`, `ktime_on_timer`, the enum value). `task_release` REPORTS the emptying
      rather than the caller re-deriving it: the transition happens once, only its cause can see
      it, and an implicit task's slot is freed inside that call.
- [ ] **SILICON OWED FOR S3/S4 ON TWO OF THE THREE.** `rx72m` IS PAID: the `m484capirq` capture
      recorded below is a tree carrying S3/S4, and the five arms are registered unconditionally, so
      its `1..104` covers them. Still owed: `esp32-wroom` (`lx6` -- the immediate-switch backend
      with two resume formats, and the ONE backend whose seam claim is a reading of the code rather
      than a run) and `picopi` (the only armv6m enforcement unit). The
      five arms to look for in a selftest stream are `thread_slay_window`, `thread_slay_gate`,
      `thread_slay_timeout`, `task_slay_group`, `task_slay_gate`.
- [ ] **Two fail-closed guards in slay are UNREACHABLE from userspace and their mutants SURVIVE**
      (`docs/design-kill-and-slay.md` section 14.5): the idle/privileged target refusal, because
      idle is a static TCB outside the `ThreadPool` and root is unprivileged, so no privileged
      thread is resolvable by handle at all; and `kos_task_slay`'s caller-is-a-member refusal,
      because a member cannot be its own group's creator. Both become reachable in the driver era
      or under M5. `kickos_fault_kill_thread` carries the identical rule with the identical
      reachability status, so this is a pre-existing shape rather than a new one.
- [x] **`rx72m/rxsci`'s relay thread sees the ring block it declared no grant for. RAISED AND
      RULED: the third memory scope is REFUSED, and the declaration that implied it is DELETED.**
      A task is defined as the set of threads sharing one memory domain, so a member that shares
      the task but not its domain makes the definition false -- two answers to "what memory do this
      task's threads see" is the second truth the tiebreaker forbids, and the right decomposition
      for a thread that must not reach the block is a task of its own, which today would also take
      it out of the kill group. Separating "shares memory" from "dies together" is M6-scale.
      **The collapse landed instead.** The per-thread flag's only reader was an OR-reduction into
      the group's grant, and it equalled `arg == KOS_DRV_ARG_BLOCK` in all TWELVE production descriptors, so it was
      a second truth twice over; `bring_up` now hands the task the block whenever there is one. Leg
      L4 grew the converse arm, refusing a block no thread reads -- the widest ask a descriptor can
      make, and nothing else would have caught it. Three mutants killed, one of them at COMPILE time
      by the `static_assert` beside the fixture.
      **The residual, stated at `Descriptor::block_size`, at `kos_drv_arg` and at the relay itself:**
      every thread of a block-owning driver sees the whole block whatever its argument, because a
      task owns exactly one `Domain` and a member may bring no grant of its own. The ruling is
      `docs/design-task-layer.md` open question 7.
      **`rx72m` has no emulator and no CI gate**, so the rxsci edit is build-verified only.
- [ ] **The `sched::wake` residue, moved here from class A because it is an ACCEPTED COST and needs a
      ruling.** A superseded publication keeps a `switch_count` it never earned and an RR slice armed
      before it ran. **It is UNDETECTABLE IN C**: no state separates "published, switch pended, not
      fired" from "fired and running", and any marker written at the pend is stale from the fire
      onward, so it cannot recognise the supersede it would have to undo -- on the sim/lx6 inline-swap
      path a stale marker is WORSE than the current code. Removing it needs the arch to report that a
      pended switch completed, or `need_resched` consumed at the mask boundary: five backends plus the
      K-seam stub. Cost of leaving it: bounded by one quantum, self-limiting, fairness only.

**C -- GATE AND TOOLING DEFECTS. FILED AS 4, CLOSED AS 10** -- the filings under-counted, and every
one of the six extras was a gate that had silently stopped gating or had disclosed a hole it never
closed. The first seven are mutation-proven; the two S3/S4 found are below them and each has its
own proof shape.
- [x] **`check_tap_stream.sh` could not see a thread that died the WRONG WAY.** Found by mutation
      during S3, and the mutant survived a fully green run. An unprivileged context rebuild faults
      the exit stub on its first kernel access; thread-scoped fault isolation catches it, so the
      victim still dies, the guarantee still looks honoured, and every plan / case / skip / partial
      check reconciles. The only trace was a `=== THREAD FAULT ===` line nothing read. The selftest
      never faults by design, so that record is now a refusal in the SHARED verdict script rather
      than in one arm -- it catches any future arm that quietly takes the degraded path. **The
      general lesson: a gate built on reconciliation cannot see a mechanism that failed into a safe
      fallback, because the fallback restores the observable end state.**
- [x] **Every K-seam gate failed to LINK under `sim-telem`, and had since M4.8.2.** `ktrace.h` is
      header-inline and `kernel/irq/irq.cc` reaches it, so with telemetry on the whole directory had
      two undefined references (`arch_trace_now`, `kickos_rtt_write_record_ch1`). The preset went
      from 2% of 146 to 100% of 216. Verified PRE-EXISTING both ways against a pristine pre-S1
      archive tree. `karch_seam.cc`'s own header comment had a symbol count that had stopped being
      true, which is the tell: **the seam's size is a property of the PRESET as well as of the
      source set, and a preset nothing routinely runs is a preset whose seam nothing re-derives.**
- [x] **`check_faultsurvive.sh` missed THREE enforcement classes, not two, and MISJUDGED one.**
      `armv6m` was absent as well, and `picopi` is the fleet's only armv6m enforcement unit. The
      misjudgement: `armv7m:overflow` demanded MemManage `MSTKERR`, and `frdmk64f`'s BUS-level SYSMPU
      latches BusFault `STKERR` with `MSTKERR` CLEAR, so it would have FAILED a correct capture.
      Now takes the SYSMPU shape (corroborated by the `SYSMPU ISOLATION FAULT` line so a bare
      BusFault cannot pass), plus `armv6m` and `rxv3` arms. `FS_CAPTURE=<log>` judges a captured
      stream, because those two classes have NO runner and their clauses would otherwise have shipped
      never once executed; the exit clauses report `NOT EVALUATED` rather than passing on an absent
      value. **Free mutation:** the archived `m484mut` record-route mutant is now REFUSED by this gate.
- [x] **`tests/lib/panic.ere` never matched `=== RX EXCEPTION` or `=== XTENSA EXCEPTION`.** It backs
      every ctest `FAIL_REGULAR_EXPRESSION` and `assert_no_panic`, so both were blind to the rxv3 and
      lx6 reporters -- exactly the "a system that panicked after printing both lines would look the
      same" case that script's own header warns about.
- [x] **Three gates parsed tab-separated records with `IFS=$'\t'`, a bashism, and `/bin/sh` IS dash
      here as well as on CI.** dash sets `IFS` to `$ \ t`, so records split on the letter `t`:
      `/var/tmp/libkickos_kernel.a` came back as `/var/` and `mp/libkickos_kernel.a`.
      `check_seam_defaults.sh` leg 1 tests `case "$arch" in *libkickos_kernel.a)`, which a truncated
      value can never match, so **that leg was VACUOUS**. `dash -n` passes the bashism, so nothing
      catches it. `gate.sh` owns `TAB` now.
- [x] **`doc_names`' identifier ORACLE included `docs/`**, and the M4.5.1 audit ledger tracked
      there was an .html -- TRACKED and not markdown -- so every name it mentioned stayed valid
      forever, masking 39 references to two deliberately-deleted knobs. Widening the gate further was measured and
      REFUSED at ~3% precision; the answer to this class is the habit, not the gate.
- [x] **The `host` ctest label, load-bearing with no gate.** `test_labels` on every board, both
      directions. Classification is DECLARED, because no sound derived discriminator exists:
      `check_oot_export.sh` and `check_oot_export_mcu.sh` share a directory and an argument shape and
      have OPPOSITE classes. An undeclared program is REFUSED rather than guessed. It also found a
      live defect: a per-test `cmake_language(DEFER CALL f "${var}")` expands LATE, so only one image
      test per directory was ever declined since M4.8.2, and no CI job uses the knob so nothing
      noticed.
- [x] **`test_classes.txt`'s own disclosure: a `build` declaration rotted silently.** The `src` half
      was pinned to a file, the `build` half to nothing, and 22 lines carried the gap. Symmetry is
      not the fix -- a `build` path is relative to the BINARY directory and every MCU board
      legitimately has none of them, so `[ -f ]` would red the fleet. CMake now passes `<complete>`,
      yes only where the tree registers the whole build-rooted set (sim arch plus the host unit
      binaries), and there each declaration must be the program of some registered test. Elsewhere
      the sweep stands down, and the header says so: an unrun `build` declaration on a
      `<complete> no` tree is indistinguishable from a program that board does not build, and a
      fleet run that never includes the complete configuration checks the `build` half NOWHERE.
      The wrong-way flag is the safe one -- claiming `yes` falsely names all 16 unit lines at once.
      Mutation-proven both directions: the dead line reds only the complete tree, and a renamed
      binary under a registered test -- which the pre-existing `@none` arm already reddened as
      "build the tree first" -- is now NAMED as the declaration it orphaned.
- [x] **`bench-capture.sh` refused every non-TAP app and the refusal skipped the log FETCH.** A TAP
      verdict is owed only by `selftest*`; `bench.sh` fetches before propagating a refusal, because
      the log is the artifact and the verdict is separate.
- [x] **TWO DIVERGENT FLASH RECIPES FOR ONE BOARD.** Resolved as ASKED rather than patched: the write
      routes through `tools/flash.sh` with `FLASH_TOOL` PINNED to `stlink`, since `candidates_for`
      offers "stlink jlink" for stm32f302 and would silently become a J-Link flash on a host without
      stlink-tools. **And closing it found the bigger thing: the separate `st-flash reset` was the
      f302 "unreliable instrument" all along** -- it cut the correct boot off mid-line and started one
      that stalled. Removed. Witnessed at TAG `m484p2`, gate-verified, and the single-variable control
      is a write-only capture of the PRE-fix image reproducing the complete line byte for byte.
      **The fix lives in gitignored `.session/`, so the repo carries none of it and no gate protects
      it** -- that is the residual hole, filed below.

**A REGRESSION M4.8.4 INTRODUCED, BISECTED ON SILICON, FIXED, AND RE-MEASURED THERE. CLOSED.**
`rr_interleave` fails on `rx72m` under `kickos_services_rx72m_uartirq`. Measured rates, one board,
same app, TAGs `m484rx*`:

| tree | list | result |
| --- | --- | --- |
| `c87f84ed` (master, M4.8.3) | `_uartirq` | 0 of 3 fail -- `rr order: ABABAB` |
| its parent | `_uartirq` | 0 of 3 fail |
| **the creator-hold commit** | `_uartirq` | **3 of 3 fail -- `rr order: AABBAB`** |
| S2 | `_uartirq` | 3 of 3 fail |
| the branch tip | `_uartirq` | 3 of 4 fail |
| the branch tip | DEFAULT | 0 of 3 fail |

The parent is green and the creator-hold commit is red, so the regression is that commit. ISOLATED to
one line by building it with the other line kept: removing `task_orphan_created_by` and keeping
`c->task = nullptr` is green 3 of 3. So the cause is the creator-hold sweep, which is
`KICKOS_MAX_TASKS` (= `KICKOS_MAX_THREADS` + 1, so 17 on rx72m) byte compares MASKED, on EVERY
thread exit.

- [x] **The creator-hold sweep is O(1) on the exits that cannot owe a drop.** `Kernel::task_holds`
      counts the slots carrying a creator tag: `task_create` takes one, `task_drop_hold` returns it
      (the only site that clears a LIVE tag -- `free_task`'s clear is reached from there, already
      NONE, and from `task_release`, which takes that arm only when there was no hold), and
      `task_orphan_created_by` returns at once while the count is zero. That is every production
      driver list and every selftest arm before the task arms, `rr_interleave` included. **The
      footprint is free**: the counter is a `uint16_t` declared before `Kernel::sleepq`, where a
      32-bit target has two bytes of padding in BOTH telemetry postures, so every `microbit` image
      is byte-identical in `.data`, `.bss`, `_ebss` and `__kickos_ram_start` across the change, and
      so is `qemu-telem`. Gated by a seventh `creator_hold` arm that a count-cleared-by-the-sweep
      mutant kills while the other six stay green.
      **This is also a finding about the INSTRUMENT**: only the `_uartirq` list shows it, and only
      on the one board with no emulator and no CI gate, so the default-list fleet pass this
      milestone ran would never have caught it.
- [x] **The `CAP_IRQ` teardown pre-pass is O(1) on the exits that hold no line.** Same class as the
      creator-hold sweep above, same milestone, and it runs on EVERY thread exit: the pass that
      releases a dying thread's IRQ caps before the chunked loop opens its first gap scanned the
      whole table under ONE unbroken `IrqLock`, which is the shape `docs/archive/M4.7.9_teardown_latency_meas.md`
      measured as a real interrupt-latency cost. `Thread::cap_irq_live` counts the `CAP_IRQ`
      entries a thread holds -- `cap_install_at` is the one install funnel (`irq_claim` and the
      spawn grant list both reach it) and `cap_slot_released` the one release funnel, shared by
      `handle_close` and the sweep -- so the pass returns having read no entry while the count is
      zero. **The no-chunking property is untouched**: the loop is still inside the one masked
      window that bumps the teardown depth, which is what both console reclaim sites rely on.
      **The footprint is free**: the counter takes the LAST padding byte before
      `Thread::quantum_ns`, so `sizeof(Thread)` is unmoved at 256 / 264 / 2480 and `microbit`'s
      `.bss`, `.data`, `_ebss` and `__kickos_ram_start` are all unchanged.
      **A per-KERNEL count was rejected on precision**, not on cost: one IRQ driver anywhere makes
      every other thread's exit scan again, and `rx72m` under `kickos_services_rx72m_uartirq` is
      exactly that image. **`c->authority & AUTH_IRQ` was rejected as fail-OPEN**: a `CAP_IRQ` with
      `CAP_TRANSFER` can be delegated at spawn to a child that never held the bit.
      Gated by a ninth `cap_sweep` arm that dates the release against gap 1 rather than against the
      line cap's own chunk boundary; five mutants killed.
- [x] **RE-MEASURED on `rx72m` under `kickos_services_rx72m_uartirq`, TAGs `m484rxfix1..3`:
      0 failures in 3 runs, `rr order: ABABAB` on every one**, matching master. Banner
      a banner with no `-dirty`, and the stream hand-validated through
      `check_tap_stream.sh` at 104 arms, 0 skipped, 0 partial. The bisect that found it and the
      isolation that named the line are above; both stay, because the instrument lesson outlives
      the bug: only the `_uartirq` list showed it, on the one board with no emulator and no CI
      gate, so the default-list fleet pass this milestone ran could never have caught it.

**THE CAP_IRQ PRE-PASS, the last code finding of the ten-angle review, FIXED AND WITNESSED.**
`cap_teardown`'s IRQ pre-pass scanned the whole capability table interrupt-masked on EVERY thread
exit -- the same shape as the `task_orphan_created_by` regression this milestone shipped and fixed,
and its own comment conceded it ("the scan by the table's own width"). It could not simply be
chunked: a gap inside that pass is a moment when a thread with a counted teardown depth still holds
an IRQ line, and both console-reclaim sites rely on that being impossible.
- [x] **FIXED in M4.8.4 with `Thread::cap_irq_live`, a `uint8_t` in the LAST padding byte before
      `quantum_ns`**, so the pass reads no entry when it is owed none. `sizeof(Thread)` unmoved at
      256/264/2480 and `microbit`'s `.bss`, `.data` and both arena symbols byte-identical.
      Rejected, with reasons: a kernel-wide count (one binding anywhere makes every other exit scan
      again -- and `rx72m` under `_uartirq` IS that image, so it would not fix the one board that
      matters); `authority & AUTH_IRQ` as the predicate (FAIL-OPEN and unsound -- `irq_claim`
      installs with `CAP_TRANSFER`, so a child that never held `AUTH_IRQ` can hold the cap); and a
      sticky bool (`handle_close` cannot clear it without knowing whether another line remains).
      **WITNESSED**, TAG `m484capirq` on `rx72m` under `kickos_services_rx72m_uartirq`
      -- the one reachable board where `CAP_IRQ` entries genuinely exist: `1..104`, 104 ok, 0 skip,
      0 partial, 11 IRQ arms present, `rr order: ABABAB`, banner clean, and the banner's `app`
      tracking `build` so the cached-`SERVICE_LIST` trap did not fire. Stream hand-validated.
      The no-chunking property is machine-checked: the gap-dating arms count 4 gaps, and 12 under a
      chunk-the-pre-pass mutant.

**D -- COVERAGE, TEST-INFRA, DOCS, PERF, FUTURE (17).** Ordinary backlog. Includes the whole M6/SMP
group and every "no arm for this yet" item. Added while closing A, B and C -- all genuinely class D,
which is the only reason they are filed rather than fixed:
- [ ] **The f302 capture-protocol fix is untracked.** `.session/bench-capture.sh` is gitignored, so
      the one repo-visible trace is a comment in `tools/flash-stlink.sh`. Nothing gates it and a fresh
      checkout does not have it. Either the bench scripts become tracked tooling or the knowledge
      stays a comment plus `CONTEXT.local.md`.
- [ ] **The `kickos_terminate` device drain has NO witness.** Contract and plumbing only, argued from
      the seam. `arch_console_flush_sync` has a body on `mk64f` and `xmc4800` only, plus `stm32f302`
      now, so on every other board the drain is still a no-op and the terminal path can still cut its
      last line. Per-chip bodies are fleet work.
- [ ] **`kernel/bench/bench.cc`'s `s_*` fix is gated but unwitnessed under `KICKOS_BENCH`** -- the
      gate is a source scan, and no bench image was run.
- [ ] **`task_cancel_group` and the endpoint drain publish twice under one `IrqLock` from a LIVE
      thread**, the same residue as the `sched::wake` supersede but with no dying guard involved, and
      more reachable. Not filed anywhere before.
- [ ] **`teardown_depth` now has one production consumer and one fixture consumer**, so the
      `cap_teardown_active()` gate in `exit_current` is premise-less. Kept as conservative retry
      throttling; revisit when the DEV-window respawn gate lands.
- [ ] **`console_crlf` now also gates console write POLICY** and should be renamed; deferred
      deliberately to avoid fleet record churn mid-milestone.
- [ ] **No reply-bearing flush op**, so the default init cannot read the residual `flush()` returns.
- [ ] **CDC console throughput is ~167 B/s** under many small writes (2008 bytes in 12 s). A console
      you cannot read a suite through is marginal.
- [ ] **Blocking console mode makes the fault-record route LOSSY while the console is wedged**: a
      driver sitting in an unbounded `push_all` is not in `kos_recv`, so `cap_console_deliver` finds
      no parked receiver and drops the record. Bounded on a UART (a guaranteed consumer), which is
      why USB REQUIRES non-blocking and refuses `-KOS_ENOTSUP` to clear it.
- [ ] **`docs/design-m4.8.2-host-unit-tests.md:435` says the K-seam is sixteen symbols; it is
      eighteen**, ten of them `arch_*`.
- [x] **`invariants.md`'s nine `file:line` citations are verified WRONG**, and each needs the fact
      re-located rather than the number nudged. `doc_names` never checks a line number.
- [x] **"per-task" was a SECOND TRUTH across the whole Reference tier, not six sentences in one
      file.** The filed count was six in `architecture.md`; it was three there, and the phrase had
      also reached `porting.md` and `boards.md`. Converted against the code rather than by search
      and replace, because the right word differs per site: the ADDRESS SPACE is a `Domain`, while
      `Thread::caps` and `Thread::quantum_ns` are per-THREAD, so `KICKOS_CAP_TABLE_SUPPLY` and the
      RR quantum are per-thread and the MPU/enforcement statements are per-domain. What stays
      "per-task" is microC/OS-III's own feature name, which is theirs to spell.
- [x] **The M4.5.1 audit ledger said a deleted knob "defaults ON" in the present tense, and the
      file is now DELETED rather than corrected.** Holding ~1,100 finding rows to the Reference's standard was
      never going to be scheduled, and the alternative -- a document nothing checks, asserting false
      things in the present tense -- is worse than not having it. Findings and their closing commits
      are regenerable from a fresh pass and from git
      (recoverable from git at `b27a409e`), which is what separates this from
      `docs/archive/`, where the content is unrepeatable measurement. The gate's `docs/` exclusion
      STAYS: it guards the class, not that one file.
- [x] **The design record for kill/slay is landed: `docs/design-kill-and-slay.md`** -- the ABI, the
      mechanism, the reaper as the REJECTED alternative, and six open questions each with a
      recommendation. **`doc_names` cannot express "an identifier this document PROPOSES"**, so the
      three names S3 and S4 introduce live in a fenced block (the gate skips fences) and the prose
      describes them instead. That is a limit of the gate, not of the doc.

## Found landing the M4.8.2 host unit-test layer (2026-08-11)

The layer's record is `docs/design-m4.8.2-host-unit-tests.md`; section 8 is what landing it found.
Items 5 to 7 of its section 7 are still owed and are the ones below plus the migration.

- [x] **FIXED in M4.8.2. `tests/static/check_class_backend.sh` covered the driver classes only, and
      the SYSCALL set was protected by nothing but `user/src/syscall_stubs.cc` being one archive
      member** -- a split per subsystem, an ordinary refactor nobody would flag, and the failure
      goes silent. The second argument is now a `;`-separated list of header directories and carries
      `user/include/kickos` and `user/include/kickos/sys` as well: **11 declared symbols became 82**,
      of which 56 to 62 are defined per image. Escaped as `\;` in `add_test`, because CMake splits an
      unescaped one into separate arguments and every positional after it shifts.
      Killed by feeding the gate an object defining `kos_clock_now`: leg 1 names it against
      `syscall_stubs.cc.obj`. Note that mutating an app TU instead does NOT test this -- the link
      collides first and `kickos_build` fails before the gate runs, which is the old protection, not
      the new one. Green on seven presets; `rx72m` reports 57 defined rather than 0, so the RX
      underscore-prefix leg is live.
- [x] **The blocking-call trap in the K-seam fixture needs a mechanism, not a paragraph.**
      **DONE:** a switch whose OUTGOING thread is `BLOCKED` is a park, so `note_switch` poisons
      `wait_result`, credits the switch-in `wq_confirm_resume` spins for, and calls the waker armed
      by `wake_next_park` -- or ends the arm with a `FIXTURE FAIL:` naming the thread, asserted by
      `KICKOS_EXPECT_FIXTURE_REFUSAL`. `tests/unit/parkresult/` drives the real `mutex_lock` and
      `sem_wait`, 6 arms, 7 mutants all killed
      (`docs/design-m4.8.2-host-unit-tests.md` section 8.5). `g_resume_on_switch` deleted with it.
- [x] **The concurrent capability-teardown path now has an instrument and still has no arm.**
      **DONE:** `tests/unit/capsweep/` puts a second dying thread's whole sweep inside the first
      one's chunk gap, found by the nesting count `arch_irq_save`/`arch_irq_restore` now keep, and
      asserts the ordered trace across the boundary. 3 arms, 5 mutants all killed; `outer-live` is
      the arm that says the depth is a COUNT and not a flag (section 8.6).
- [ ] **The K-seam fixture's OTHER self-diagnostics have no death case, and now there is a macro for
      them.** `KICKOS_EXPECT_FIXTURE_REFUSAL` (`tests/unit/kfixture/kseam_test.h`) gates the
      no-waker refusal; the same shape would gate `reset()`'s in-flight-sweep refusal, `note_park`'s
      "parked with no arm waiting for it", `kickos_terminate` and the range checks in `spawn` /
      `seat_pool` / `task`. Each is an `exit(1)` a mutation cannot currently be caught by, and the
      `reset()` one needs an arm that deliberately abandons a sweep, which is the entry below.
- [ ] **`tests/unit/kfixture/reset()` cannot clear `g_cap.teardown_depth`**, because `cap.cc` keeps its
      `CapState` in a TU-local `constinit` that the `kernel() = Kernel{}` assignment does not reach.
      It refuses loudly instead (`cap_teardown_active()` at the top of `reset`), so an arm that
      abandons a sweep stops the suite rather than poisoning every later arm. Widening
      `cap_slab_init()` to zero the depth was considered and REFUSED: no arm needs it, and widening
      shipped code for a fixture's convenience is the wrong direction. Revisit only if an arm
      legitimately needs to abandon a sweep.
- [x] **A guard that is usually redundant needs an arm for the case where it is not.** The M3 mutant
      (delete the `dying` clause outright) SURVIVED the gate's first version, because the guard's
      effect on an equal-priority peer is a `pick_next` call avoided rather than a switch avoided,
      and only a ready list whose head is NOT the dying thread makes the decision observable. Worth
      applying to every other early return that reads as an optimisation: gate the decision, not the
      usual outcome.
- [x] **`cap.cc`'s `g_stdout_target` is a THIRD TU-local the K-seam fixture cannot reset**, and
      unlike `teardown_depth` it cannot even be detected: `cap.cc` exports no reader. So
      `tests/unit/kfixture/kfixture.h` note 4 carries a "no arm may call `cap_console_publish`", and a
      comment is a stopgap, not a fix. The failure it prevents is nasty: `reset()` zeroes the
      endpoint pool, so gen-encoded handles REPEAT across arms, and a stale global handle can be
      matched by a later arm's unrelated endpoint close, noting a console death in a DIFFERENT arm's
      counter. Fix when an arm needs to publish: a reset entry point beside `cap_slab_init()`.

## Found by the M4.8.2 ten-angle review (2026-08-11)

Five reviewers over ten angles, against `b77a3ef4`+`a2695e08`. No Critical: no isolation or
memory-safety escape was constructible from the new mid-sweep preemption, and every protection
`design-m4.8.2-host-unit-tests.md` section 8.2 relies on was verified independent of the guard. What
follows is what survived that.

- [ ] **`sched::wake`'s guard reads a `kernel().current` that a deferred switch has already moved.**
      `switch_to` publishes `kernel().current = next` BEFORE `arch_switch`, and on ARM, RISC-V and RX
      that only pends. So after one admitted wake inside a sweep the dying thread keeps running with
      `c` naming the PEER, and every later wake in that chunk sees `dying == false` and
      `state == RUNNING`: both new clauses are dead and `reschedule()` runs unconditionally. No wrong
      final state was constructible, because the EPIPE drain pops in DESCENDING priority so
      `pick_next` returns the already-published peer and the extra `reschedule()` early-returns. The
      residue: the guard's stated premise is defeated for later wakes, and two arms in ONE chunk
      waking ASCENDING priorities would run `on_switch_in`/`arm_slice` for a peer that never runs, so
      an RR peer forfeits part of its first quantum. The gate cannot see it (the stub also returns).
- [x] **Two name-keyed releases are now observable as not-yet-done by the peer the sweep itself
      wakes.** A supervisor EPIPE-woken by a dying driver's endpoint arm, at higher priority, now
      runs BEFORE the `CAP_IRQ` slot is swept, so its respawn's `kos_irq_claim` gets `-KOS_EBUSY`;
      and `console_on_driver_death` is gated on `cap_teardown_active()`, so a preempted sweep
      postpones the console reclaim for as long as the peer keeps the CPU, which is unbounded.
      `domain.h` makes exactly this promise for the DEV window and pays for it with the early
      `domain_release`; the IRQ line and the console got no equivalent. A READING: no in-tree arm
      hits it, because `irq_reclaim`'s worker runs ABOVE root so the woken peer is lower-priority.
      Needs a supervisor above its driver, which is a normal posture with no in-tree instance.
- [x] **`sched::wake` will resurrect an EXITED thread.** The early return covers READY and RUNNING
      only, so `wake(t)` with `t->state == EXITED` sets it READY and pushes it onto the ready list,
      after which `ThreadPool::alloc` no longer sees the slot as free. Unreachable today (an exiting
      thread is on no queue and carries `WAIT_NONE`), and worth closing now that `state == EXITED` is
      load-bearing two lines below.
- [x] **The `teardown_depth` axis is ungated end to end.** Deleting BOTH the increment and the
      decrement so `cap_teardown_active()` is permanently false SURVIVES the suite, and so does
      making `console_on_driver_death` unconditional, and so does deleting the fixture's own
      in-flight-sweep refusal.
      **DONE for the first two by `tests/unit/capsweep/`**: the depth-deleted mutant and the
      unconditional-reclaim mutant are both killed by named arms, and so is a depth that is a FLAG
      rather than a count. **The fixture's own in-flight-sweep refusal in `reset()` is still
      ungated**, and gating it needs an arm that deliberately abandons a sweep, which is the
      `reset()` entry below.
- [x] **The `host` label is now LOAD-BEARING, and still has no gate.** Re-derived with
      `git grep -n "LABELS host" -- '*.txt' '*.cmake'`: 11 literal sites (2 in the root
      `CMakeLists.txt`, 1 in the shared `kickos_discover_unit_tests` in `cmake/kickos.cmake`, 1 in
      `tests/unit/captable`, 2 in `tests/unit/telemetry`, 5 in `user/apps/common/selftest`), plus 6
      more test targets that inherit the label indirectly through `kickos_add_unit_test` /
      `kickos_add_kseam_gate` -- not the stale eighteen. Since M4.8.2 the root `CMakeLists.txt`
      defers `kickos_decline_image_test`, which sets `DISABLED TRUE` on any test NOT labelled
      `host` when `KICKOS_BUILD_INTEGRATION_TESTS=OFF`. Both directions are now silent: a host
      test missing the label gets disabled under that knob, and an image test wrongly labelled
      `host` carries `host` in `_labels`, so `kickos_decline_image_test`'s `NOT "host" IN_LIST`
      check is false, it is never disabled, and it keeps running under a knob meant to skip it.
      `ctest -L host` silently shrinks and
      `ctest -LE host` silently grows on the next test added either way, which is exactly the
      mechanicalness the label was supposed to buy. A `check_labels.sh` over
      `ctest --show-only=json-v1` is a few lines.
- [ ] **`f302nucleo`'s skip and partial sets are declared nowhere in the tree.** `microbit` is the
      only board whose expectations are stated (`user/apps/common/selftest/CMakeLists.txt`), so a
      hand-run of `check_tap_stream.sh` on f302nucleo has to be handed sets taken from the log being
      judged, which is self-confirming. Declare them the way microbit's are.
- [ ] **The K-seam fixture only ever compiles the SIM posture.** It takes `kickos_kernel`'s
      `COMPILE_DEFINITIONS` verbatim and registers only under `KICKOS_ARCH STREQUAL "sim"`, so a
      SEGMENTED capability table (`KCAP_RUN_CHUNKS > 1`, which `frdmk64f` runs), `KICKOS_DIAG_TERSE`,
      and every non-sim cap geometry never reach a K-seam arm. That compounds with the chunk-boundary
      arm: flat-versus-segmented teardown IS a chunk-boundary property.
- [ ] **Splitting `user/src/syscall_stubs.cc` per subsystem breaks the U-seam shadow protection
      silently, and this tree has a PRECEDENT for doing exactly that.** `user/CMakeLists.txt` already
      splits `sim_exit.cc` and `newlib_sbrk.cc` into TUs of their own precisely to control extraction
      granularity, so the refactor is the file's own idiom rather than a hypothetical. Secondary
      condition nobody has stated: the collision is loud only while the image references at least one
      symbol from that member the shadow does not define, which is true of `selftest` and not
      established for a minimal image.
- [ ] **Swapping `Thread::privileged` and `Thread::dying` would save a load on EVERY wake, free.**
      Measured on armv7em: `state` is at offset 67 and `dying` at 69 with `privileged` between them,
      so no single `ldrh` covers both and the guard pays two byte loads. Both are `bool` and both sit
      in the same padding band the header already documents as free, so the swap costs no footprint
      and the `sizeof(Thread)` asserts would catch a mistake.
- [ ] **A K-seam gate cannot register a fixture refusal, and cannot print `not ok` when passing.**
      `kickos_add_kseam_gate` now sets `FAIL_REGULAR_EXPRESSION "not ok"` unconditionally, which is
      what stops a forged exit code, and the cost is that the fixture's own refusals cannot be gated
      through that function and no future gate may quote a TAP-shaped expectation in a diff printer.
      An `EXPECT` parameter that replaces the fail regex would cover both.
- [x] **The `PANIC` branch of `kickos_add_kseam_gate` has no caller.** Its `PASS`+`FAIL` interaction
      was verified out of tree and is correct; in tree it is untested. Either the first death-case arm
      lands (the ISR-context `kpanic` the fixture header promises is the obvious one, and
      `arch_in_isr` is already wired to `g_in_isr` for it) or the branch goes.

## Found porting fault isolation to rxv3 (2026-08-12)

- [ ] **`kickos_fault_below_stack` NARROWS WHAT A FAULT REPORT CAN ATTRIBUTE ON rxv3, and design 4.3's
      reaper is what removes it rather than tuning it.** RXv3 cancels the faulting instruction and
      restores SP (ISA UM sec.5.3.1), so no SP-based test can see a stack overflow and the faulting
      ADDRESS is the only evidence. The test is EXACT for the overflow class -- a stack grows down, so
      the first denied access is beneath the base by construction -- but its converse is not, and the
      cost is MEASURED on one tree: `mpu_fault`'s cross-domain write to `0x13200`, below `domainA`'s
      stack, escalates to the panic dump instead of dying alone, while `rxdrv`'s `0x8c068` above the
      stack dies alone. So on this board an unprivileged operand access to any LOWER address is
      reported as a system panic and the thread is not credited with dying alone. **Do NOT replace the
      stack base with a distance threshold**: a frame larger than the threshold puts privileged code
      back on an exhausted stack, which is the UNSAFE direction and is the exact defect this closed
      (`.session/logs/m483rxovf-*`, which reached `PC=0x0`). The real fix is a stub that never runs on
      the dying thread's stack, i.e. `docs/design-m4.7.9-fault-isolation.md` section 4.3, which
      already recorded that it "would survive 4.2" and is now measured on an ISA that needs it. It
      would delete the rxv3 test, not tune it.

- [x] **An anonymous namespace nested inside `extern "C"` emits UNMANGLED GLOBAL symbols, and
      `arch/rx/rxv3/arch_rxv3.cc` already does it.** Measured with `rx-elf-nm` on a linked image:
      `_g_pend_regions` and `_g_pend_count` (the MPU deferred-commit stash, in a `namespace { }` inside
      the file's `extern "C"` block) are `B`, i.e. global, while `g_in_isr` in the file's TOP-level
      anonymous namespace mangles to `__ZN12_GLOBAL__N_18g_in_isrE` and is local. C language linkage
      wins over the anonymous namespace, so the namespace reads as internal linkage and is not. Two
      RX chips or a second backend touching those names would collide at link. This port used an
      explicit `static` for its own helper to sidestep it; the pre-existing pair was left alone
      because renaming them is not this change. Audit the other backends for the same shape.

- [x] **`bench-capture.sh` REFUSES any app that is not the TAP selftest, and its refusal skips the
      log fetch.** `faultsurvive`, `mpu_fault` and `rxdrv` all end with "has no plan line at all: the
      suite never announced itself", `bench.sh` exits before its `rsync`, and the capture -- which is
      complete and correct on the bench host -- has to be fetched by hand. Every fault witness in this
      pass was collected that way. The refusal is right for a selftest run and wrong as a blanket
      rule; it needs to key on whether a plan line was EXPECTED. (`bench.sh`'s sibling-image search was
      fixed in the same session: `faultsurvive_ovf`/`_off` are declared by another directory's
      CMakeLists, so neither the `<app>/<app>` convention nor the `_p<N>` strip could find them.)

- [ ] **On a FLAT rxv3 board nothing catches a wild SP that faults with a non-MPU cause.** The
      below-stack test is inside `#if KICKOS_HAVE_MPU` because it reads `MPESTS`/`MPDEA`, and the USP
      containment test only sees an SP that is out of range, not one that is in range with no room
      below. So on `rx72m-flat` an address exception taken by a thread whose stack is exhausted would
      be killed and the stub would run on it. Not witnessed either way -- the flat posture has no arm
      that reaches this -- and it is the same hole 4.3 closes. Filed so the enforcing-only scope of the
      guard is on the record rather than implied by an `#if`.

## Found landing task-layer step 9.3 (2026-08-11)

- [x] **A PUBLISHED driver console SWALLOWS the thread-fault RECORD, and the boards it hits are the
      ones whose default service list carries a driver.** FIXED in M4.8.3 by a ROUTE, not a reclaim:
      the five record sites call `kprintf_fault`, which while the console is `USER_OWNED` also hands
      the line to the published endpoint's already-parked receiver (`cap_console_deliver`), so the
      DRIVER prints it and the healthy driver keeps the device. Ruling, the four options rejected and
      the two things the fix still loses: `docs/design-m4.7.9-fault-isolation.md` section 9.5. Gated
      on the host by `sim_published_panic` case 2 and the new `sim_faultsurvive_published` (the
      ordering claim), and witnessed on silicon under DEFAULT driver-carrying service lists.

- [ ] **`microbit` has ZERO arena slack, so no `.bss` addition is ever inert there again.**
      `__kickos_ram_start` IS `_ebss` and the granule is 32 bytes, so four bytes cost as much as
      thirty-two. Step 9.3's task pool took `mem_self_grant`'s last `kos_ram_alloc` grain and that arm
      is now a declared skip. The structural options, none taken: shrink the board's static demand,
      give the selftest a microbit-specific arena reservation the way `uart_service` got one, or
      accept that this board's skip set grows once per milestone. Worth deciding deliberately rather
      than one arm at a time, because the next `.bss` byte from ANY change takes the next arm.
- [x] **`docs/reference/invariants.md` carried the invariant ID `object-access-via-per-task-cap` while
      the capability table is per-THREAD. FIXED, AND THIS ENTRY OUTLIVED THE FIX.** The ID is
      `object-access-via-per-thread-cap` now, the spelling `object-access-via-per-task-cap` appears
      nowhere in the file, `handle-not-pointer-across-boundary` cites the new ID, and the prose reads
      "every SPAWNED thread gets `KICKOS_CAP_CHILD_WIDTH`" with "the width is per thread, not one
      number per image" beside it. That is every clause this entry asked for.
      Closed 2026-08-28 at the M6.3 documentation audit.
- [x] **`task_release` freed the task slot BEFORE `cap_teardown`, so `c->task` dangled for the rest
      of `exit_current`. FIXED, AND THIS ENTRY OUTLIVED THE FIX.** The release still precedes the
      sweep, for the reason this entry gives, but the NAME is retired with the reference:
      `kernel/sched/sched.cc` clears `c->task` immediately after `task_release(c->task)`, so the
      window in which a peer's `task_for` could recycle that slot while `c->task` still named it no
      longer exists, and a second `task_release` would take a null rather than a live task.
      Closed 2026-08-28 at the M6.3 documentation audit.
- [ ] **M6/SMP: the claim-then-commit shape in `domain_for` and `task_for` is safe only because
      `IrqLock` is enough on one core.** Both hand out a pool slot at refcount 0 and are committed by
      a later `domain_ref`/`task_ref`, and what makes the window atomic is that `thread_create_call`
      declares a FUNCTION-SCOPE `IrqLock` as its first statement, spanning the claim, the thread-slot
      alloc and `thread_create`. `IrqLock` masks LOCAL interrupts only, so under SMP a peer core can
      claim the same slot: two threads would then share one task and the loser's domain would sit at
      refcount 0 as a free slot while a live thread names it. Verified NOT reachable today (no second
      `IrqLock`, no `arch_irq_restore` and no `reschedule()` between the claim and the commit; the
      nested lock in `assign_thread_id` restores "masked" rather than "enabled"). Pre-existing in
      `domain_for` and inherited unchanged by the task layer, so it belongs to the SMP work and not to
      M4.8.3. The neighbouring comment about snapshotting `p->caps[ci]` is a DIFFERENT seam, about
      user-memory TOCTOU, and is not evidence either way.

## Found landing the fault-record route, M4.8.3 (2026-08-12)

- [x] **`check_faultsurvive.sh`'s corroboration table misses TWO of the fleet's enforcement classes,
      and one of them it would actively MISJUDGE.** The `arch:arm` switch covers only
      `armv7m:overflow`, `armv7m:offstack`, `rv32imac:overflow` and `rv32imac:offstack`.
      - **`rxv3` is absent entirely**, so `rx72m` -- the one board that has no emulator and no CI gate
        -- hits `fail "no corroborating evidence is defined for arch 'rxv3'"` and every escalation
        verdict for it is a human reading a capture against the same reasoning the script encodes. The
        evidence exists and is per-arch, measured at `m484`: `MPU FAULT: thread 'faulter' attempted
        write at 0x121fc` for `overflow`, and `=== RX EXCEPTION (privileged instruction) ===` with
        `PSW=0x130001` (PM set, so user mode) for `offstack`.
      - **`armv7m:overflow` assumes the ARM MemManage unit and so does not fit `frdmk64f`.** It
        requires `CFSR & 0x10` (MSTKERR), which `xmc4800-relax` does produce (`CFSR=0x92`). The K64F's
        SYSMPU is a BUS-level slave-port MPU, so the same stacking abort latches BusFault `STKERR`
        (bit 12) with `IMPRECISERR` -- `CFSR=0x1400`, MSTKERR CLEAR -- and the real evidence is the
        `SYSMPU ISOLATION FAULT: port=3 addr=... W` line. The arm would FAIL on a correct capture. It
        is latent only because no ctest runner points this gate at a SYSMPU board; the class needs its
        own corroboration, keyed on the MPU backend rather than on the arch.
      Both are `case` arms, no new mechanism, and blocked on nothing.
- [x] **`docs/design-c6-driver.md` was reported as citing an "open question 3" it does not carry.
      NOT A DEFECT, AND THIS ENTRY IS A STALE DUPLICATE OF ONE ALREADY SETTLED ABOVE.** That document
      DOES carry a numbered `## Open questions / risks` section, and its item 3 is the timer-IRQ
      follow-on the citation points at, so the citation resolves and the parenthesis stays. The same
      false finding was investigated and closed in the doc-audit section of this file; this entry was
      filed against the same premise and never re-checked. The `check_sim_pubpanic.sh` half of it was
      a separate and real defect, fixed at M4.8.3 by pointing at the numbered question that exists.
      Closed 2026-08-28 at the M6.3 documentation audit.

## Found landing task-layer steps 9.4 and 9.5 (2026-08-12)

- [x] **An explicit task's slot and its domain are held until `kos_task_kill`, and NOTHING sweeps them
      at the creator's death. CLOSED IN M4.8.4, and re-read as an AUTHORITY ESCAPE rather than a leak
      -- see class A of the triage section above, which also records why the third candidate below is
      the escape itself.** `task_create` takes a hold that only `task_drop_hold` releases, and the
      one caller of that is the kill syscall. Root is the creator in every in-tree case and root's
      death ends the system, so nothing leaks today; a supervisor thread that creates groups and then
      dies reserves those slots for the rest of the run. Three candidates, in the order they compose:
      a task-pool sweep in `exit_current` keyed on the creator tag, which is the shape
      `ThreadPool::alloc` already uses for the spawner tag and for the same reason (a recycled slot
      must not inherit authority over what its predecessor created); a `kos_task_release` that drops
      the hold without ending the group, which a hand-over would want anyway; or declaring the hold to
      be the creator's for life. Open question 6 of `docs/design-task-layer.md`.
- [x] **`rx72m/rxsci`'s relay thread now SEES the ring block it declared no grant for. RULED IN
      M4.8.4: the third memory scope is refused and the per-thread flag is deleted rather than
      honoured** -- class B of the triage section above has the reasoning and the residual.
- [ ] **A cancelled thread that never re-enters the kernel is still unreachable.** The death point is
      the syscall entry, so a pure compute loop survives a `kos_thread_kill` and a `kos_task_kill`
      indefinitely. Preemptive cancellation is the only fix and it is a much larger change: it needs a
      point at which one thread may run a stranger's `cap_teardown`. No in-tree thread shape has this
      problem (all of them loop through a syscall), so this is a documented floor rather than a live
      gap -- but it is the reason 0 from either call means ACCEPTED and not GONE.
- [ ] **"Release the DEV window BEFORE the capability sweep" has NO gate, and never had one.**
      M22 and M25 in the 9.4/9.5 mutation run (`docs/design-task-layer.md` 8.3) are this entry,
      filed rather than killed. Measured by mutation at the 9.5 tree, both ways. `dev_window_free`
      skips a `dying` thread so a supervisor woken by the sweep's EPIPE can respawn into the
      window at once; deleting that arm
      leaves every suite green on `sim`, `qemu` and `qemu-riscv`. So does the ORDERING it preserves:
      moving `task_release` from before `cap_teardown` to after it is equally invisible. The second
      result is what says this is INHERITED and not something 9.4 introduced -- before it, the same
      exclusion came from the domain reference being dropped at the top of `exit_current`, and that
      was untested too. What a gate needs is the full choreography: thread A holds window W, A exits,
      and DURING A's teardown the EPIPE wake reaches a supervisor that respawns into W and must
      succeed. `sim_driver_death` case 3 has every piece except the respawn. Until then the comments
      at both sites are the only thing holding the invariant, which is why they say so.
- [ ] **Nothing witnesses the DEATH POINT on silicon.** `tests/unit/taskdeath` gates that a peer is
      marked and made runnable, `sim_driver_death` and the `task_group_kill` selftest arm gate that it
      then dies -- all under a host or emulated clock. The interesting case is a driver cancelled while
      its IRQ line is armed on real hardware, where the wake races the device. Wants an enforcing board
      with an IRQ UART service list, which is what makes it a fleet-pass item and not a gate.

## Found during M6.3 and M6.4, deferred with the reason, then fixed at the pre-audit step (2026-08-28)

- [x] **DONE: the word-copy bound is exact, and an aligned 8-byte `kmemcpy` costs 32 instructions
      instead of 56.** `lib/libc/string.cc` no longer gates the copy word path on a conservative
      `2 * WORD_BYTES`. The byte prologue's length is fixed by the destination alignment and is
      therefore known before the branch, so `copy_ascending` enters on `n >= ((-du) & WORD_MASK) +
      WORD_BYTES` and `copy_descending`, whose prologue walks the END of the range DOWN rather than
      the start UP, on `n >= (du & WORD_MASK) + WORD_BYTES`. Measured on rv64imac at `-Os` by
      executed-instruction count under qemu: aligned `kmemcpy` of 8 bytes 56 to 32, aligned
      descending `kmemmove` of 8 bytes 61 to 42, every other shape +2 for the wider guard, and the
      emitted opcode stream is identical after the guard.
      **`memset` deliberately KEEPS the conservative `2 * WORD_BYTES`**, now named `MEMSET_WORD_MIN`
      and documented as a profitability threshold rather than a safety one. Its word path has to
      build the fill word first, and measurement says one aligned word costs 2 more instructions
      than the byte loop on rv64imac and 12 more on rv32imac. Two words is where it starts paying.
      **The obvious "just lower the constant" was measured and rejected.** `n >= WORD_BYTES` is
      safe, and is 2 cheaper than the exact bound wherever the exact bound agrees with it, but it
      admits copies that cannot move a whole word: at a shared misalignment of 1 to 7 with `n` of 8
      it pays the prologue, moves nothing, and costs 70 instructions against the 56 the byte loop
      costs today. The exact bound never regresses any shape by more than the 2 instructions of its
      own guard.
      The unit sweep in `tests/unit/kstring/kstring_words.cc` is what gates it, and mutation says it
      is load-bearing: dropping any of the three thresholds below the prologue length kills the run
      with SIGSEGV inside that primitive's own sweeping test. It is also blind to one thing worth
      knowing, that handing `copy_descending` the ascending bound stays green, because that error is
      merely wasteful and never unsafe. Only the instruction count catches it.

- [ ] **OWED MOVE: the three byte-seam DEFINITIONS are in the syscall layer while their declarations
      are in the memory layer.** `kaccess_from_user`, `kaccess_to_user` and `ep_copy` are declared in
      `kernel/include/kickos/aspace.h` (lines 46-52), which is where R6 moved them from
      `kernel/syscall/syscall_internal.h`, but they are still defined in
      `kernel/syscall/syscall_mem.cc` (lines 387-408) over a file-local `access_copy` (line 307).
      They arguably belong in `kernel/mem/aspace.cc` beside the rest of the address-space code, and
      the move did not happen only because another agent owned that file during the pass. No
      behaviour depends on it; it is a layering tidy-up with one caller-visible consequence, that a
      reader following the header lands in the syscall directory.

- [ ] **RESIDUAL: `KICKOS_RV64_SSTATUS_SUM` is still defined and is now used by nothing.**
      `arch/riscv/rv64imac/include/kickos/arch/rv64_frame.h` line 95 defines it; R6 removed both
      writes and the assembler `.equ`, and a tree-wide grep finds no other reference. A dead constant
      naming a bit the port has decided never to set is the kind of thing a later edit picks up and
      uses, so it should either go or carry a one-line comment saying the port never sets it and why.
      Left alone here because the file was not this pass's to edit.

- [x] **`kfault_terminate` bypassed the console flush on every board whose panic ends in a
      shutdown, so a panic on silicon with a transmit FIFO could truncate the dump.** `kpanic`
      reaches `kfault_terminate`, and `virt_rv64`, `virt_rv32`, `virt_arm64`, `mps2`, `nrf51` and
      `sim` all defined it as a bare `arch_shutdown(status)`. SIX chips, not the five this entry
      first named: `sim` was missed, and it is the one every host-side gate runs on. Only
      `kickos_terminate` and a clock retune called `arch_console_flush_sync`. Found at M6.3's R1.4,
      which gave the RISC-V chip a real flush body after measuring that the fallback ASSERTS the
      console cannot outrun shutdown, which is false of any 16550 with a FIFO; `virt_arm64` has had
      the body since S4 and the panic path did not use it either.
      **FIXED at the M6.3 pre-audit step: one `arch_console_flush_sync()` per chip ahead of the
      shutdown, which is what the ordered exit has done since S6.** The per-board answer is what
      made this safe rather than a one-line edit. Only TWO of the six can truncate anything:
      `virt_rv64` (NS16550A, TEMT poll) and `virt_arm64` (PL011, BUSY poll) carry real bodies. The
      other four take the lone-TU fallback, and there the fallback's assertion is TRUE by
      TRANSPORT rather than by luck: `virt_rv32`, `mps2` and `nrf51` publish through semihosting
      `SYS_WRITEC`, which the host consumes inside the trap, and `sim`'s sync writer loops on
      `write(1, ...)` until the host has taken every byte. There was never a runtime assert to
      fire either way: the fallback body is EMPTY in the linked image, `bx lr` on both ARM boards
      and `ret` on `virt_rv32` and `sim`, supplied by `arch_console_flush_sync_default.cc.obj` in
      every case. The RING needed nothing: `kpanic_enter` sets `g_console_panicking`, which routes
      every panic-time write through `arch_console_write_sync`, so only the DEVICE was ever left
      loaded. **Still unwitnessable on QEMU**, whose UART hands each byte to its chardev on the
      register write and reports the shift register idle with it, so the fix rests on the 16550
      TEMT and PL011 BUSY semantics and not on a bench capture. What IS witnessed, by mutation on
      `qemu-riscv64` with the same instrument compiled into all three arms: the flush is REACHED on
      the panic path (both markers, exit 132), deleting the one call makes it unreached (neither
      marker) with the banner and the status unchanged, which is exactly how the defect stayed
      invisible, and with the device mutated never to report idle the loop still terminates on
      `UART_POLL_BOUND`, so the new call cannot turn a fault into a harness timeout.
      Two OTHER terminal `arch_shutdown` sites were swept and deliberately left: the
      `arch_shutdown(0)` after `kmain` in every `Reset_Handler`, and `sim.cc`'s `arch_shutdown(2)`
      after `kickos_isr_fault`. Both run only when a noreturn path returned, so a drain there would
      state a reachability the code denies.
- [x] **`check_dash_punct.sh`'s separator exemption could be forged from inside prose.** Its
      command position anchors on `^`, `;`, `&`, `|`, `(` or `$(`, so a comment such as
      `# see: rm -rf; ls --color -- and then the prose` was exempted: the `;` creates a command
      position and `rm` is on the whitelist. Found while widening the option run to admit long
      options at M6.3, whose adversarial pass proved the widening opened no new class, this being
      the pre-existing one it leaked identically for short options. Nothing in the corpus, 886
      files at this tree, ever hit it.
      **FIXED at the M6.3 pre-audit step, and NOT the way this entry proposed.** Narrowing the
      ANCHOR was measured and rejected. The candidate narrowing required the operand after the
      `--` to start with a quote, a `$`, or the end of the line, and it fails twice over. It
      breaks two legitimate corpus lines, `check_service_lists.sh:71` and `:153`, which spell
      `git ls-files -- CMakeLists.txt '*/CMakeLists.txt'`: a real terminator with a bare-word
      pathspec, and quoting a literal to satisfy a gate is the rewrite-of-working-shell the gate's
      own header names as the wrong fix. An operand-shape survey of all 34 real terminators in the
      corpus is why: 19 sit behind a double quote, 13 behind a single quote, 2 behind a bare
      letter, and NONE behind a `$` or an end of line. And it kills only that one forge:
      `# see: rm -rf; ls --color -- "and then" the prose` survives it by two characters.
      **What landed instead is a POSITION test. SEP reads shell and a comment is not shell, so the
      separator erase now reaches only as far as the line's first comment opener.** `SEP_ERE` and
      its three mutation variants are textually UNCHANGED, so the counts keyed on them did not
      move. The scanner gained a fifth ERE, `HASH_ERE='(^|[[:blank:]])#'`, so a `#` inside a word
      or a literal is not an opener, and the erase is applied to the prefix ahead of the first
      opener with the suffix appended back. The position comparison is valid because `erase` pads
      its match to equal length, which is what keeps a column in `work` the same column as in the
      original line. Zero corpus churn, measured two ways: the gate still PASSes over 886 files
      with 0 findings, and a sweep of all 34 SEP occurrences found 0 carrying a `#` earlier on the
      line than the anchor, which still holds under the strictest possible `HASH_ERE='#'`.
      **The rig, before then after.** Positive controls 7 then 8, the forge being the eighth.
      Negative controls each scanned alone 13 then 14, the fourteenth a comment opening AFTER a
      real separator, which is the POSITIONAL guard: a sloppier "the line contains a `#`, refuse"
      implementation makes that line report and the loop fails loudly. Mutation arm `separator` 8
      then 9, the added line being a near miss to that clause. `banner`, `backtick`, `sep-prefix`,
      `sep-new-words` and `sep-long-option` all unmoved at 1, 1, 3, 5 and 1, each because control
      14 carries no hyphen run, no backticks, no `VAR=` or array prefix, an old word, and a short
      option. One arm ADDED, `sep-comment`, expected 7: with `HASH` disabled the forge is exempted
      again and the positive file drops 8 to 7, and the one line silenced is the forge, which is
      what makes `HASH` a near miss rather than slack. `dash -n` clean, and the diff read by eye
      for the bashism class `dash -n` does not catch. The gate also PASSes under mawk, nawk and
      gawk, so the new `length`/`substr`/`match` use is not gawk-specific.
- [x] **The aspace differ's baseline file's own header called the EMPTY diff the M6.3
      deliverable, which R5 falsified. FIXED, AND THIS ENTRY WAS FILED IN ERROR.** The header now
      reads "THE DIFF IS THE DELIVERABLE AND IT IS NOT EMPTY", names R2.1 as where the owed member
      was measured and R2.2 as where it landed, and tells a reader meeting the exit-2 refusal not to
      move the ref to quiet it. That is the whole of the fix this entry asked for.
      **It went in at `9b4bd49e`, the same commit that filed this entry**, so the entry was open and
      answered from the moment it was written: the entry's own reasoning that the pre-audit step had
      to hold the file byte-identical does not apply to a COMMENT, which the differ does not read
      (comments and blank lines are ignored, and the verdict is taken over the bare ref alone).
      The project's rule is that a review finding is FIXED or raised as a decision, never filed and
      merged; this one was both, which is the inversion, and the correction is to close it here and
      say so rather than to delete it. Closed 2026-08-28 at the M6.3 documentation audit.
