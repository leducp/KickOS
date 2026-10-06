// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread-local object, which every stack carves a block for.

int kickos_probe_tls_read(void);

static _Thread_local int g_probe_tls[16];

int kickos_probe_tls_read(void)
{
    return g_probe_tls[0];
}
