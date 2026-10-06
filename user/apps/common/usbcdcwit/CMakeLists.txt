# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Sustained-output witness for a published USB CDC console, the packaged driver its composition
# names as stdout. The parent builds it only where the kernel brings up the USB console's clock.
add_executable(usbcdcwit main.cc)
kickos_app_system(usbcdcwit COMPOSITION systems/${KICKOS_BOARD}.yaml)
kickos_emit_image(usbcdcwit)
