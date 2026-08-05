// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-board service list: an ordered set of userspace services the default init
// brings up BEFORE the app's main. This is the sole userspace-console bring-up path: the
// console is just the first KOS_SVC_CONSOLE entry. Per-instance config travels as DATA in
// kos_service_cfg, not as literals in the driver TU. Lives in the kickos_system library;
// keep it dependency-free (shared verbatim by the init body and every per-board provider
// TU).
//
// HARD RULE: NO libc stdio anywhere in any service start(). Between an endpoint
// publish and the driver's first recv, the publisher holds the only WAIT cap, so a
// stray printf (which routes through _write to the console endpoint) self-deadlocks
// in the rendezvous. Diagnostics in every start() use kos::print (the RTT / kernel
// debug path), never stdio.

#ifndef KICKOS_SYS_SERVICE_H
#define KICKOS_SYS_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// kos_service_cfg.kind: the service class. start() rejects a cfg authored for the
// wrong class.
enum kos_svc_kind
{
    KOS_SVC_CONSOLE = 0,
    KOS_SVC_SPI = 1,
    KOS_SVC_I2C = 2,
    // A general UART port serving the <kickos/sys/uart.h> wire ABI: RX as well as TX,
    // buffered rather than polled, and NOT a console handover (KOS_SVC_CONSOLE is the
    // write-only stdout sink the kernel gives its own console away to).
    KOS_SVC_UART = 3
};

// Per-instance bring-up config. POD, no libc, no chip headers. start() reads base/window
// from here, never from a literal. name/mmio_base are pointer-width, so sizeof is 32 B on
// LP64 and 24 B on ILP32 (the static_assert below tracks both).
struct kos_service_cfg
{
    char const* name;        // driver thread name (diagnostics); e.g. "k64uart", "spi0"
    uintptr_t mmio_base;     // controller register block base -> the spawn MMIO grant + arg
    uint32_t mmio_window;    // grant window size in bytes (arch-encodable; per-chip granule)
    uint32_t hz;             // target clock (SPI/I2C bit clock, UART baud); driver rounds DOWN
    uint16_t addr;           // I2C 7/10-bit slave address slot; 0 when unused
    uint8_t prio;            // driver thread priority
    uint8_t kind;            // enum kos_svc_kind; start() rejects a mismatched cfg
    uint8_t rsv[4];          // reserved zero (keeps the struct pointer-aligned, room to grow)
};

// One service instance to bring up. start() is the one-shot handover choreography
// (create endpoint, make it reachable, grant the MMIO window + WAIT cap, spawn the
// unprivileged driver, drop the parent's cap); it returns 0 on success or a negative
// code on failure, and the runner short-circuits on nonzero. start == NULL is a
// skipped slot; cfg is NULL only for a config-less service.
struct kos_service_bringup
{
    int (*start)(struct kos_service_cfg const* cfg);
    struct kos_service_cfg const* cfg;
};

struct kos_service_list
{
    struct kos_service_bringup const* services;
    uint32_t count;
};

// The selected board's service list. EXACTLY ONE definition links per image, chosen
// by the KICKOS_SERVICE_LIST CMake target (default kickos_services_none, count = 0).
extern struct kos_service_list const kickos_board_services;

#ifdef __cplusplus
static_assert(sizeof(struct kos_service_cfg) == 2 * sizeof(void*) + 16,
              "kos_service_cfg layout drift (2 pointers + 16 fixed bytes)");
#else
_Static_assert(sizeof(struct kos_service_cfg) == 2 * sizeof(void*) + 16,
               "kos_service_cfg layout drift (2 pointers + 16 fixed bytes)");
#endif

#ifdef __cplusplus
}
#endif

#endif
