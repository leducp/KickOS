# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# These standalone programs and Python checks do not require GTest.
add_subdirectory(telemetry)

if(KICKOS_BUILD_UNIT_TESTS)
  # Independent targets; consumers of shared fixture libraries follow their providers.
  foreach(_unit IN ITEMS
      ampwindow benchdist capreply captable consoleline consoleown consolepace consoletx
      consoletxsmp
      deathspace drvbringup fastrefuse faultline frame fusedwake grantnocache irqquiesce kstring
      ktime mapexec mapfence mpuskip mtimeconv placeadmit ramowner reschedowed slotpool taskbudget tlscarve
      uartclass vrange)
    add_subdirectory(${_unit})
  endforeach()

  # Define providers first so kickos_add_unit_test can read their configuration
  # macros and avoid conflicts with preset definitions.
  add_subdirectory(kfixture)
  foreach(_unit IN ITEMS
      cancelkind capbadge capprobe capsweep faultband irqbindown parkresult priodonor rangecheck rootslot
      schedwake slayhook taskdeath taskpool wildstack)
    add_subdirectory(${_unit})
  endforeach()

  add_subdirectory(exitquiesce)
  foreach(_unit IN ITEMS heldassert migrateask slaypeer)
    add_subdirectory(${_unit})
  endforeach()
endif()
