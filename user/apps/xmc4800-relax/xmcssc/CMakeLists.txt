# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800/USIC0-CH1 SSC consumer of the SPI class <kickos/driver/spi.h>, built BOTH WAYS from
# ONE source: the substitution property made concrete. The client body that opens a device and
# clocks transfers is identical over the proxy and over the local engine; only who hands the
# client which capability differs, and that difference is the point (an endpoint cap and a
# window grant are not the same thing). Which of the two answers the class here is the image
# posture's, so this file names the CLASS and not a backend; KICKOS_SPI_LOCAL_ENGINE in
# system/CMakeLists.txt is what selects, and KICKOS_SPI_LOCAL is what main.cc reads.
#
# No CTest gate: it answers a HARDWARE question (an unprivileged SSC engine over the
# PMSA-granted USIC window, exercising the FEM=1 hardware CS-hold framing) and is validated by
# the operator on silicon.
kickos_add_app_target(xmcssc main.cc CLASSES spi)
# The endpoint accessor is the DRIVER's, not the class backend's: this app names the service it
# takes a capability from, which no posture chooses for it.
target_link_libraries(xmcssc PRIVATE kickos_xmcssc kickos)
kickos_emit_image(xmcssc)
