// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The layout of the table a composition is emitted as (docs/design-m10-composition.md, "The
// emitted table"): a header, then task[], grant[], ref[], priv[] and region[] in that order with
// no gap between them, then the string pool. A string is an offset into the pool, and an index
// or a count is 16 bits, KOS_TABLE_NONE standing for none.

#ifndef KICKOS_SYS_TABLE_H
#define KICKOS_SYS_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/table_version.h> // KICKOS_TABLE_VERSION (generated)

#ifdef __cplusplus
extern "C"
{
#endif

#define KOS_TABLE_MAGIC 0x54534F4Bu
#define KOS_TABLE_NONE 0xFFFFu

// kos_table_header.flags
enum kos_table_flags
{
    KOS_TABLE_ENDS_TASK = 1 << 0 // the system ends when task `ends_task` returns
};

// kos_table_task.flags
enum kos_table_task_flags
{
    // A packaged driver that takes the console: the init narrows its endpoint at the end of its
    // handover rather than at boot, and drains it before the system ends.
    KOS_TABLE_TASK_CONSOLE = 1 << 0,
    // A packaged driver whose ring block is uncached: the init self-grants it KOS_MEM_NOCACHE.
    KOS_TABLE_TASK_BLOCK_UNCACHED = 1 << 1
};

// kos_table_grant.kind
enum kos_grant_kind
{
    KOS_GRANT_ENDPOINT_SERVE = 0,
    KOS_GRANT_ENDPOINT_USE = 1,
    KOS_GRANT_NOTIFICATION = 2,
    KOS_GRANT_WINDOW = 3,
    KOS_GRANT_PORTS = 4,
    KOS_GRANT_REGION = 5,
    KOS_GRANT_LINE = 6,
    KOS_GRANT_STATUS = 7 // /init/status: the init's status block, read-only, for a watcher
};

struct kos_service_cfg;
struct kos_table_task;

// The entry the init passes each task, which its lookups take.
typedef struct kos_table_task kos_self_t;

// A user task's entry or a packaged driver's start, as `driver` says.
union kos_table_entry
{
    void (*task)(kos_self_t const* self);
    int (*driver)(struct kos_service_cfg const* cfg);
    uint64_t width;
};

struct kos_table_header
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags; // enum kos_table_flags
    uint16_t ends_task;
    uint16_t task_count;
    uint16_t grant_count;
    uint16_t ref_count;
    uint16_t priv_count;
    uint16_t region_count;
    uint32_t strings_size;
    uint8_t init_priority; // the priority the init lowers itself to, its first act
    uint8_t rsv0;
    uint16_t rsv1;
    uint32_t rsv2;
};

struct kos_table_task
{
    uint32_t name;
    uint32_t block; // a packaged driver's ring block in bytes, 0 for none or for user code
    union kos_table_entry entry;
    uint16_t driver; // catalogue index, or KOS_TABLE_NONE for user code
    uint16_t rsv1;
    uint32_t stack;
    uint8_t priority;
    uint8_t restart_max;
    uint16_t flags; // enum kos_table_task_flags
    uint32_t core_mask;
    uint32_t authority; // KOS_AUTH_* bits
    uint16_t first_grant;
    uint16_t grant_count;
    uint16_t cap_grant_count;
    uint16_t first_use; // ref[]: the tasks whose endpoints it uses
    uint16_t use_count;
    uint16_t first_watch; // ref[]: the tasks it watches
    uint16_t watch_count;
    uint16_t rsv3[3];
};

struct kos_table_grant
{
    uint8_t kind; // enum kos_grant_kind
    uint8_t flags; // KOS_WINDOW_RO and KOS_WINDOW_UNCACHED
    uint16_t cap_slot; // KOS_SPAWN_DELEGATED_CAP0 + i, or KOS_TABLE_NONE
    uint32_t name;
    uint32_t path;
    uint16_t target; // the served endpoint's task, or the region
    uint16_t window; // a window-kind grant's place in the spawn's window list, or KOS_TABLE_NONE
    uint64_t base; // a window's PHYSICAL base, or a port range's first port
    uint32_t size; // bytes, or ports
    uint16_t line_index;
    uint16_t line;
    uint16_t priv_first;
    uint16_t priv_count;
    uint32_t rsv1;
};

struct kos_table_ref
{
    uint16_t task;
    uint16_t rsv0;
};

