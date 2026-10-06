# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 console-reclaim test: its composition names the test-owned testusic as stdout, and main
# prints through it, has it scramble the channel and panics; the panic must still reach the wire.
# Judged from a silicon capture.
if(NOT KICKOS_TEST_DRIVERS)
  message(STATUS "KickOS: conreclaim skipped: its composition names testusic, which "
    "-DKICKOS_TEST_DRIVERS=ON builds")
  return()
endif()

add_executable(conreclaim main.cc)
kickos_app_system(conreclaim COMPOSITION system.yaml)
kickos_emit_image(conreclaim)
kickos_app_judge(conreclaim tests/integration/check_conreclaim.sh)
