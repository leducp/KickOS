# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_simcon: the host sim's console driver, one thread writing what its endpoint receives to
# host fd 1.
kickos_add_driver(simcon THREADS service:0:default:1:0 RECEIVER service
                  BLOCK none POSTURE handover BARRIER none START simcon_console_start CONSOLE)

# Make the console driver DIE after N served messages, so driver-death reclaim has a witness
# that needs no board (tests/integration/check_sim_drvdeath.sh). 0 = serve forever, which is
# every other posture.
if(KICKOS_SIMCON_EXIT_AFTER)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_EXIT_AFTER=${KICKOS_SIMCON_EXIT_AFTER})
endif()
# The other death shape: the driver dies BEFORE it ever receives, so the handover probe is what
# has to notice.
if(KICKOS_SIMCON_DIE_AT_BRINGUP)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_DIE_AT_BRINGUP=1)
endif()
# The two-thread shape: a SECOND driver thread holds the console's register window and parks in
# kos_irq_wait, so the endpoint's last receiver dying no longer means the device is free. The
# gate app carries the same knob (user/apps/common/drvdeath).
if(KICKOS_SIMCON_WINDOW_THREAD)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_WINDOW_THREAD=1)
endif()
# The READY-TIMEOUT shape: the IRQ thread takes the register window and never sets the ready
# flag, so the bounded wait expires. Its bring-up is ordered like the silicon drivers' (publish,
# claim, IRQ thread, wait, service thread), which is what makes the timeout diagnostic reportable.
if(KICKOS_SIMCON_IRQ_WEDGE)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_IRQ_WEDGE=1)
endif()
