// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PE32+ requires hidden, linker-defined optional symbols to avoid unsupported
// GOT references. Other targets allow weak undefined symbols.

#ifndef KICKOS_KLINK_H
#define KICKOS_KLINK_H

#if KICKOS_LINKER_WEAK_UNDEF
#define KICKOS_LINK_OPTIONAL __attribute__((weak))
#else
#define KICKOS_LINK_OPTIONAL __attribute__((visibility("hidden")))
#endif

#endif
