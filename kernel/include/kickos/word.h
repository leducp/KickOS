// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A 32-bit word in user memory that the kernel reads, compares and writes on a thread's behalf:
// whether a thread may name it, the key every mapping of its memory shares, and the access to it.
// The word is hostile input; nothing here trusts what it holds.

#ifndef KICKOS_WORD_H
#define KICKOS_WORD_H

#include <kickos/arch/arch.h>
#include <kickos/aspace.h>

#include <kickos/arch/atomic_cas.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/errno.h>

#include <stdint.h>

namespace kickos
{
    struct Thread;

    struct WordRef
    {
        // The word's physical address: one frame mapped at two addresses, in one space or two,
        // gives one key.
        arch_phys_addr_t key = 0;
        UserOwner space = nullptr;
        uintptr_t va = 0;
    };

    // Whether `c` may name the word at `va`, and what it names. Reads nothing. 0 with *out
    // filled; -KOS_EINVAL for a misaligned word, one not in normal cacheable memory, or one in
    // the AMP partition's user share; -KOS_EFAULT for a word `c` cannot write. Caller holds
    // IrqLock.
    int word_ref(Thread const* c, uintptr_t va, WordRef* out);

    // word_ref, and the word reached in the same walk of its page: 0 with *word the kernel's
    // pointer to it, which kaccess_word_put(*out) gives back on a translating backend;
    // word_ref's refusals, and -KOS_EFAULT where its page cannot be reached, with nothing held.
    int word_open(Thread const* c, uintptr_t va, WordRef* out, uint32_t** word);

    // The reference through which parked thread `w` named the word keyed `key`
    // (Thread::wait_va), as word_ref answered it at the park.
    WordRef word_ref_parked(Thread const* w, arch_phys_addr_t key);

    // Whether `t` could have written the word `w` names, by its own write reach: the test that
    // separates a live owner from an id a writer forged. Caller holds IrqLock.
    bool word_could_hold(Thread const* t, WordRef const& w);

    using UserWord = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;

#if KICKOS_HAVE_ASPACE
    // The kernel's alias of the word through arch_aspace_acquire, held until kaccess_word_put;
    // null where its page is gone or no longer cacheable.
    uint32_t* kaccess_word_get(WordRef const& w);
    void kaccess_word_put(WordRef const& w);
#endif

    // The kernel's hold on the word `w` names, reached as kaccess reaches any user byte: through
    // the kernel's alias on a translating backend, at the word's own address elsewhere. Not
    // reached where the word's page is gone or no longer cacheable. The load is acquire, the
    // compare-and-swap acquire and release, the store release. Caller holds IrqLock across the
    // whole hold and parks nowhere inside it.
    class WordAccess
    {
      public:
        // `c`'s hold on the word at `va`, named into *named: status() answers word_open.
        __attribute__((always_inline)) WordAccess(Thread const* c, uintptr_t va, WordRef* named)
            : word_(nullptr)
        {
            status_ = word_open(c, va, named, &word_);
#if KICKOS_HAVE_ASPACE
            ref_ = *named;
#endif
        }

#if KICKOS_HAVE_ASPACE
        __attribute__((always_inline)) explicit WordAccess(WordRef const& w)
            : word_(kaccess_word_get(w)), ref_(w)
        {
            status_ = reach_status();
        }

        __attribute__((always_inline)) ~WordAccess()
        {
            if (word_ != nullptr)
            {
                kaccess_word_put(ref_);
            }
        }
#else
        // word_ref checked the address the kernel reaches the word at.
        __attribute__((always_inline)) explicit WordAccess(WordRef const& w)
            : word_(reinterpret_cast<uint32_t*>(w.va))
        {
            status_ = reach_status();
        }
#endif

        WordAccess(WordAccess const&) = delete;
        WordAccess& operator=(WordAccess const&) = delete;

        bool reached() const
        {
            return word_ != nullptr;
        }

        // 0 where the word is reached, else why not.
        int status() const
        {
            return status_;
        }

        uint32_t load() const
        {
            return reinterpret_cast<UserWord*>(word_)->load();
        }

        // False with nothing stored; *expected then holds what the word held, which is the value
        // asked for when the store kept being lost.
        bool cas(uint32_t* expected, uint32_t desired)
        {
            return arch_cas_u32(word_, expected, desired);
        }

        void store(uint32_t value)
        {
            reinterpret_cast<UserWord*>(word_)->store(value);
        }

      private:
        int reach_status() const
        {
            if (word_ == nullptr)
            {
                return -KOS_EFAULT;
            }
            return 0;
        }

        uint32_t* word_;
#if KICKOS_HAVE_ASPACE
        WordRef ref_;
#endif
        int status_ = 0;
    };
}

#endif
