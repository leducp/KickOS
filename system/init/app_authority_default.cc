// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The fallback kickos_app_authority: what an app gets when it declares no mask of its
// own. Spawn worker threads, and end the system when main returns.
//
// This TU MUST define exactly one symbol. An app that expands KICKOS_APP_AUTHORITY
// resolves the reference in its own object, so this member is never extracted; a second
// symbol here would extract it anyway and collide with that definition.

#include <kickos/sys/abi.h> // KOS_AUTH_*
#include <kickos/sys/init.h>

extern "C" uint8_t kickos_app_authority(void)
{
    return static_cast<uint8_t>(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM);
}
