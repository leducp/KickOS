// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// sim startup glue: the host process entry. Brings up the arch backend then
// enters the kernel. On MCU targets the equivalent is the reset handler.
//
// Under KICKOS_MULTI_INSTANCE the process hosts several independent kernels, one per
// emulated MCU, so this is also where instances are handed to host threads.

#include <kickos/arch/arch.h>

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
#include <kickos/instance_local.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" int arch_sim_instance_run(int argc, char** argv);
extern "C" void arch_sim_instance_hosting(unsigned count);
#else
namespace kickos
{
    int kmain(int argc, char** argv);
}
#endif

// Explicitly EMPTY app-ctor window: the host runtime has already run every app ctor
// before main, so kmain's walk must see start == end (walking again would construct
// them twice). kmain's bounds are STRONG, so a target with no window fails the link.
// Labels rather than a zero-length array: two distinct C++ objects are not guaranteed
// to share an address, and start == end must hold exactly.
asm(".pushsection .kickos_app_init_array,\"a\"\n"
    ".globl __kickos_app_init_array_start\n"
    ".globl __kickos_app_init_array_end\n"
    "__kickos_app_init_array_start:\n"
    "__kickos_app_init_array_end:\n"
    ".popsection\n");

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE

namespace
{
    struct Node
    {
        unsigned index;
        int argc;
        char** argv;
        int status;
    };

    void* node_entry(void* arg)
    {
        Node* const n = static_cast<Node*>(arg);
        // Before anything instance-scoped: an unadopted host thread holds index 0 and
        // would run inside instance 0's kernel without saying so.
        kickos::instance_select(n->index);
        n->status = arch_sim_instance_run(n->argc, n->argv);
        return nullptr;
    }

    // How many kernels this process hosts. An env knob rather than an argument because
    // argv belongs to the app inside each instance and is handed on unchanged.
    unsigned instance_count()
    {
        char const* const spec = getenv("KICKOS_SIM_INSTANCES");
        if (spec == nullptr or spec[0] == '\0')
        {
            return 1;
        }
        char* end = nullptr;
        unsigned long const n = strtoul(spec, &end, 10);
        if (end == spec or *end != '\0' or n == 0 or n > KICKOS_MAX_INSTANCES)
        {
            fprintf(stderr,
                    "[KickOS] KICKOS_SIM_INSTANCES='%s' is not 1..%d "
                    "(KICKOS_MAX_INSTANCES)\n",
                    spec, KICKOS_MAX_INSTANCES);
            return 0;
        }
        return static_cast<unsigned>(n);
    }
}

int main(int argc, char** argv)
{
    unsigned const count = instance_count();
    if (count == 0)
    {
        return 1;
    }
    arch_sim_instance_hosting(count);

    static Node nodes[KICKOS_MAX_INSTANCES];
    static pthread_t threads[KICKOS_MAX_INSTANCES];
    unsigned started = 0;
    for (unsigned i = 0; i < count; i++)
    {
        nodes[i].index = i;
        nodes[i].argc = argc;
        nodes[i].argv = argv;
        nodes[i].status = 0;
        int const rc = pthread_create(&threads[i], nullptr, node_entry, &nodes[i]);
        if (rc != 0)
        {
            // Report before joining: the instances already running still get to finish,
            // but the run no longer means what it was asked to mean.
            fprintf(stderr, "[KickOS] instance %u: pthread_create failed (%d)\n", i, rc);
            break;
        }
        started++;
    }

    int status = 0;
    for (unsigned i = 0; i < started; i++)
    {
        pthread_join(threads[i], nullptr);
        if (status == 0)
        {
            status = nodes[i].status;
        }
    }
    if (started != count and status == 0)
    {
        status = 1;
    }
    return status;
}

#else

int main(int argc, char** argv)
{
    arch_init();
    return kickos::kmain(argc, argv);
}

#endif
