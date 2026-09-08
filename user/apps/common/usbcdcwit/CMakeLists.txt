# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Sustained-output witness for a published USB CDC console. Without a CDC driver cap 0 is the
# kernel console and the app measures nothing, so the parent builds it only for a service list
# that provides one.
kickos_add_app_target(usbcdcwit main.cc)
target_link_libraries(usbcdcwit PRIVATE kickos)
kickos_emit_image(usbcdcwit)
