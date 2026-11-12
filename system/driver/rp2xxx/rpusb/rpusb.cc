// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 / RP2350 USB CDC-ACM console driver on <kickos/sys/usb_cdc_service.h>. One backend
// for both chips; the controller itself is rp_usb_dev.h.
//
// Register facts come from the RP2350 datasheet section 12.7 and the RP2040 datasheet
// section 4.1 via rp_usb_regs.h.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/declared/rpusb.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/usb_cdc_service.h>

#include "rp_usb_dev.h"
#include "rp_usb_regs.h"

#include <stdint.h>

namespace drv = kickos::driver;
namespace reg = kickos::rpusb::reg;
namespace usb = kickos::usb;

namespace
{
    void rpusb_irq_thread(void* arg)
    {
        usb::Shared* sh = static_cast<usb::Shared*>(arg);
        kickos::rpusb::RpUsb dev;
        dev.sh = sh;
        dev.dpram = reg::DPRAM_BASE;
        dev.regs = reg::REGS_BASE;
        usb::Cdc<kickos::rpusb::RpUsb> cdc(dev, sh);
        if (cdc.bring_up() != 0)
        {
            // Leaving the ready latch clear is the report.
            drv::trap();
        }
        usb::irq_loop(cdc, sh); // never returns
    }

    void rpusb_service_thread(void* arg)
    {
        usb::console_serve_loop(static_cast<usb::Shared*>(arg));
    }

    int block_init(void* blk, struct kos_driver_instance const*)
    {
        // A bare Shared: the CDC class carries no device config.
        usb::shared_init(static_cast<usb::Shared*>(blk));
        return 0;
    }

    constexpr drv::Descriptor k_desc = KICKOS_DRIVER_DESCRIPTOR;

    static_assert(drv::valid(k_desc), "the rpusb descriptor is not a well-formed driver shape");
    static_assert(usb::desc_ok(k_desc), "the rpusb cap positions do not match KOS_USB_CAP_*");
}

extern "C"
{

int rpusb_console_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}

}
