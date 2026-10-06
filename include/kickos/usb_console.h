// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_USB_CONSOLE_H
#define KICKOS_USB_CONSOLE_H

// Defined by every system target: 1 where its stdout is a packaged driver marked USB_DEVICE, so
// chip init brings the USB clock tree up in that image and in no other. The bench reads it from
// the image to capture over the device's own ACM.
#ifdef __cplusplus
extern "C" char const kickos_usb_device_console;
#else
extern char const kickos_usb_device_console;
#endif

#endif
