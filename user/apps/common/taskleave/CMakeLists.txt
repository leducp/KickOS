# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TRANSLATING AND ABOVE ONE CORE, both load-bearing: the counter it reads only exists where a
# space can be installed on a core at all, and a peer core is the whole hazard. The probe is
# selftest scaffolding, so the selftest posture is required too. main.cc calls kos_aspace_probe
# unconditionally, which is not declared where KICKOS_HAVE_ASPACE is 0.
kickos_add_app_target(taskleave main.cc)
target_link_libraries(taskleave PRIVATE kickos)
kickos_emit_image(taskleave)
