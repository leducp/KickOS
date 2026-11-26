// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_STDOUT_PORT_H
#define KICKOS_STDOUT_PORT_H

// The AMP port every thread's standard output is seated to at boot, or KICKOS_STDOUT_PORT_NONE.
#define KICKOS_STDOUT_PORT_NONE 255u
#ifdef __cplusplus
extern "C" unsigned char const kickos_stdout_port;
#else
extern unsigned char const kickos_stdout_port;
#endif

#endif
