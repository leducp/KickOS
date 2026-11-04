# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_simcon: the host sim's console driver, one thread writing what its endpoint receives to
# host fd 1.
#
# The knobs of tests/integration/check_sim_drvdeath.sh. Under the window and the wedge ones the
# task's entry holds nothing and the receiver is a second thread; its START spawns a window
# thread on the register window the host mapped at boot, which a declaration cannot name.
if(KICKOS_SIMCON_IRQ_WEDGE)
  kickos_add_driver(simcon BLOCK 64 INIT block_init BARRIER 1 READY SIMCON_READY_OFFSET
                    POSTURE handover CONSOLE START simcon_console_start AUTHORITY memory
                    THREAD entry ENTRY simconsole_entry_thread ARG block
                    THREAD service ENTRY simconsole_driver CAPS ep:wait)
elseif(KICKOS_SIMCON_WINDOW_THREAD)
  kickos_add_driver(simcon BLOCK 64 INIT block_init BARRIER 1 READY SIMCON_READY_OFFSET
                    POSTURE handover CONSOLE START simcon_console_start AUTHORITY memory
                    THREAD entry ENTRY simconsole_entry_thread ARG block
                    THREAD service ENTRY simconsole_driver CAPS ep:wait)
else()
  kickos_add_driver(simcon BLOCK none BARRIER none POSTURE handover CONSOLE START simcon_console_start
                    THREAD service ENTRY simconsole_driver CAPS ep:wait)
endif()

# The driver exits after N served messages, 0 serving forever.
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
# A thread takes the register window and never sets the readiness latch the receiver waits on.
if(KICKOS_SIMCON_IRQ_WEDGE)
  target_compile_definitions(kickos_simcon PRIVATE KICKOS_SIMCON_IRQ_WEDGE=1)
endif()
