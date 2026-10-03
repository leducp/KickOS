# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Packaged drivers the driver restart witnesses compose (docs/design-m10-target.md, section 8):
# three with no window and no line, and testpl011, a console driver over a PL011's window that
# only the console restart witness on QEMU `virt` composes. Built under KICKOS_TEST_DRIVERS alone,
# which only a witness preset sets: tests/integration/check_test_drivers_absent.sh holds every
# other package to none.

kickos_add_driver(testexits SOURCES testexits.cc
                  THREADS service:0:default:1:0 RECEIVER service
                  BLOCK none POSTURE retain BARRIER none START testexits_start)

kickos_add_driver(testtraps SOURCES testtraps.cc
                  THREADS entry:0:default:0:0 worker:0:default:1:0 RECEIVER worker
                  BLOCK none POSTURE retain BARRIER none START testtraps_start)

kickos_add_driver(testfails SOURCES testfails.cc
                  THREADS service:0:default:1:0 RECEIVER service
                  BLOCK none POSTURE retain BARRIER none START testfails_start)

kickos_add_driver(testpl011 SOURCES testpl011.cc
                  THREADS service:0:default:1:0 RECEIVER service WINDOWS regs
                  BLOCK none POSTURE handover BARRIER none START testpl011_start CONSOLE)
