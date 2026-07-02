# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A standalone downstream project: it does NOT live in the KickOS tree. It
# consumes the installed KickOS package and builds a flashable/runnable image
# in ~3 lines (the NuttX-export ergonomics bar). Swapping BOARD sim -> an MCU
# keeps main.cc unchanged.

cmake_minimum_required(VERSION 3.24)
project(kickos_oot_app LANGUAGES C CXX)

find_package(KickOS REQUIRED)
kickos_add_application(oot_app SOURCES main.cc BOARD sim)
