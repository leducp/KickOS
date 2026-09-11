# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The host gates. The root adds this directory on the sim under KICKOS_BUILD_TESTS, so
# nothing below restates either condition.

# Two host programs and a python checker, no GTest, so it is outside the unit-test knob.
add_subdirectory(telemetry)

if(KICKOS_BUILD_UNIT_TESTS)
  # Nothing in this list links a library another directory defines. One that does belongs
  # under its provider below instead.
  foreach(_unit IN ITEMS
      ampwindow capreply captable consoleown consoletx deathspace drvbringup fastrefuse
      faultline frame grantnocache irqquiesce kstring ktime mapexec placeadmit ramowner
      reschedowed slotpool taskbudget tlscarve uartclass vrange)
    add_subdirectory(${_unit})
  endforeach()

  # kickos_add_unit_test reads KICKOS_POSTURE off each library it is handed, at the moment
  # the call runs. A consumer added before its provider finds no target, so it keeps the
  # preset's own posture macros and takes the library's PUBLIC ones on top: the configure
  # succeeds and the translation unit is compiled under both.
  add_subdirectory(kfixture)
  foreach(_unit IN ITEMS
      cancelkind capprobe capsweep faultband parkresult rootslot schedwake slayhook
      taskdeath wildstack)
    add_subdirectory(${_unit})
  endforeach()

  add_subdirectory(exitquiesce)
  foreach(_unit IN ITEMS migrateask slaypeer)
    add_subdirectory(${_unit})
  endforeach()
endif()
