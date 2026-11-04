// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The driver witness's client of the driver fails.
// Never started: the init starts a task once the servers it uses serve, and testfails never does.

#include "clients.h"

int main(int, char**)
{
    driver_witness::call_until_refused("/svc/fails", "fails");
}
