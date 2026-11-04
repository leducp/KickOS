# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The type ceilings of the kernel's pools. ABI, not configuration: no Kconfig symbol, no defconfig
# states one. Emitted to C through the generated <kickos/config/pool_abi.h>, which the kernel
# archives and every system's generated pools TU include, so each encoding width is fixed by a
# type and never by a pool's count. The export manifest carries the per-pool ceilings, which the
# composer refuses a sum of budgets past, naming the ceiling.

# The most thread-pool slots a system may have. A task counts its members in one byte, and a
# slot's kill tag is kept in one byte as a task's creator.
set(KICKOS_THREAD_SLOT_CEILING 254)

# The index half of the handle every generational pool shares. The generation takes the rest of
# the 32 bits.
set(KICKOS_HANDLE_INDEX_BITS 16)

# The most task records: a free-list link and a creator's list link are an index plus one in a
# byte, and Task::image an image index.
set(KICKOS_TASK_SLOT_CEILING 254)

# The most slots a generational pool may have: an index plus one is a uint16_t link below the
# value that marks a live slot, and the all-ones index is never seated.
set(KICKOS_SLOT_POOL_CEILING 65534)

# The most IRQ bindings: a binding's chain link is an index plus one in a byte.
set(KICKOS_IRQ_BINDING_CEILING 254)

kickos_emit_geometry(kickos/config/pool_abi.h KICKOS_THREAD_SLOT_CEILING KICKOS_HANDLE_INDEX_BITS
  KICKOS_TASK_SLOT_CEILING KICKOS_SLOT_POOL_CEILING KICKOS_IRQ_BINDING_CEILING)
