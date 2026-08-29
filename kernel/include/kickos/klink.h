// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// On `ld -m i386pep` (the PE32+ UEFI image) gcc reaches a weak undefined symbol GOT-indirect,
// the link is clean and the image faults later at a plausible address
// (tools/check-x86_64-no-got.sh). That target states every such symbol and takes the hidden arm.

#ifndef KICKOS_KLINK_H
#define KICKOS_KLINK_H

#if KICKOS_LINKER_WEAK_UNDEF
#define KICKOS_LINK_OPTIONAL __attribute__((weak))
#else
#define KICKOS_LINK_OPTIONAL __attribute__((visibility("hidden")))
#endif

#endif
