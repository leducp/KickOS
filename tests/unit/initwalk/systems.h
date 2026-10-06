// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The systems emit_systems.py emits, each table compiled under its own name, with what the kernel
// build tells the walk and what admission counts for the init.

#ifndef KICKOS_TESTS_UNIT_INITWALK_SYSTEMS_H
#define KICKOS_TESTS_UNIT_INITWALK_SYSTEMS_H

#include "walk.h"

#include <kickos/sys/table.h>

#include <stddef.h>
#include <stdint.h>

namespace systems
{
    // In the order `kickos_compose cost` prints them.
    struct Figures
    {
        uint32_t cap_slots;
        uint32_t endpoints;
        uint32_t notifications;
        uint32_t threads;
        uint32_t tasks;
        uint32_t irq_handles;
        uint32_t domains;
        uint32_t reservations;
        uint32_t self_grants;
    };

    // The kernel build's ceilings and stack rules, 0 for none.
    struct Limits
    {
        uint32_t cap_table;
        uint32_t tasks;
        uint32_t threads;
        uint32_t endpoints;
        uint32_t notifications;
        uint32_t irq_handles;
        uint32_t domains;
        uint32_t free_regions;
        uint32_t ram_owners;
        uint32_t task_endpoints;
        uint32_t task_notifications;
        uint32_t task_lines;
        uint32_t min_stack;
        uint32_t stride;
        uint32_t kernel_cores;
    };

    struct System
    {
        char const* name;
        kos_table_header const* const* table;
        kickos::init::Build build;
        uint32_t cap_reserved;
        uint32_t amp_ports;
        char const* refusal; // the panic the walk refuses it with at boot, or null
        Figures figures;
        Limits limits;
    };

    extern System const ALL[];
    extern size_t const COUNT;

    // The system named `name`, which must exist.
    System const& find(char const* name);
}

#endif
