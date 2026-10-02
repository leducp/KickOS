# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import save

# The families conan/toolchain builds, as it names them.
FAMILIES = ("arm-none-eabi", "aarch64-none-elf", "riscv64-none-elf", "x86_64-elf", "rx-elf",
            "xtensa-esp32-elf")
PREFIX = "kickos-toolchain-"


class KickOSToolchains(ConanFile):
    """The KickOS toolchain a tree uses: every family it builds for, from conan/toolchain.

    The output folder holds kickos-toolchain.cmake, one bin directory per family, which the
    toolchain files read through KICKOS_TOOLCHAIN, and kickos-toolchain.sh, which sets that
    variable to this folder.
    """

    settings = "os", "arch"
    options = {"families": ["ANY"]}
    default_options = {"families": "all"}

    def _families(self):
        wanted = str(self.options.families)
        if wanted == "all":
            return FAMILIES
        chosen = tuple(f for f in wanted.split(",") if f)
        unknown = [f for f in chosen if f not in FAMILIES]
        if unknown:
            raise ConanInvalidConfiguration(
                f"unknown families {', '.join(unknown)}; conan/toolchain builds "
                f"{', '.join(FAMILIES)}")
        return chosen

    def requirements(self):
        for family in self._families():
            self.requires(f"{PREFIX}{family}/1.0")

    def generate(self):
        lines = ["# Written by conan/toolchains: one bin directory per KickOS toolchain family.\n"]
        for dep in self.dependencies.values():
            family = dep.ref.name[len(PREFIX):]
            bindir = os.path.join(dep.package_folder, "bin").replace("\\", "/")
            lines.append(f'set(KICKOS_TOOLCHAIN_BIN_{family} "{bindir}")\n')
        folder = self.generators_folder.replace("\\", "/")
        save(self, os.path.join(self.generators_folder, "kickos-toolchain.cmake"), "".join(lines))
        save(self, os.path.join(self.generators_folder, "kickos-toolchain.sh"),
             f'export KICKOS_TOOLCHAIN="{folder}"\n')
