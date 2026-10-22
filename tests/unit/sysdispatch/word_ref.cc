// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A user word the kernel names at the translating posture, over the REAL address-space, domain,
// range and system-call layers: the key every mapping of one frame shares, which thread may
// name the word, which thread could hold it, and the kernel's access to it. Every memory a
// thread reaches is built by the real calls that build it.

#include <kickos/sync.h>
#include <kickos/word.h>

#include <stdint.h>

#include "host_aspace.h"
#include "sys_fixture.h"
#include "syscall_internal.h"

extern "C"
{
    alignas(4096) unsigned char g_sys_image[SYS_IMAGE_BYTES];
    alignas(4096) unsigned char g_word_text[4096];
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uintptr_t RUN_VA = static_cast<uintptr_t>(0x500000000000ull);
            constexpr uintptr_t RUN_VA2 = RUN_VA + 4u * G;
            constexpr uintptr_t AT = 8u;
            constexpr uint32_t RW = ARCH_MAP_R | ARCH_MAP_W;
            uint32_t g_stand_in = 0;

            class WordRefSpace : public SysFixture
            {
              protected:
                static int ref(Thread const* t, uintptr_t va, WordRef* w)
                {
                    IrqLock lock;
                    return word_ref(t, va, w);
                }

                static WordRef named(Thread const* t, uintptr_t va)
                {
                    WordRef w;
                    EXPECT_EQ(ref(t, va, &w), 0) << "the word at " << va << " was refused";
                    return w;
                }

                static bool holds(Thread const* t, WordRef const& w)
                {
                    IrqLock lock;
                    return word_could_hold(t, w);
                }

                static VirtualRanges const* ranges_of(Thread const* t)
                {
                    return domain_ranges(thread_domain(t));
                }

                static arch_phys_addr_t frame_of_va(Thread const* t, uintptr_t va)
                {
                    return arch_aspace_frame_at(domain_space(thread_domain(t)), va);
                }

                // A thread spawned into `into`, or into main's own task for KOS_TASK_NONE, holding
                // one memory window over [base, base + bytes) when `bytes` is not 0.
                Thread* spawn_into(kos_task_t into, uintptr_t base = 0, size_t bytes = 0,
                                   uint8_t flags = 0)
                {
                    kos_thread_t h = KOS_THREAD_NONE;
                    if (spawn(main_, spawn_params(MAIN_PARAMS, base, bytes, flags, into),
                              MAIN_OUT, &h)
                        != 0)
                    {
                        return nullptr;
                    }
                    return kernel().threads.resolve(h);
                }

                // Where `t` reaches its first window.
                static uintptr_t window_of(Thread const* t)
                {
                    VirtualRanges const* const r = ranges_of(t);
                    uint16_t const holder =
                        static_cast<uint16_t>(kernel().threads.index_of(t) + 1);
                    for (size_t i = 0; r != nullptr and i < VirtualRanges::capacity(); i++)
                    {
                        VirtualRange const* const e = r->at(i);
                        if (e != nullptr and (e->flags & VR_WINDOW) != 0 and e->holder == holder)
                        {
                            return e->base;
                        }
                    }
                    return 0;
                }

                // The first range of `t`'s space carrying every flag of `flags`.
                static VirtualRange const* range_flagged(Thread const* t, uint8_t flags)
                {
                    VirtualRanges const* const r = ranges_of(t);
                    for (size_t i = 0; r != nullptr and i < VirtualRanges::capacity(); i++)
                    {
                        VirtualRange const* const e = r->at(i);
                        if (e != nullptr and (e->flags & flags) == flags)
                        {
                            return e;
                        }
                    }
                    return nullptr;
                }

                // Whether a hold of the word reaches it, the hold giving back every page it
                // acquired.
                static bool reached(WordRef const& w)
                {
                    AcquireHolds const before = g_acquire_holds;
                    bool r = false;
                    {
                        WordAccess word(w);
                        r = word.reached();
                    }
                    expect_released(before);
                    return r;
                }

                // A system call as `t`, which must give back every page it acquired.
                static int32_t call_released(Thread* t, uintptr_t nr, uintptr_t a0,
                                             uintptr_t a1 = 0, uintptr_t a2 = 0)
                {
                    AcquireHolds const before = g_acquire_holds;
                    int32_t const rc = err(call_as(t, nr, a0, a1, a2));
                    expect_released(before);
                    return rc;
                }

                // A free word `t` may not name is neither taken by its try nor, held by `t`,
                // released by its unlock. `word` is null where nothing maps it.
                static void expect_untouched(Thread* t, uintptr_t va, uint32_t* word,
                                             int32_t refusal)
                {
                    if (word != nullptr)
                    {
                        *word = 0;
                    }
                    EXPECT_EQ(call_released(t, KOS_SYS_MUTEX_LOCK, va, 0), refusal)
                        << "a lock of the word at " << va;
                    if (word != nullptr)
                    {
                        EXPECT_EQ(*word, 0u) << "a refused lock took the word at " << va;
                        *word = t->lock_id;
                    }
                    EXPECT_EQ(call_released(t, KOS_SYS_MUTEX_UNLOCK, va), refusal)
                        << "an unlock of the word at " << va;
                    if (word != nullptr)
                    {
                        EXPECT_EQ(*word, t->lock_id) << "a refused unlock released the word at "
                                                     << va;
                    }
                }

                // `waiter` parked through `va` on a record of the word `owner` holds, the key
                // `owner` names. The fixture's own write lands in a stand-in: no record write
                // reads the word.
                static WordRecord* park_on_word(Thread* owner, Thread* waiter, uintptr_t va,
                                           uintptr_t owner_va)
                {
                    WordRef const w = named(owner, owner_va);
                    WordRecord* const m = own_mutex(owner, &g_stand_in);
                    m->key = w.key;
                    park_mutex_waiter(waiter, m);
                    waiter->wait_va = va;
                    return m;
                }

                WordRecord* park_on_mains_word(Thread* waiter, uintptr_t va)
                {
                    return park_on_word(main_, waiter, va, va);
                }

                // `waiter` parked on the plain word `va` names for main, as KOS_SYS_WORD_WAIT
                // leaves it.
                WordRecord* park_plain(Thread* waiter, uintptr_t va)
                {
                    WordRef const w = named(main_, va);
                    WordRecord* const m = plain_word(&g_stand_in);
                    m->key = w.key;
                    park_word_waiter(waiter, m);
                    waiter->wait_va = va;
                    return m;
                }

                static void expect_the_wait_ended(Thread* waiter, Thread* owner)
                {
                    EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the waiter is still parked";
                    EXPECT_EQ(waiter->wait_result, -KOS_EFAULT);
                    EXPECT_EQ(kernel().word_active, nullptr) << "the emptied record was kept";
                    EXPECT_EQ(owner->held_list, nullptr);
                }

                void expect_the_wait_ended(Thread* waiter)
                {
                    expect_the_wait_ended(waiter, main_);
                }

                static void expect_released(AcquireHolds const& before)
                {
                    EXPECT_EQ(g_acquire_holds.live, before.live) << "an acquire was kept";
                    EXPECT_EQ(g_acquire_holds.unpaired, before.unpaired)
                        << "a release matched no acquire";
                }

                int32_t map_run(RunCaps const& run, uintptr_t va)
                {
                    return err(call_as(main_, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(va),
                                       0));
                }
            };

            // THE KEY IS THE FRAME, NOT THE ADDRESS. Keyed on the address, the two mappings of
            // one run would answer two keys and two waiters would sleep on two records.
            TEST_F(WordRefSpace, one_run_mapped_at_two_addresses_gives_one_key)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(map_run(run, RUN_VA), 0);
                ASSERT_EQ(map_run(run, RUN_VA2), 0);
                WordRef const a = named(main_, RUN_VA + AT);
                WordRef const b = named(main_, RUN_VA2 + AT);
                EXPECT_EQ(a.key, run.base + AT);
                EXPECT_EQ(b.key, a.key) << "one frame answered two keys";
                EXPECT_EQ(a.va, RUN_VA + AT) << "the word is reached where it was named";
                EXPECT_EQ(b.va, RUN_VA2 + AT);
            }

            // A memory window sits where the kernel chose, not at its frames' address.
            TEST_F(WordRefSpace, a_window_address_gives_the_donor_s_frame)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const child = spawn_into(into, blk, G);
                ASSERT_NE(child, nullptr);
                uintptr_t const win = window_of(child);
                ASSERT_NE(win, 0u);
                ASSERT_NE(win, blk) << "fixture: the window landed at the donor's own address";
                WordRef const donor = named(main_, blk + AT);
                WordRef const holder = named(child, win + AT);
                EXPECT_EQ(donor.key, frame_of_va(main_, blk) + AT);
                EXPECT_EQ(holder.key, donor.key) << "the window's word is another key";
                EXPECT_TRUE(holds(child, donor)) << "the window's holder could not hold it";
                EXPECT_TRUE(holds(main_, holder)) << "the donor could not hold it";
            }

            // Task memory: the child's space maps the donor's reservation at the donor's address.
            TEST_F(WordRefSpace, a_handoff_is_one_key_seen_from_both_sides)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, blk, G, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const member = spawn_into(into);
                ASSERT_NE(member, nullptr);
                ASSERT_NE(domain_space(thread_domain(member)), domain_space(thread_domain(main_)));
                WordRef const donor = named(main_, blk + AT);
                WordRef const borrower = named(member, blk + AT);
                EXPECT_EQ(borrower.key, donor.key);
                EXPECT_NE(borrower.space, donor.space);
                EXPECT_TRUE(holds(member, donor));
                EXPECT_TRUE(holds(main_, borrower));
            }

            // A frame capability's mapping in another space, made by main through that space's
            // capability.
            TEST_F(WordRefSpace, a_run_mapped_into_another_space_is_held_from_there)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(map_run(run, RUN_VA), 0);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const member = spawn_into(into);
                ASSERT_NE(member, nullptr);
                WordRef const w = named(main_, RUN_VA + AT);
                EXPECT_FALSE(holds(member, w)) << "a space that maps nothing of it held it";

                uint32_t const acap = space_cap(main_, thread_domain(member));
                ASSERT_NE(acap, KCAP_INVALID);
                ASSERT_EQ(err(call_as(main_, KOS_SYS_FRAME_MAP, run.fcap, acap,
                                      va_word(RUN_VA2), 0)),
                          0);
                EXPECT_TRUE(holds(member, w)) << "the run's other mapping is a frame capability's";
                WordRef const there = named(member, RUN_VA2 + AT);
                EXPECT_EQ(there.key, w.key);
                EXPECT_TRUE(holds(main_, there));
            }

            // Stacks are reserved and mapped task-wide, never granted: a thread names its
            // sibling's stack through the stack range itself, and never its guard.
            TEST_F(WordRefSpace, a_sibling_s_stack_is_nameable_and_its_guard_is_not)
            {
                Thread* const sib = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(sib, nullptr);
                ASSERT_EQ(thread_domain(sib), thread_domain(main_));
                uintptr_t const lo = reinterpret_cast<uintptr_t>(sib->stack_base);
                uintptr_t const top = lo + sib->stack_size - 64u;
                VirtualRange const* const e = ranges_of(main_)->find(top, sizeof(uint32_t));
                ASSERT_NE(e, nullptr);
                ASSERT_NE(e->flags & VR_USTACK, 0u) << "fixture: the stack is no kernel stack run";
                EXPECT_EQ(named(main_, top).key, frame_of_va(main_, top & ~(G - 1u)) + (top & (G - 1u)));
                EXPECT_EQ(named(sib, top).key, named(main_, top).key) << "its own stack";
                EXPECT_TRUE(holds(sib, named(main_, top)));
                EXPECT_TRUE(holds(main_, named(sib, top)));

                // The guard mapped by hand, so its refusal is the stack clause's and not the
                // walk's.
                uintptr_t const guard_top = e->base + G - sizeof(uint32_t);
                EXPECT_EQ(named(main_, e->base + G).key, frame_of_va(main_, e->base + G))
                    << "the first word above the guard";
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                struct arch_aspace* const space = domain_space(thread_domain(main_));
                ASSERT_EQ(arch_aspace_map(space, e->base, run.base, 1, RW, ARCH_MAP_NORMAL),
                          ARCH_ASPACE_OK);
                ASSERT_NE(frame_of_va(main_, e->base), 0u);
                WordRef w;
                EXPECT_EQ(ref(main_, e->base + AT, &w), -KOS_EFAULT) << "the guard was named";
                EXPECT_EQ(ref(main_, guard_top, &w), -KOS_EFAULT) << "the guard's last word";
                EXPECT_EQ(ref(sib, guard_top, &w), -KOS_EFAULT) << "its own guard";
                ASSERT_EQ(arch_aspace_unmap(space, e->base, 1), ARCH_ASPACE_OK);
            }

            // Each space copies the image data, so one static's address is a key per space, and
            // no other space could hold it.
            TEST_F(WordRefSpace, a_static_word_is_a_key_per_space)
            {
                VirtualRange const* const data = range_flagged(main_, VR_IMAGE);
                ASSERT_NE(data, nullptr);
                VirtualRange const* d = data;
                if ((d->rights & ARCH_MAP_W) == 0)
                {
                    d = nullptr;
                    VirtualRanges const* const r = ranges_of(main_);
                    for (size_t i = 0; i < VirtualRanges::capacity(); i++)
                    {
                        VirtualRange const* const e = r->at(i);
                        if (e != nullptr and (e->flags & VR_IMAGE) != 0
                            and (e->rights & ARCH_MAP_W) != 0)
                        {
                            d = e;
                        }
                    }
                }
                ASSERT_NE(d, nullptr) << "fixture: no writable image data";
                uintptr_t const va = d->base + AT;
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const other = spawn_into(into);
                ASSERT_NE(other, nullptr);
                Thread* const sib = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(sib, nullptr);
                WordRef const mine = named(main_, va);
                WordRef const theirs = named(other, va);
                EXPECT_NE(mine.key, theirs.key) << "two spaces' copies of one static met";
                EXPECT_TRUE(holds(sib, mine));
                EXPECT_FALSE(holds(other, mine)) << "another space held this space's copy";
            }

            TEST_F(WordRefSpace, a_writable_window_holder_holds_and_a_read_only_one_does_not)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const reader = spawn_into(into, blk, G, KOS_WINDOW_RO);
                ASSERT_NE(reader, nullptr);
                uintptr_t const win = window_of(reader);
                ASSERT_NE(win, 0u);
                WordRef w;
                EXPECT_EQ(ref(reader, win + AT, &w), -KOS_EFAULT) << "a read-only word was named";
                EXPECT_FALSE(holds(reader, named(main_, blk + AT)))
                    << "a thread that can only read the word held it";
            }

            TEST_F(WordRefSpace, a_word_the_caller_cannot_write_is_refused)
            {
                WordRef w;
                EXPECT_EQ(ref(main_, uva(MAIN_OUT, 2), &w), -KOS_EINVAL) << "misaligned";
                EXPECT_EQ(ref(main_, RUN_VA + AT, &w), -KOS_EFAULT) << "nothing mapped";
                uintptr_t const blk = reserve_only(G);
                ASSERT_NE(blk, 0u);
                EXPECT_EQ(ref(main_, blk + AT, &w), -KOS_EFAULT) << "reserved, never granted";
                VirtualRange const* const text = ranges_of(main_)->find(
                    reinterpret_cast<uintptr_t>(g_word_text) + AT, sizeof(uint32_t));
                ASSERT_NE(text, nullptr) << "fixture: the image has no text";
                EXPECT_EQ(ref(main_, reinterpret_cast<uintptr_t>(g_word_text) + AT, &w),
                          -KOS_EFAULT)
                    << "a text word";
            }

            TEST_F(WordRefSpace, a_word_in_uncached_or_device_memory_is_refused)
            {
                uintptr_t const unc = reserve(main_, G, KOS_MEM_NOCACHE);
                ASSERT_NE(unc, 0u);
                WordRef w;
                EXPECT_EQ(ref(main_, unc + AT, &w), -KOS_EINVAL) << "an uncached reservation";

                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const child = spawn_into(into, unc, G, KOS_WINDOW_UNCACHED);
                ASSERT_NE(child, nullptr);
                EXPECT_EQ(ref(child, window_of(child) + AT, &w), -KOS_EINVAL)
                    << "an uncached window";

                // A device window as the spawn's map records one, over a frame of the pool.
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint16_t const holder = static_cast<uint16_t>(kernel().threads.index_of(main_) + 1);
                {
                    IrqLock lock;
                    Domain* const d = thread_domain(main_);
                    VirtualRanges* const r = domain_ranges_mut(d);
                    ASSERT_TRUE(r->reserve(RUN_VA, 1, VR_BORROWED | VR_WINDOW, 0, holder));
                    ASSERT_EQ(arch_aspace_map(domain_space(d), RUN_VA, run.base, 1, RW,
                                              ARCH_MAP_DEVICE),
                              ARCH_ASPACE_OK);
                    ASSERT_TRUE(r->grant(RUN_VA, 1, RW, static_cast<uint8_t>(ARCH_MAP_DEVICE)));
                }
                EXPECT_EQ(ref(main_, RUN_VA + AT, &w), -KOS_EINVAL) << "a device window";
            }

            // A retype the self-grant admits takes the write a waiter parked through exactly when
            // it leaves normal memory: the self-grant always maps read and write.
            TEST_F(WordRefSpace, a_self_grant_retype_reports_the_range_to_end)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                VirtualRange const was = *ranges_of(main_)->at_base(blk);
                uint8_t const normal = static_cast<uint8_t>(ARCH_MAP_NORMAL);
                uint8_t const nocache = static_cast<uint8_t>(ARCH_MAP_NOCACHE);
                EXPECT_FALSE(aspace_remap_takes_write(was, RW, normal)) << "the same grant again";
                EXPECT_TRUE(aspace_remap_takes_write(was, ARCH_MAP_R, normal));
                EXPECT_TRUE(aspace_remap_takes_write(was, RW, nocache));
                ASSERT_EQ(call_as(main_, KOS_SYS_MEM_SELF_GRANT, blk, G, KOS_MEM_NOCACHE), 0u);
                VirtualRange const now = *ranges_of(main_)->at_base(blk);
                EXPECT_EQ(now.memtype, nocache);
                WordRef w;
                EXPECT_EQ(ref(main_, blk + AT, &w), -KOS_EINVAL)
                    << "the retyped range still names a word";
                EXPECT_FALSE(aspace_remap_takes_write(now, RW, normal))
                    << "a retype back to normal memory took a write it never gave";
                EXPECT_FALSE(aspace_remap_takes_write(now, RW, nocache))
                    << "the same uncached grant again took a write it never gave";
                VirtualRange const reserved = *ranges_of(main_)->at_base(reserve_only(G));
                EXPECT_FALSE(aspace_remap_takes_write(reserved, RW, nocache))
                    << "a first grant took a write";
            }

            // Named while cacheable, then retyped: no access may go through the kernel's
            // cacheable alias, and each lets go of the page it acquired.
            TEST_F(WordRefSpace, a_word_retyped_uncached_after_it_was_named_is_refused_at_access)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                uint32_t* const word = host<uint32_t>(blk + AT);
                *word = 0x55u;
                WordRef const w = named(main_, blk + AT);
                ASSERT_EQ(call_as(main_, KOS_SYS_MEM_SELF_GRANT, blk, G, KOS_MEM_NOCACHE), 0u);
                IrqLock lock;
                EXPECT_FALSE(reached(w));
                EXPECT_EQ(*word, 0x55u) << "an uncached word was written";
            }

            TEST_F(WordRefSpace, the_kernel_reaches_a_window_s_word_in_the_donor_s_frame)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const child = spawn_into(into, blk, G);
                ASSERT_NE(child, nullptr);
                WordRef const w = named(child, window_of(child) + AT);
                uint32_t* const donor = host<uint32_t>(blk + AT);
                IrqLock lock;
                AcquireHolds const before = g_acquire_holds;
                {
                    WordAccess word(w);
                    ASSERT_TRUE(word.reached());
                    word.store(0x1234u);
                    EXPECT_EQ(*donor, 0x1234u);
                    uint32_t expected = 0x1234u;
                    EXPECT_TRUE(word.cas(&expected, 0x80000005u));
                    EXPECT_EQ(*donor, 0x80000005u);
                    expected = 5u;
                    EXPECT_FALSE(word.cas(&expected, 7u));
                    EXPECT_EQ(expected, 0x80000005u) << "a failed compare did not answer the word";
                    EXPECT_EQ(*donor, 0x80000005u);
                    EXPECT_EQ(word.load(), 0x80000005u);
                    EXPECT_EQ(g_acquire_holds.live, before.live + 1u) << "one hold per access";
                }
                expect_released(before);
            }

            // The write test asks the range the key's lookup found, here the list's last: a
            // granted word in it is named and locked, and the reserved ranges before it are not.
            TEST_F(WordRefSpace, a_word_in_the_last_range_of_a_full_list_is_named_by_its_own)
            {
                VirtualRanges const* const r = ranges_of(main_);
                uintptr_t bare = 0;
                while (r->count() + 1u < VirtualRanges::capacity())
                {
                    uintptr_t const va = reserve_only(G);
                    ASSERT_NE(va, 0u) << "fixture: the list filled at " << r->count();
                    bare = va;
                }
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                ASSERT_EQ(r->count(), VirtualRanges::capacity());
                ASSERT_EQ(r->find(blk + AT, sizeof(uint32_t)), r->at(VirtualRanges::capacity() - 1u))
                    << "fixture: the word's range is not the last slot";
                EXPECT_EQ(named(main_, blk + AT).key, frame_of_va(main_, blk) + AT);
                WordRef w;
                EXPECT_EQ(ref(main_, bare + AT, &w), -KOS_EFAULT) << "a reserved range was named";
                uint32_t* const word = host<uint32_t>(blk + AT);
                *word = 0;
                EXPECT_EQ(call_released(main_, KOS_SYS_MUTEX_LOCK, blk + AT, 0), 0);
                EXPECT_EQ(*word, main_->lock_id);
                EXPECT_EQ(call_released(main_, KOS_SYS_MUTEX_UNLOCK, blk + AT), 0);
                EXPECT_EQ(*word, 0u);
            }

            // Every refusal of the word's reference comes before the kernel reaches the word, and
            // no refused call keeps a hold.
            TEST_F(WordRefSpace, a_mutex_on_a_word_the_caller_cannot_name_leaves_it_alone)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const reader = spawn_into(into, blk, G, KOS_WINDOW_RO);
                ASSERT_NE(reader, nullptr);
                expect_untouched(reader, window_of(reader) + AT, host<uint32_t>(blk + AT),
                                 -KOS_EFAULT);
                uintptr_t const unc = reserve(main_, G, KOS_MEM_NOCACHE);
                ASSERT_NE(unc, 0u);
                expect_untouched(main_, unc + AT, host<uint32_t>(unc + AT), -KOS_EINVAL);
                uint32_t* const text = reinterpret_cast<uint32_t*>(g_word_text + AT);
                expect_untouched(main_, reinterpret_cast<uintptr_t>(text), nullptr, -KOS_EFAULT);
                uintptr_t const bare = reserve_only(G);
                ASSERT_NE(bare, 0u);
                expect_untouched(main_, bare + AT, nullptr, -KOS_EFAULT);
                expect_untouched(main_, RUN_VA + AT, nullptr, -KOS_EFAULT);
                expect_untouched(main_, blk + 2u, nullptr, -KOS_EINVAL);
            }

            // The waiter reached the word through its window, in another space: the record is
            // found by the frame from the holder's own address, and the hand-over lands there.
            TEST_F(WordRefSpace, an_unlock_hands_the_word_to_a_waiter_in_another_space)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const waiter = spawn_into(into, blk, G);
                ASSERT_NE(waiter, nullptr);
                uintptr_t const win = window_of(waiter);
                ASSERT_NE(win, 0u);
                uint32_t* const word = host<uint32_t>(blk + AT);
                *word = main_->lock_id | KOS_WORD_WAITERS;
                WordRecord* const m = park_on_word(main_, waiter, win + AT, blk + AT);
                EXPECT_EQ(m->key, named(waiter, win + AT).key) << "two spaces, two keys";
                EXPECT_EQ(call_released(main_, KOS_SYS_MUTEX_UNLOCK, blk + AT), 0);
                EXPECT_EQ(*word, waiter->lock_id) << "the hand-over wrote another word or owner";
                EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the waiter is still parked";
                EXPECT_EQ(waiter->wait_result, 0);
                EXPECT_EQ(kernel().word_active, nullptr) << "the emptied record was kept";
                EXPECT_EQ(main_->held_list, nullptr);
            }

            // A try from another space reads the word, asks whether its holder could hold it
            // there, and writes it only to repair the record's bit or to take it from the dead.
            TEST_F(WordRefSpace, a_try_from_another_space_reads_and_writes_the_holder_s_frame)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const taker = spawn_into(into, blk, G);
                ASSERT_NE(taker, nullptr);
                Thread* const waiter = spawn_into(into, blk, G);
                ASSERT_NE(waiter, nullptr);
                uintptr_t const at = window_of(taker) + AT;
                ASSERT_NE(at, AT);
                uint32_t* const word = host<uint32_t>(blk + AT);

                *word = main_->lock_id;
                EXPECT_EQ(call_released(taker, KOS_SYS_MUTEX_LOCK, at, 0), -KOS_ETIMEDOUT)
                    << "the holder's own reservation did not make it a live holder";
                EXPECT_EQ(*word, main_->lock_id) << "a failed try wrote the word";

                *word = main_->lock_id | KOS_WORD_WAITERS;
                (void)park_on_word(main_, waiter, window_of(waiter) + AT, blk + AT);
                *word = main_->lock_id;
                EXPECT_EQ(call_released(taker, KOS_SYS_MUTEX_LOCK, at, 0), -KOS_ETIMEDOUT);
                EXPECT_EQ(*word, main_->lock_id | KOS_WORD_WAITERS)
                    << "the record's bit was not set again";
            }

            TEST_F(WordRefSpace, a_try_from_another_space_takes_a_dead_holder_s_word)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const dead = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(dead, nullptr);
                uint32_t const dead_id = dead->lock_id;
                exit_as(dead);
                Thread* const taker = spawn_into(into, blk, G);
                ASSERT_NE(taker, nullptr);
                uint32_t* const word = host<uint32_t>(blk + AT);
                *word = dead_id;
                EXPECT_EQ(call_released(taker, KOS_SYS_MUTEX_LOCK, window_of(taker) + AT, 0),
                          -KOS_EOWNERDEAD);
                EXPECT_EQ(*word, taker->lock_id) << "the dead holder's word was not taken";
            }

            // Every call that takes from a space the write a waiter parked through ends that wait,
            // so the death walk's store through the waiter's own address can always land.
            TEST_F(WordRefSpace, a_waiter_through_a_frame_cap_word_ends_when_its_run_is_unmapped)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(map_run(run, RUN_VA), 0);
                Thread* const sibling = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(sibling, nullptr);
                (void)park_on_mains_word(sibling, RUN_VA + AT);
                ASSERT_EQ(call_as(main_, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                expect_the_wait_ended(sibling);
            }

            TEST_F(WordRefSpace, a_plain_waiter_through_a_frame_cap_word_ends_when_its_run_is_unmapped)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(map_run(run, RUN_VA), 0);
                Thread* const sibling = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(sibling, nullptr);
                (void)park_plain(sibling, RUN_VA + AT);
                ASSERT_EQ(call_as(main_, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                expect_the_wait_ended(sibling);
            }

            // A wake with a cancel pending on its caller completes, so the unit it carries
            // reaches the waiter; the caller dies at its next entry. A mutex unlock likewise.
            TEST_F(WordRefSpace, a_word_wake_and_a_mutex_unlock_are_no_death_point)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                Thread* const sibling = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(sibling, nullptr);
                uint32_t* const word = host<uint32_t>(blk + AT);
                (void)park_plain(sibling, blk + AT);
                *word = KOS_WORD_WAITERS;
                uint32_t* const lock = host<uint32_t>(blk + 2u * AT);
                *lock = main_->lock_id;
                {
                    IrqLock held;
                    thread_cancel(main_, held);
                }
                ASSERT_EQ(main_->cancel_kind, CANCEL_KILL) << "fixture: no cancel is pending";

                EXPECT_EQ(call_released(main_, KOS_SYS_WORD_WAKE, blk + AT, 1u, 1u), 1)
                    << "the wake did not complete";
                EXPECT_EQ(sibling->wait_kind, WAIT_NONE) << "the unit never reached the waiter";
                EXPECT_EQ(sibling->wait_result, 1);
                EXPECT_EQ(*word, 0u);
                EXPECT_EQ(call_released(main_, KOS_SYS_MUTEX_UNLOCK, blk + 2u * AT), 0);
                EXPECT_EQ(*lock, 0u);
                EXPECT_NE(main_->state, ThreadState::EXITED);
                EXPECT_EQ(main_->cancel_kind, CANCEL_KILL) << "the cancel did not stay pending";
                main_->cancel_kind = CANCEL_NONE;
            }

            TEST_F(WordRefSpace, a_waiter_through_a_reservation_word_ends_when_it_is_retyped)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                Thread* const sibling = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(sibling, nullptr);
                (void)park_on_mains_word(sibling, blk + AT);
                ASSERT_EQ(call_as(main_, KOS_SYS_MEM_SELF_GRANT, blk, G, KOS_MEM_NOCACHE), 0u);
                expect_the_wait_ended(sibling);
            }

            // The holder's own exit takes its stack, so a sibling parked on a word in it is
            // ended before the death walk could store through an unmapped page.
            TEST_F(WordRefSpace, a_waiter_on_a_word_in_a_dying_holder_s_stack_ends_with_it)
            {
                Thread* const dying = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(dying, nullptr);
                Thread* const sibling = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(sibling, nullptr);
                ASSERT_TRUE(dying->kstack_owned) << "fixture: the stack is not the kernel's";
                uintptr_t const va =
                    reinterpret_cast<uintptr_t>(dying->stack_base) + dying->stack_size - 64u;
                (void)park_on_word(dying, sibling, va, va);
                exit_as(dying);
                expect_the_wait_ended(sibling, dying);
            }

            TEST_F(WordRefSpace, a_waiter_through_a_dying_holder_s_window_ends_with_it)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                // The task's entry first: its death would end the task and every wait in it.
                Thread* const member = spawn_into(into);
                ASSERT_NE(member, nullptr);
                Thread* const dying = spawn_into(into, blk, G);
                ASSERT_NE(dying, nullptr);
                uintptr_t const win = window_of(dying);
                ASSERT_NE(win, 0u);
                (void)park_on_word(dying, member, win + AT, win + AT);
                exit_as(dying);
                expect_the_wait_ended(member, dying);
            }

            // The waiter reached the word through a window, at another address in another
            // space than its dying holder: the hand-over is written through the waiter's own.
            TEST_F(WordRefSpace, a_holder_s_death_hands_its_word_over_through_the_waiter_s_window)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_task_t const into = new_task(main_, 0, 0, 0);
                ASSERT_NE(into, KOS_TASK_NONE);
                Thread* const waiter = spawn_into(into, blk, G);
                ASSERT_NE(waiter, nullptr);
                Thread* const dying = spawn_into(KOS_TASK_NONE);
                ASSERT_NE(dying, nullptr);
                uintptr_t const win = window_of(waiter);
                ASSERT_NE(win, 0u);
                ASSERT_NE(win, blk) << "fixture: the window landed at the donor's own address";
                uint32_t* const word = host<uint32_t>(blk + AT);
                *word = dying->lock_id | KOS_WORD_WAITERS;
                (void)park_on_word(dying, waiter, win + AT, blk + AT);
                exit_as(dying);
                EXPECT_EQ(*word, waiter->lock_id) << "the death wrote another word or owner";
                EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the waiter is still parked";
                EXPECT_EQ(waiter->wait_result, -KOS_EOWNERDEAD);
                EXPECT_EQ(kernel().word_active, nullptr) << "the emptied record was kept";
                EXPECT_EQ(waiter->held_list, nullptr);
            }

            TEST_F(WordRefSpace, a_word_whose_page_went_is_unreachable)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(map_run(run, RUN_VA), 0);
                WordRef const w = named(main_, RUN_VA + AT);
                ASSERT_EQ(call_as(main_, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                IrqLock lock;
                EXPECT_FALSE(reached(w));
            }
        }
    }
}
