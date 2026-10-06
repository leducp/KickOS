# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_simcon: the host sim's console driver, one thread writing what its endpoint receives to
# host fd 1.
kickos_add_driver(simcon THREADS service:0:default:1:0 RECEIVER service
                  BLOCK none POSTURE handover BARRIER none START simcon_console_start CONSOLE)

# The knobs of tests/integration/check_sim_drvdeath.sh. The driver exits after N served messages,
# 0 serving forever.
if(KICKOS_SIMCON_EXIT_AFTER)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_EXIT_AFTER=${KICKOS_SIMCON_EXIT_AFTER})
endif()
# The driver dies before it ever receives.
if(KICKOS_SIMCON_DIE_AT_BRINGUP)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_DIE_AT_BRINGUP=1)
endif()
# A second thread holds the console's register window past the receiver's death. The gate app
# carries the same knob (user/apps/common/drvdeath).
if(KICKOS_SIMCON_WINDOW_THREAD)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_WINDOW_THREAD=1)
endif()
# An IRQ thread takes the register window and never sets the ready flag.
if(KICKOS_SIMCON_IRQ_WEDGE)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_IRQ_WEDGE=1)
endif()
