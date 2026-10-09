# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TRANSLATING AND ABOVE ONE CORE, both load-bearing: the space count it reads exists only where
# a space can be installed on a core at all, and a peer core is the whole hazard.
add_executable(taskleave main.cc)
target_link_libraries(taskleave PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(taskleave)
