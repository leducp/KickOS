# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The init's fixed shape. Structural, not configuration: no Kconfig symbol, no defconfig states
# one. Declared in the build, which exports it in the manifest's `init` section beside the init's
# priority (cmake/sched_geometry.cmake) and emits it to C through the generated
# <kickos/sys/init_geometry.h>.

# The bytes of one record in each of the init's blocks, which the init asserts. A watcher's status
# block holds a record per task it watches: its /init/status maps it read-only, the count of tasks
# it watches times this rounded as a shared region is. The private block, the init's own handles,
# capabilities, reservations and instance records, a record per task then one per shared region,
# is never mapped.
set(KICKOS_INIT_STATUS_RECORD_SIZE 8)
set(KICKOS_INIT_PRIVATE_RECORD_SIZE 64)
