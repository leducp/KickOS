# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Packaged drivers the driver witnesses compose (docs/design-m10-target.md, section 8): three
# with no window and no line, testpl011, a console driver over a PL011's window that only the
# console restart witness on QEMU `virt` composes, testline, whose lines only the line witness
# on QEMU `virt` binds and whose raiser holds line 0 for SIGNAL alone, and testusic, a console
# driver over an XMC4800 USIC channel that scrambles it on request, which only conreclaim
# composes. Built under KICKOS_TEST_DRIVERS alone, which only
# a witness preset or an operator's configure sets: tests/integration/check_test_drivers_absent.sh
# holds every other package to none.

kickos_add_driver(testexits SOURCES testexits.cc BLOCK none BARRIER none POSTURE retain START testexits_start
                  THREAD service ENTRY service CAPS ep:wait)

kickos_add_driver(testtraps SOURCES testtraps.cc BLOCK none BARRIER none POSTURE retain START testtraps_start
                  THREAD entry ENTRY entry
                  THREAD worker ENTRY worker CAPS ep:wait)

kickos_add_driver(testfails SOURCES testfails.cc BASE 0x1000u BLOCK none BARRIER none POSTURE retain
                  START testfails_start
                  THREAD service ENTRY service CAPS ep:wait)

# Bound to a window and lines virt_arm64's chip file names, as no other emulated chip's does.
if(KICKOS_CHIP STREQUAL "virt_arm64")
  kickos_add_driver(testpl011 SOURCES testpl011.cc WINDOWS regs BLOCK none BARRIER none
                    POSTURE handover CONSOLE START testpl011_start
                    THREAD service ENTRY service WINDOW regs CAPS ep:wait)

  kickos_add_driver(testline SOURCES testline.cc LINES routed:edge held:edge
                    BLOCK none BARRIER none POSTURE retain START testline_start
                    THREAD irq PRIORITY 1 ENTRY irq ARG line0_index
                           CAPS notify:wait routed:wait held:wait
                    THREAD service ENTRY service CAPS ep:wait
                    THREAD raiser ENTRY raiser CAPS routed:signal)
endif()

# The USIC backend uart_usic.cc is the one xmcuartirq compiles, so an image links one of the two.
if(KICKOS_CHIP STREQUAL "xmc4800")
  kickos_add_driver(testusic SOURCES testusic.cc "${PROJECT_SOURCE_DIR}/system/driver/xmc4800/uart_usic.cc"
                    CLASS kickos_class_xmc4800 REGDIR arch/arm/chip/xmc4800
                    WINDOWS regs BLOCK none BARRIER none POSTURE handover CONSOLE START testusic_start
                    THREAD service ENTRY service ARG window WINDOW regs CAPS ep:wait)
endif()
