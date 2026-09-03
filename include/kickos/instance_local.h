// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Instance selection, and the holder every instance-scoped object uses.
//
// Several independent kernels may co-reside in one address space, one per emulated MCU
// under the multi-instance sim. Every instance-scoped object is KICKOS_MAX_INSTANCES of
// something addressed by ONE index, and kickos_instance_index() is the only line a
// different keying substitutes.
//
// This lives on the tree-wide include root because the arch layer needs the same holder
// for its own backend state, and arch may not include a kernel header.

#ifndef KICKOS_INSTANCE_LOCAL_H
#define KICKOS_INSTANCE_LOCAL_H

// The provisioning bound, from the generated board config where a board states one.
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif
#ifndef KICKOS_MAX_INSTANCES
#define KICKOS_MAX_INSTANCES 1
#endif

// KICKOS_AMP_SHARED_IMAGE and arch_cpu_id, for the AMP keying below.
#include <kickos/arch/arch.h>

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE && KICKOS_AMP_SHARED_IMAGE
#define kickos_instance_index() (arch_cpu_id())
#elif defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
namespace kickos
{
    namespace detail
    {
        // __thread, not thread_local: an `extern thread_local` is reached through a
        // generated TLS wrapper CALL, so every instance-scoped access would carry a
        // function call. initial-exec, not the default model, because this is read from
        // signal handlers and __tls_get_addr is not async-signal-safe.
        extern __thread unsigned g_instance_index __attribute__((tls_model("initial-exec")));
    }
}
#define kickos_instance_index() (::kickos::detail::g_instance_index)
#else
#define kickos_instance_index() 0u
#endif

namespace kickos
{
    // One T per instance, for state a module owns privately, so a module's internals
    // never have to enter a shared header to become per-instance.
    //
    // At KICKOS_MAX_INSTANCES == 1 the subscript is a literal 0 over a one-element array:
    // same size, same alignment, same address, same instructions at every use.
    template <typename T>
    struct InstanceLocal
    {
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE && KICKOS_AMP_SHARED_IMAGE
        static_assert(KICKOS_MAX_INSTANCES >= KICKOS_NUM_CORES,
                      "an AMP image keys the instance on the core, so it must provision one "
                      "instance per core the image drives");
#endif
        T per_instance[KICKOS_MAX_INSTANCES];

        T& get() { return per_instance[kickos_instance_index()]; }
    };

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE && !KICKOS_AMP_SHARED_IMAGE
    // Adopt instance `i`, and hand back the index it displaced. A host thread that never
    // calls this holds index 0, so every spawned one must adopt before its first
    // instance-scoped access.
    //
    // The host-thread keying only: an index READ from the hardware has nothing to adopt, so
    // there is no AMP arm of this and a setter over a core identity would be a second truth.
    inline unsigned instance_select(unsigned i)
    {
        unsigned const prev = detail::g_instance_index;
        detail::g_instance_index = i;
        return prev;
    }
#endif
}

#endif
