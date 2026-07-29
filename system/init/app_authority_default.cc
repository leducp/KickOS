// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The fallback kickos_app_authority: what an app gets when it declares no mask of its
// own. Spawn worker threads, and end the system when main returns.
//
// ALONE IN THIS TU, and that is the whole mechanism -- no weak symbol anywhere. An app
// that expands KICKOS_APP_AUTHORITY defines the symbol strongly in its own object, so
// this archive member is never extracted and there is no second definition to resolve;
// an app that declares nothing leaves the reference undefined and the linker pulls this
// member in. Adding any other symbol here would break that: the member would be
// extracted for the other symbol and collide with the app's definition.
//
// Weak was tried and rejected. GCC carries a weak attribute from any declaration onto
// the definition in the same translation unit, so declaring the symbol weak in init.h
// made every app's override weak too -- two weak definitions, resolved by link order
// rather than by strong-beats-weak, which is a silent fallback to this mask on a
// relink. Archive-member-on-demand has no such ambiguity and no per-toolchain variation.

#include <kickos/sys/abi.h> // KOS_AUTH_*
#include <kickos/sys/init.h>

extern "C" uint8_t kickos_app_authority(void)
{
    return static_cast<uint8_t>(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM);
}
