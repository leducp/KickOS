# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Packaged drivers the driver witnesses compose (docs/design-m10-target.md, section 8): three
# with no window and no line, testpl011, a console driver over a PL011's window that only the
# console restart witness on QEMU `virt` composes, testline, whose lines only the line witness
# on QEMU `virt` binds, and testusic, a console driver over an XMC4800 USIC channel that scrambles
# it on request, which only conreclaim composes. Built under KICKOS_TEST_DRIVERS alone, which only
# a witness preset or an operator's configure sets: tests/integration/check_test_drivers_absent.sh
# holds every other package to none.

kickos_add_driver(testexits SOURCES testexits.cc
                  THREADS service:0:default:1:0 RECEIVER service
                  BLOCK none POSTURE retain BARRIER none START testexits_start)

kickos_add_driver(testtraps SOURCES testtraps.cc
                  THREADS entry:0:default:0:0 worker:0:default:1:0 RECEIVER worker
                  BLOCK none POSTURE retain BARRIER none START testtraps_start)

kickos_add_driver(testfails SOURCES testfails.cc
                  THREADS service:0:default:1:0 RECEIVER service
                  BLOCK none POSTURE retain BARRIER none START testfails_start)

# Bound to a window and lines virt_arm64's chip file names, as no other emulated chip's does.
if(KICKOS_CHIP STREQUAL "virt_arm64")
  kickos_add_driver(testpl011 SOURCES testpl011.cc
                    THREADS service:0:default:1:0 RECEIVER service WINDOWS regs
                    BLOCK none POSTURE handover BARRIER none START testpl011_start CONSOLE)

  kickos_add_driver(testline SOURCES testline.cc
                    THREADS irq:1:default:3:0 service:0:default:1:0 RECEIVER service LINES routed held NOTIFY
                    BLOCK none POSTURE retain BARRIER none START testline_start)
endif()

# The USIC backend uart_usic.cc is the one xmcuart and xmcuartirq compile, so an image links one
# of the three.
if(KICKOS_CHIP STREQUAL "xmc4800")
  kickos_add_driver(testusic SOURCES testusic.cc "${PROJECT_SOURCE_DIR}/system/driver/xmc4800/uart_usic.cc"
                    CLASS kickos_class_xmc4800 REGDIR arch/arm/chip/xmc4800
                    THREADS service:0:default:1:0 RECEIVER service WINDOWS regs
                    BLOCK none POSTURE handover BARRIER none START testusic_start CONSOLE)
endif()
