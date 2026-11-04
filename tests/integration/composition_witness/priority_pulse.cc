// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The priority witness's pulse: it sleeps and ends, and the init restarts it once.

#include "priority.h"

int main(int, char**)
{
    kos_sleep_ns(priority_witness::PULSE_NS);
}
