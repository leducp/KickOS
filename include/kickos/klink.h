// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_KLINK_H
#define KICKOS_KLINK_H

// A bound the image's own linker script states. The reference is STRONG: a script that states
// none fails the link naming the symbol, and a window that is legitimately empty is stated as
// start == end, so absent and empty stay distinct at link time.
//
// On PE32+ an undefined external reference stays GOT-indirect whatever -fvisibility says, and
// ld -m i386pep leaves the load in place, so the address read back would be the bytes AT the
// symbol; tools/check-x86_64-no-got.sh refuses a survivor.
#define KICKOS_LINK_BOUND __attribute__((visibility("hidden")))

// A symbol an app translation unit may or may not define. Hidden visibility does NOT save a
// weak undefined reference on PE32+, measured both ways: gcc reaches it GOT-indirect at any
// visibility. So where the linker resolves no weak undefined symbol the reference is strong
// instead, and that target's images must supply the definition.
#if KICKOS_LINKER_WEAK_UNDEF
#define KICKOS_LINK_OPTIONAL __attribute__((weak))
#else
#define KICKOS_LINK_OPTIONAL __attribute__((visibility("hidden")))
#endif

#endif