struct kos_table_priv
{
    uint16_t offset;
    uint8_t width;
    uint8_t rsv0;
};

struct kos_table_region
{
    uint32_t name;
    uint32_t size;
    uint8_t flags; // KOS_MEM_NOCACHE
    uint8_t rsv0[3];
};

// The emitted table, defined by the composition's system target.
extern struct kos_table_header const* const kickos_table;

// Defined beside the table, one per system target. KickOS::kernel requires it at the link, so an
// image linking no system target fails naming it, and one linking two defines it twice.
extern char const kickos_link_one_system_target;

#ifdef __cplusplus
#define KOS_TABLE_ASSERT(cond, why) static_assert(cond, why)
#else
#define KOS_TABLE_ASSERT(cond, why) _Static_assert(cond, why)
#endif

KOS_TABLE_ASSERT(sizeof(union kos_table_entry) == 8, "the entry is eight bytes (table layout)");

KOS_TABLE_ASSERT(sizeof(struct kos_table_header) == 32, "the header is 32 bytes (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, magic) == 0, "header.magic (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, version) == 4, "header.version (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, flags) == 6, "header.flags (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, ends_task) == 8, "header.ends_task (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, task_count) == 10, "header.task_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, grant_count) == 12, "header.grant_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, ref_count) == 14, "header.ref_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, priv_count) == 16, "header.priv_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, region_count) == 18, "header.region_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, strings_size) == 20, "header.strings_size (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, init_priority) == 24, "header.init_priority (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, rsv0) == 25, "header.rsv0 (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, rsv1) == 26, "header.rsv1 (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_header, rsv2) == 28, "header.rsv2 (table layout)");

KOS_TABLE_ASSERT(sizeof(struct kos_table_task) == 56, "a task is 56 bytes (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, name) == 0, "task.name (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, block) == 4, "task.block (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, entry) == 8, "task.entry (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, driver) == 16, "task.driver (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, rsv1) == 18, "task.rsv1 (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, stack) == 20, "task.stack (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, priority) == 24, "task.priority (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, restart_max) == 25, "task.restart_max (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, flags) == 26, "task.flags (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, core_mask) == 28, "task.core_mask (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, authority) == 32, "task.authority (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, first_grant) == 36, "task.first_grant (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, grant_count) == 38, "task.grant_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, cap_grant_count) == 40, "task.cap_grant_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, first_use) == 42, "task.first_use (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, use_count) == 44, "task.use_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, first_watch) == 46, "task.first_watch (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, watch_count) == 48, "task.watch_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_task, rsv3) == 50, "task.rsv3 (table layout)");

KOS_TABLE_ASSERT(sizeof(struct kos_table_grant) == 40, "a grant is 40 bytes (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, kind) == 0, "grant.kind (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, flags) == 1, "grant.flags (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, cap_slot) == 2, "grant.cap_slot (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, name) == 4, "grant.name (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, path) == 8, "grant.path (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, target) == 12, "grant.target (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, window) == 14, "grant.window (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, base) == 16, "grant.base (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, size) == 24, "grant.size (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, line_index) == 28, "grant.line_index (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, line) == 30, "grant.line (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, priv_first) == 32, "grant.priv_first (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, priv_count) == 34, "grant.priv_count (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_grant, rsv1) == 36, "grant.rsv1 (table layout)");

KOS_TABLE_ASSERT(sizeof(struct kos_table_ref) == 4, "a ref is 4 bytes (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_ref, task) == 0, "ref.task (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_ref, rsv0) == 2, "ref.rsv0 (table layout)");

KOS_TABLE_ASSERT(sizeof(struct kos_table_priv) == 4, "a priv is 4 bytes (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_priv, offset) == 0, "priv.offset (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_priv, width) == 2, "priv.width (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_priv, rsv0) == 3, "priv.rsv0 (table layout)");

KOS_TABLE_ASSERT(sizeof(struct kos_table_region) == 12, "a region is 12 bytes (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_region, name) == 0, "region.name (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_region, size) == 4, "region.size (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_region, flags) == 8, "region.flags (table layout)");
KOS_TABLE_ASSERT(offsetof(struct kos_table_region, rsv0) == 9, "region.rsv0 (table layout)");

#undef KOS_TABLE_ASSERT

#ifdef __cplusplus
}
#endif

#endif
