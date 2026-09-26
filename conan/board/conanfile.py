# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import os

from conan import ConanFile
from conan.tools.files import save

# The environment variable each multilib's toolchain file reads the libc location from.
LIBC_ENV = {
    "armv6m": "KICKOS_NEWLIB_ARMV6M",
    "armv7m": "KICKOS_NEWLIB_ARMV7M",
    "armv7em_fp_softfp": "KICKOS_NEWLIB_ARMV7EM_FP_SOFTFP",
    "armv7em_dp_softfp": "KICKOS_NEWLIB_ARMV7EM_DP_SOFTFP",
    "armv8m_fp_softfp": "KICKOS_NEWLIB_ARMV8M_FP_SOFTFP",
    "armv8m_fp_hard": "KICKOS_NEWLIB_ARMV8M_FP_HARD",
    "aarch64": "KICKOS_NEWLIB_AARCH64",
    "rv32imac_ilp32": "KICKOS_NEWLIB_RV32IMAC_ILP32",
    "rv64imac_lp64": "KICKOS_NEWLIB_RV64IMAC_LP64",
    "rxv3_dfpu": "KICKOS_NEWLIB_RXV3_DFPU",
    "xtensa_esp32": "KICKOS_NEWLIB_XTENSA_ESP32",
}


class KickOSBoard(ConanFile):
    """Cross-target dependencies for KickOS boards: pinned newlib per target multilib.

    The output is `kickos-newlib.sh`, which exports the package folder in the variable the
    toolchain file reads. No CMakeDeps and no CMakeToolchain: every preset pins its own
    toolchainFile, and the libc is a toolchain input rather than a package CMake finds.
    """

    options = {
        "multilib": list(LIBC_ENV),
    }
    default_options = {
        "multilib": "aarch64",
    }

    def requirements(self):
        self.requires("kickos-newlib/1.0",
                      options={"multilib": str(self.options.multilib)})

    def generate(self):
        pkg = self.dependencies["kickos-newlib"].package_folder
        var = LIBC_ENV[str(self.options.multilib)]
        save(self, os.path.join(self.generators_folder, f"kickos-newlib-{self.options.multilib}.sh"),
             f'export {var}="{pkg}"\n')
