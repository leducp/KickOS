// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "book.h"

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdio.h>

#include <kickos/amp.h>
#include <kickos/sys/table.h>

uint32_t ampping_crossing(kos_self_t const* self, uint8_t right, uint32_t node, kos_cap_t* cap)
{
    struct kos_table_header const* const h = kickos_table;
    struct kos_table_task const* const tasks = (struct kos_table_task const*)(h + 1);
    struct kos_table_grant const* const grants = (struct kos_table_grant const*)(tasks + h->task_count);
    uint16_t k;
    for (k = 0; k < self->grant_count; k++)
    {
        struct kos_table_grant const* const g = &grants[self->first_grant + k];
        if (g->kind != KOS_GRANT_PORT or g->flags != right or (node != KOS_AMP_NO_ENTRY and g->target != node))
        {
            continue;
        }
        char name[16];
        (void)snprintf(name, sizeof(name), "/amp/%u", (unsigned)g->base);
        *cap = kos_grant_endpoint(self, name);
        return (uint32_t)g->base;
    }
    *cap = KOS_CAP_NONE;
    return KOS_AMP_NO_ENTRY;
}
