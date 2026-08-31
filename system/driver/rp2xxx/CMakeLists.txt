# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Drivers shared by the RP2040 and the RP2350. The directory is named for the pair
# rather than for a chip because the USB device blocks are the same IP; a driver that is
# genuinely per-chip belongs under system/driver/<chip>/ like every other one.
add_subdirectory(rpusb)
