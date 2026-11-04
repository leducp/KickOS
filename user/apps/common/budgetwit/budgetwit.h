// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What budgetwit's main and its peers agree on: the budgets system.yaml states, the roles'
// figures, and the reports a role sends main.

#ifndef KICKOS_USER_APPS_COMMON_BUDGETWIT_BUDGETWIT_H
#define KICKOS_USER_APPS_COMMON_BUDGETWIT_BUDGETWIT_H

#include <stddef.h>
#include <stdint.h>

// The budgets of system.yaml.
constexpr int HOG_THREADS = 4;
constexpr int HOG_ENDPOINTS = 3;
constexpr int SIBLING_THREADS = 4;
constexpr int SWEEPER_THREADS = 3;
constexpr size_t RAM_BYTES_BUDGET = 16384;
constexpr int RAM_BLOCKS_BUDGET = 3;
constexpr size_t RELAUNCH_BLOCK = 0x4000;
constexpr int RELAUNCH_BLOCKS = 2;

// The widths declaration's tables: its threads' width, its CAP_SLOTS budget, the widths its
// first workers ask, and the replacement's, wider than any of them.
constexpr uint16_t WIDTHS_WIDTH = 8;
constexpr uint16_t WIDTHS_SLOTS = 48;
constexpr uint16_t WIDTHS_ASK[3] = {16, 8, 16};
constexpr uint16_t WIDTHS_WIDE = WIDTHS_SLOTS - WIDTHS_WIDTH - 8;
// The narrow workers that fill the budget again at the declared width.
constexpr int WIDTHS_SMALL = WIDTHS_SLOTS / WIDTHS_WIDTH - 1;

// A spawn loop's bound, past every budget and every pool a witness composition sizes.
constexpr int SPAWN_CAP = 64;
// The sweeper's children, each one priority above the last from SWEEP_CHILD_PRIO, all below
// its entry's 12.
constexpr int SWEEP_CAP = 8;
constexpr uint8_t SWEEP_CHILD_PRIO = 3;
constexpr uint8_t PEER_PRIO = 11;
constexpr uint32_t CALL_US = 2000000u;
constexpr int SIBLING_SPAWNS = SIBLING_THREADS - 1;
// The restarts the arena-drain loop takes at most.
constexpr int RELAUNCH_CAP = 1024;

// The pager's figures: the granule, how far a probe looks, and the run it leaves in main's
// space as it dies.
constexpr size_t PAGE = 4096;
constexpr uint32_t PAGER_PROBE_PAGES = 256;
constexpr uint32_t ELSEWHERE_PAGES = 4;

struct Report
{
    int32_t count;
    int32_t rc;
    int32_t extra;
    int32_t rc2;
    int32_t paused;
};

// What the widths role saw: each first worker's free slots seated and again after the
// refusals, the refusals, the replacement's spawn and its free slots, and the narrow refill.
struct WidthsReport
{
    int32_t seated[3];
    int32_t kept[3];
    int32_t over;
    int32_t floor;
    int32_t wide;
    int32_t wide_seated;
    int32_t small;
    int32_t small_rc;
};

struct PagesReport
{
    uint32_t largest;
    int32_t over;
    int32_t mapped;
    uint32_t after_map;
    int32_t elsewhere;
    uint32_t won;
};

#endif
