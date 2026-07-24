// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default init's service bring-up step: walk the selected board's service list
// (see <kickos/sys/service.h>) in array order before the app's main. Each entry's
// start() is a one-shot handover; a nonzero return short-circuits so the app never
// runs against a half-brought-up board. A board with an empty list (count = 0) is a
// no-op. This generalizes the single-console hook (console_bringup.cc).
//
// HARD RULE (see <kickos/sys/service.h>): no libc stdio here. Every start() holds
// the only WAIT cap between publish and its driver's first recv, so a printf would
// self-deadlock in the rendezvous. This TU touches no console at all.

#include <kickos/sys/service.h>
#include <kickos/sys/init.h>

extern "C" int kickos_service_list_run(void)
{
    for (uint32_t i = 0; i < kickos_board_services.count; ++i)
    {
        struct kos_service_bringup const* s = &kickos_board_services.services[i];
        if (s->start == nullptr)
        {
            continue; // skipped slot
        }
        int const rc = s->start(s->cfg);
        if (rc != 0)
        {
            return rc; // loud short-circuit, no app run
        }
    }
    return 0;
}
