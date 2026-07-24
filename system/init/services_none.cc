// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The universal-default service-list provider: no board services, so the default
// init brings nothing up (the console stays whatever the console hook left it).
// Selected by KICKOS_SERVICE_LIST on every board that ships no service list. count =
// 0 is the "nothing to bring up" signal, not a failure.

#include <kickos/sys/service.h>

extern "C"
{
    struct kos_service_list const kickos_board_services = { nullptr, 0 };
}
