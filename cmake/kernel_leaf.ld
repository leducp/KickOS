/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * An implicit linker script KickOS::kernel links. A system target defines KICKOS_USER_HEAP_SIZE,
 * which leaves this unread, and its asserts refuse a value other than its heap. Only an image
 * linking no system target takes the 0, so its link gets past the chip script's heap and fails
 * on the kickos_link_one_system_target KickOS::kernel requires, naming it.
 */
PROVIDE(KICKOS_USER_HEAP_SIZE = 0);
