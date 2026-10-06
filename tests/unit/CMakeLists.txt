# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# These standalone programs and Python checks do not require GTest.
add_subdirectory(telemetry)

if(KICKOS_BUILD_UNIT_TESTS)
  # Independent targets; consumers of shared fixture libraries follow their providers.
  foreach(_unit IN ITEMS
      ampconsole amphold ampshare ampwindow benchdist c6apm capreply captable consoleline consoleown consolepace consoletx
      consoletxsmp dcacheplan
      deathspace drvbringup fastrefuse faultline frame fusedwake grantlookup grantnocache initwalk irqquiesce kstring
      ktime mapexec mapfence mpuskip mtimeconv newlibstubs placeadmit presync probecatch ramowner regionsync
      reschedowed rp2350gate slotpool
      stdoutwrite taskbudget uartclass vrange windowholder)
    add_subdirectory(${_unit})
  endforeach()

  # Define providers first so kickos_add_unit_test can read their configuration
  # macros and avoid conflicts with preset definitions.
  add_subdirectory(kfixture)
  foreach(_unit IN ITEMS
      cancelkind capbadge capprobe capsweep consolepub faultband irqbindown parkresult priodonor rangecheck rootslot
      rrslice schedwake slayhook taskdeath taskpool wildstack)
    add_subdirectory(${_unit})
  endforeach()

  add_subdirectory(exitquiesce)
  foreach(_unit IN ITEMS consolehold heldassert irqlinecore migrateask parkdeath placement schedring
      slaypeer tlscarve)
    add_subdirectory(${_unit})
  endforeach()
endif()
