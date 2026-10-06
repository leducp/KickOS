// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Prints a compiled table in the canonical dump kickos_compose.emit.dump writes of its model,
// for round_trip.py. It finds each array from the header's counts alone, never from the
// emitted image's members, so a gap between two arrays shows as a dump that differs.

#include <kickos/sys/abi.h>
#include <kickos/sys/table.h>

#include <inttypes.h>
#include <iso646.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct entry_symbol
{
    char const* name;
    uintptr_t address;
};

// Written by round_trip.py beside the stub of each entry the table names, ending on a null name.
extern struct entry_symbol const entry_symbols[];

struct bit_name
{
    uint32_t bit;
    char const* name;
};

static struct bit_name const authority_names[] = {
    {KOS_AUTH_MEMORY, "memory"}, {KOS_AUTH_PINMUX, "pinmux"}, {KOS_AUTH_PSTATE, "pstate"},
    {KOS_AUTH_IRQ, "irq"},       {KOS_AUTH_SYSTEM, "system"}, {KOS_AUTH_CONSOLE, "console"},
    {KOS_AUTH_TASKS, "tasks"},   {KOS_AUTH_BUS_MASTER, "bus_master"}, {0, NULL}};
static struct bit_name const grant_flag_names[] = {
    {KOS_WINDOW_RO, "ro"}, {KOS_WINDOW_UNCACHED, "uncached"}, {0, NULL}};
static struct bit_name const port_flag_names[] = {{KOS_CAP_WAIT, "wait"}, {KOS_CAP_SIGNAL, "signal"}, {0, NULL}};
static struct bit_name const region_flag_names[] = {
    {KOS_MEM_NOCACHE, "uncached"}, {KOS_TABLE_REGION_PARTITION, "partition"}, {0, NULL}};
static struct bit_name const header_flag_names[] = {{KOS_TABLE_ENDS_TASK, "ends_task"}, {0, NULL}};
static struct bit_name const task_flag_names[] = {
    {KOS_TABLE_TASK_CONSOLE, "console"}, {KOS_TABLE_TASK_BLOCK_UNCACHED, "block_uncached"}, {0, NULL}};

static char const* const kind_names[] = {"endpoint_serve", "endpoint_use", "notification", "window",
                                         "ports",          "region",       "line",         "status",
                                         "port"};

static int faults = 0;

static void print_bits(char const* label, uint32_t word, struct bit_name const* names)
{
    printf(" %s=", label);
    if (word == 0)
    {
        printf("-");
        return;
    }
    char const* separator = "";
    for (struct bit_name const* n = names; n->name != NULL; ++n)
    {
        if ((word & n->bit) != 0)
        {
            printf("%s%s", separator, n->name);
            separator = ",";
            word &= ~n->bit;
        }
    }
    if (word != 0)
    {
        printf("%s0x%" PRIX32, separator, word);
    }
}

static void print_index(char const* label, uint16_t value)
{
    if (value == KOS_TABLE_NONE)
    {
        printf(" %s=none", label);
        return;
    }
    printf(" %s=%u", label, (unsigned)value);
}

static char const* text_at(char const* strings, uint32_t size, uint32_t offset)
{
    for (uint32_t at = offset; at < size; ++at)
    {
        if (strings[at] == '\0')
        {
            return strings + offset;
        }
    }
    ++faults;
    return "<no string>";
}

static char const* symbol_at(uintptr_t address)
{
    for (struct entry_symbol const* s = entry_symbols; s->name != NULL; ++s)
    {
        if (s->address == address)
        {
            return s->name;
        }
    }
    ++faults;
    return "<no symbol>";
}

