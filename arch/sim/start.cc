// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The host process entry. Under KICKOS_MULTI_INSTANCE the process hosts several independent
// kernels, one per emulated MCU, handed to host threads here.

#include <kickos/arch/arch.h>

#if KICKOS_MULTI_INSTANCE
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

// An EMPTY app-ctor window: the host runtime has run every app ctor before main, so kmain's
// walk must see start == end or it constructs them twice. Labels, not a zero-length array: two
// distinct objects need not share an address.
asm(".pushsection .kickos_app_init_array,\"a\"\n"
    ".globl __kickos_app_init_array_start\n"
    ".globl __kickos_app_init_array_end\n"
    "__kickos_app_init_array_start:\n"
    "__kickos_app_init_array_end:\n"
    ".popsection\n");

// The windows a hosted image has none of, stated empty: kmain and domain.cc reference these
// bounds strongly, so absent is a link error. Labels, so each pair is start == end at a real
// address.
asm(".pushsection .kickos_link_bounds,\"a\"\n"
    ".globl __kickos_code_start\n"
    ".globl __kickos_code_end\n"
    ".globl __kickos_appdata_start\n"
    ".globl __kickos_appdata_end\n"
    ".globl _kickos_heap_start\n"
    ".globl _kickos_heap_limit\n"
    "__kickos_code_start:\n"
    "__kickos_code_end:\n"
    "__kickos_appdata_start:\n"
    "__kickos_appdata_end:\n"
    "_kickos_heap_start:\n"
    "_kickos_heap_limit:\n"
    ".popsection\n");

#if KICKOS_MULTI_INSTANCE

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
        // An unadopted host thread holds index 0 and would run inside instance 0's kernel.
        kickos::instance_select(n->index);
        n->status = arch_sim_instance_run(n->argc, n->argv);
        return nullptr;
    }

    // An env knob, not an argument: argv belongs to the app inside each instance.
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
            // Instances already running still finish; the exit status then reports the failure.
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
