# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800/USIC0-CH1 SSC consumer of the SPI class, built BOTH WAYS from one source in every
# configure: `xmcssc` over the packaged service's endpoint, whose composition links the proxy as
# that driver's client, and `xmcssc_local` on the local engine, owning the channel window and its
# line. XMCSSC_LOCAL is what the source reads. Each is judged from a silicon capture.
add_executable(xmcssc main.cc)
target_compile_definitions(xmcssc PRIVATE XMCSSC_LOCAL=0)
kickos_app_system(xmcssc COMPOSITION systems/proxy.yaml)
kickos_emit_image(xmcssc)
kickos_app_judge(xmcssc tests/integration/check_xmcssc.sh)

add_executable(xmcssc_local main.cc)
target_compile_definitions(xmcssc_local PRIVATE XMCSSC_LOCAL=1)
target_link_libraries(xmcssc_local PRIVATE KickOS::kickos_spi_xmcssc)
kickos_app_system(xmcssc_local COMPOSITION systems/local.yaml)
kickos_emit_image(xmcssc_local)
kickos_app_judge(xmcssc_local tests/integration/check_xmcssc.sh)