int main(void)
{
    struct kos_table_header const* h = kickos_table;
    unsigned char const* at = (unsigned char const*)h + sizeof(*h);
    struct kos_table_task const* tasks = (struct kos_table_task const*)at;
    at += h->task_count * sizeof(struct kos_table_task);
    struct kos_table_grant const* grants = (struct kos_table_grant const*)at;
    at += h->grant_count * sizeof(struct kos_table_grant);
    struct kos_table_ref const* refs = (struct kos_table_ref const*)at;
    at += h->ref_count * sizeof(struct kos_table_ref);
    struct kos_table_priv const* privs = (struct kos_table_priv const*)at;
    at += h->priv_count * sizeof(struct kos_table_priv);
    struct kos_table_region const* regions = (struct kos_table_region const*)at;
    at += h->region_count * sizeof(struct kos_table_region);
    char const* strings = (char const*)at;

    printf("header magic=");
    if (h->magic == KOS_TABLE_MAGIC)
    {
        printf("ok");
    }
    else
    {
        printf("0x%" PRIX32, h->magic);
    }
    printf(" version=%u", (unsigned)h->version);
    print_bits("flags", h->flags, header_flag_names);
    print_index("ends_task", h->ends_task);
    printf(" tasks=%u grants=%u refs=%u privs=%u regions=%u strings=%" PRIu32 " init_priority=%u\n",
           (unsigned)h->task_count, (unsigned)h->grant_count, (unsigned)h->ref_count, (unsigned)h->priv_count,
           (unsigned)h->region_count, h->strings_size, (unsigned)h->init_priority);
    if (h->rsv0 != 0 or h->rsv1 != 0 or h->rsv2 != 0)
    {
        ++faults;
    }

    for (unsigned n = 0; n < h->task_count; ++n)
    {
        struct kos_table_task const* t = &tasks[n];
        uintptr_t entry = (uintptr_t)t->entry.task;
        if (t->driver != KOS_TABLE_NONE)
        {
            entry = (uintptr_t)t->entry.driver;
        }
        printf("task %u name=%s entry=%s", n, text_at(strings, h->strings_size, t->name), symbol_at(entry));
        print_index("driver", t->driver);
        printf(" stack=%" PRIu32 " block=%" PRIu32 " priority=%u ceiling=%u restart_max=%u", t->stack,
               t->block, (unsigned)t->priority, (unsigned)t->ceiling, (unsigned)t->restart_max);
        print_bits("flags", t->flags, task_flag_names);
        printf(" core_mask=0x%" PRIX32, t->core_mask);
        print_bits("authority", t->authority, authority_names);
        printf(" grants=%u+%u cap_grants=%u uses=%u+%u watches=%u+%u\n", (unsigned)t->first_grant,
               (unsigned)t->grant_count, (unsigned)t->cap_grant_count, (unsigned)t->first_use,
               (unsigned)t->use_count, (unsigned)t->first_watch, (unsigned)t->watch_count);
        if (t->rsv1 != 0 or t->rsv3[0] != 0 or t->rsv3[1] != 0 or t->rsv3[2] != 0
            or (t->flags & ~(uint16_t)(KOS_TABLE_TASK_CONSOLE | KOS_TABLE_TASK_BLOCK_UNCACHED)) != 0
            or ((t->flags & KOS_TABLE_TASK_BLOCK_UNCACHED) != 0 and t->block == 0)
            or (t->driver == KOS_TABLE_NONE and t->block != 0))
        {
            ++faults;
        }
    }

    for (unsigned n = 0; n < h->grant_count; ++n)
    {
        struct kos_table_grant const* g = &grants[n];
        printf("grant %u kind=", n);
        if (g->kind < sizeof(kind_names) / sizeof(kind_names[0]))
        {
            printf("%s", kind_names[g->kind]);
        }
        else
        {
            printf("kind%u", (unsigned)g->kind);
        }
        if (g->kind == KOS_GRANT_PORT)
        {
            print_bits("flags", g->flags, port_flag_names);
        }
        else
        {
            print_bits("flags", g->flags, grant_flag_names);
        }
        if (g->cap_slot == KOS_TABLE_NONE)
        {
            printf(" cap_slot=none");
        }
        else
        {
            printf(" cap_slot=cap0+%u", (unsigned)(g->cap_slot - KOS_SPAWN_DELEGATED_CAP0));
        }
        printf(" name=%s", text_at(strings, h->strings_size, g->name));
        printf(" path=%s", text_at(strings, h->strings_size, g->path));
        print_index("target", g->target);
        print_index("window", g->window);
        printf(" base=0x%" PRIX64 " size=0x%" PRIX32, g->base, g->size);
        print_index("line_index", g->line_index);
        print_index("line", g->line);
        printf(" privs=%u+%u\n", (unsigned)g->priv_first, (unsigned)g->priv_count);
        if (g->rsv1 != 0)
        {
            ++faults;
        }
    }

    for (unsigned n = 0; n < h->ref_count; ++n)
    {
        printf("ref %u task=%u\n", n, (unsigned)refs[n].task);
        if (refs[n].rsv0 != 0)
        {
            ++faults;
        }
    }

    for (unsigned n = 0; n < h->priv_count; ++n)
    {
        printf("priv %u offset=0x%X width=%u\n", n, (unsigned)privs[n].offset, (unsigned)privs[n].width);
        if (privs[n].rsv0 != 0)
        {
            ++faults;
        }
    }

    for (unsigned n = 0; n < h->region_count; ++n)
    {
        struct kos_table_region const* r = &regions[n];
        printf("region %u name=%s size=0x%" PRIX32 " offset=0x%" PRIX32, n, text_at(strings, h->strings_size, r->name),
               r->size, r->offset);
        print_bits("flags", r->flags, region_flag_names);
        printf("\n");
        if (r->rsv0[0] != 0 or r->rsv0[1] != 0 or r->rsv0[2] != 0)
        {
            ++faults;
        }
    }

    uint32_t offset = 0;
    while (offset < h->strings_size)
    {
        char const* text = text_at(strings, h->strings_size, offset);
        printf("string %" PRIu32 " %s\n", offset, text);
        uint32_t length = 0;
        while (text[length] != '\0')
        {
            ++length;
        }
        offset = offset + length + 1;
    }

    if (faults != 0)
    {
        printf("faults=%d\n", faults);
        return 1;
    }
    return 0;
}
