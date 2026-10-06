// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_USB_CONSOLE_H
#define KICKOS_USB_CONSOLE_H

#include <kickos/klink.h>

// Defined by a USB device console driver, in the translation unit of its start function, so
// chip init brings the USB clock tree up in an image that links one and in no other. The bench
// reads it from the image to capture over the device's own ACM.
#ifdef __cplusplus
extern "C" KICKOS_LINK_OPTIONAL char const kickos_usb_device_console;
#else
KICKOS_LINK_OPTIONAL extern char const kickos_usb_device_console;
#endif

#endif
