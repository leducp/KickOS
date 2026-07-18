# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Out-of-tree KickOS consumer: the KickCAT LAN9252 EtherCAT slave on the
# Freedom-K64F over DSPI0. Built entirely against the installed KickOS package
# (find_package(KickOS)); KickCAT is brought in from a sibling checkout with the
# KickOS OS backend selected (KICKOS=ON) and linked as the plain `kickcat` target.

cmake_minimum_required(VERSION 3.24) # export needs >=3.24 for LINK_GROUP
project(freedom_slave LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

set(KICKCAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../KickCAT"
    CACHE PATH "Path to the sibling KickCAT checkout")

if(NOT EXISTS "${KICKCAT_DIR}/lib/slave/include/kickcat/ESC/Lan9252.h")
  message(FATAL_ERROR "freedom-slave: KickCAT not found at ${KICKCAT_DIR}")
endif()

# Select the KickOS OS backend inside KickCAT before descending, so KickCAT builds
# against KickOS (time only; no socket/pthreads).
set(KICKOS ON CACHE BOOL "" FORCE)
add_subdirectory("${KICKCAT_DIR}" "${CMAKE_BINARY_DIR}/kickcat" EXCLUDE_FROM_ALL)

kickos_add_application(freedom_slave
  SOURCES main.cc od_populator.cc spi_transport.cc
  BOARD   frdmk64f
  FULL_CXX)

# kickcat exposes its slave/kickos includes as INTERFACE/PUBLIC, so linking it lets
# main.cc see the kickcat/ headers (SPI, Lan9252, Slave, PDO, Mailbox, CoE/OD).
target_link_libraries(freedom_slave PRIVATE kickcat)

# Opt-in Stage-D CoE bring-up tracing: KickCAT's own slave/CoE debug macros (SM config,
# mailbox/SDO flow) route through fprintf -> newlib _write -> console. OFF by default.
option(FREEDOM_SLAVE_COE_TRACE "Enable KickCAT mailbox/CoE debug tracing" OFF)
if(FREEDOM_SLAVE_COE_TRACE)
  target_compile_definitions(kickcat PRIVATE
    DEBUG_SLAVE_INFO DEBUG_SLAVE_WARNING DEBUG_SLAVE_ERROR
    DEBUG_COE_INFO DEBUG_COE_WARNING DEBUG_COE_ERROR)
  target_compile_definitions(freedom_slave PRIVATE FREEDOM_SLAVE_COE_TRACE)
endif()
