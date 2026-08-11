// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The model a mocked UART channel runs on, reached through kos_uart_config::base: on silicon
// that is the granted register window, here the address of one of these.

#ifndef KICKOS_SELFTEST_UART_MOCK_H
#define KICKOS_SELFTEST_UART_MOCK_H

#include <stdint.h>

enum
{
    KOS_UART_MOCK_CAP = 32
};

struct kos_uart_mock
{
    // Bytes the device accepted, in order.
    unsigned char tx[KOS_UART_MOCK_CAP];
    uint32_t tx_len;

    // Bytes waiting on the wire, handed out by kos_uart_read.
    unsigned char rx[KOS_UART_MOCK_CAP];
    uint32_t rx_len;
    uint32_t rx_pos;

    // Bytes the transmitter will still accept before it starts refusing: FIFO room, so a
    // test can force a short write.
    uint32_t tx_room;

    // The rate the divider is modelled as producing. Deliberately NOT the requested baud, so
    // an echo is distinguishable from a measurement.
    int32_t rate;

    // Set by the transmit path, never by the test: 1 while the TX source is armed.
    uint32_t tx_armed;

    uint32_t flushes;
    uint32_t closes;
    uint32_t opened;
};

#endif
