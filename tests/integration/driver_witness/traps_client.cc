// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The driver witness's client of the driver traps: it calls until the driver is refused for good.

#include "clients.h"

int main(int, char**)
{
    driver_witness::call_until_refused("/svc/traps", "traps");
}
