// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default kickos_init_entry provider. A DISTINCT TU from the run body so a
// delegating override that references kickos_default_init_run does not also drag in
// this default kickos_init_entry.

#include <kickos/sys/init.h>

extern "C" int kickos_init_entry(int argc, char** argv)
{
    return kickos_default_init_run(argc, argv);
}
