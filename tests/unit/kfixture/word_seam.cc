// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mutex word's seam (kickos/word.h) and the user write (kickos/aspace.h) over host memory,
// for a posture that links neither kernel/syscall/word.cc nor syscall_mem.cc: a word's key is its
// address, every live thread could hold it, and at a translating posture the alias is the address
// itself and g_word_fault makes every hold fail as a vanished page does. Each is weak, so a
// posture linking the real word.cc or syscall_mem.cc beside the fixture keeps the real one.
// Without translation the access is word.h's own.

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/sync.h>
#include <kickos/thread.h>
#include <kickos/word.h>

#include <kickos/sys/errno.h>

#include <string.h>

#include "kfixture.h"

namespace kickos
{
    namespace testfix
    {
        bool g_word_fault = false;
    }

    namespace
    {
        UserOwner owner_of(Thread const* c)
        {
#if KICKOS_HAVE_ASPACE
            return domain_space(thread_domain(c));
#else
            return c;
#endif
        }
    }

#if KICKOS_HAVE_ASPACE or KICKOS_ARCH_ARENA_DCACHE
    __attribute__((weak)) UserOwner user_space_of(Thread const* t)
    {
        return owner_of(t);
    }
#endif

    __attribute__((weak)) bool kaccess_to_user(UserOwner, uintptr_t udst, void const* ksrc,
                                               size_t n)
    {
        memcpy(reinterpret_cast<void*>(udst), ksrc, n);
        return true;
    }

    __attribute__((weak)) int word_ref(Thread const* c, uintptr_t va, WordRef* out)
    {
        if ((va & (sizeof(uint32_t) - 1u)) != 0)
        {
            return -KOS_EINVAL;
        }
        if (c == nullptr)
        {
            return -KOS_EFAULT;
        }
        out->key = static_cast<arch_phys_addr_t>(va);
        out->space = owner_of(c);
        out->va = va;
        return 0;
    }

    __attribute__((weak)) WordRef word_ref_parked(Thread const* w, arch_phys_addr_t key)
    {
        WordRef r;
        r.key = key;
        r.space = owner_of(w);
        r.va = w->wait_va;
        return r;
    }

    __attribute__((weak)) bool word_could_hold(Thread const* t, WordRef const&)
    {
        return t != nullptr;
    }

#if KICKOS_HAVE_ASPACE
    __attribute__((weak)) uint32_t* kaccess_word_get(WordRef const& w)
    {
        if (testfix::g_word_fault)
        {
            return nullptr;
        }
        return reinterpret_cast<uint32_t*>(w.va);
    }

    __attribute__((weak)) void kaccess_word_put(WordRef const&)
    {
    }
#endif

    __attribute__((weak)) int word_open(Thread const* c, uintptr_t va, WordRef* out,
                                        uint32_t** word)
    {
        *word = nullptr;
        int const rc = word_ref(c, va, out);
        if (rc != 0)
        {
            return rc;
        }
#if KICKOS_HAVE_ASPACE
        *word = kaccess_word_get(*out);
        if (*word == nullptr)
        {
            return -KOS_EFAULT;
        }
#else
        *word = reinterpret_cast<uint32_t*>(va);
#endif
        return 0;
    }
}
