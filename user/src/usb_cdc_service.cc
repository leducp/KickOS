// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The service-thread half of the USB CDC-ACM layer. The contract for each function is stated
// at its declaration in <kickos/sys/usb_cdc_service.h>; the request bodies are the shared
// ones of <kickos/sys/console_service.h>, and the Cdc class and irq_loop are templated over
// the controller and stay in that header.

#include <kickos/sys/usb_cdc_service.h>

#include <stdlib.h>

namespace kickos::usb
{

void shared_init(Shared* s)
{
    console::shared_init<Transport>(s);
}

void console_serve_loop(Shared* sh)
{
    console::console_serve_loop<Transport>(sh);
    exit(0);
}

}
