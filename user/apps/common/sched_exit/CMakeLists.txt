# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(sched_exit main.cc)
target_link_libraries(sched_exit PRIVATE kickos)
kickos_emit_image(sched_exit)

# The wait-until-last phase can only complete in an image whose service list spawns no driver
# threads: a driver never exits, so kernel().live never reaches 1 and root parks forever.
# main() skips that phase when SCHED_EXIT_SERVICE_THREADS is 1, and the gate registrations
# stay conditioned on the same service list.
set(_sched_exit_services 0)
if(KICKOS_SERVICE_LIST AND NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  set(_sched_exit_services 1)
endif()
target_compile_definitions(sched_exit PRIVATE
  SCHED_EXIT_SERVICE_THREADS=${_sched_exit_services})
