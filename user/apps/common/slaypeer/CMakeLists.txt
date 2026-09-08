# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ABOVE ONE CORE, which is the whole hazard: at one core a victim cannot be RUNNING anywhere
# but where the slayer is, and thread_cancel_kind hands it its own escalation on the way out.
# main.cc pins by core mask and reads a printf formatter, neither of which is there below two
# cores or without a libc.
kickos_add_app_target(slaypeer main.cc)
target_link_libraries(slaypeer PRIVATE kickos)
kickos_emit_image(slaypeer)
