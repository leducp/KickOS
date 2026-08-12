# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

from conan import ConanFile


class KickOSDev(ConanFile):
    """Host-side development dependencies for KickOS.

    A BOARD BUILD NEEDS NOTHING FROM HERE. Every cross target compiles freestanding with
    no third-party code, so this file exists only for the host unit-test layer. It emits
    CMakeDeps and not CMakeToolchain: every KickOS preset already pins its own
    toolchainFile, and a second one would silently replace the compiler selection.
    """

    settings = "os", "compiler", "build_type", "arch"

    options = {
        "unit_tests": [True, False],
    }
    default_options = {
        "unit_tests": True,
    }

    generators = "CMakeDeps"

    def requirements(self):
        if self.options.unit_tests:
            self.requires("gtest/1.15.0")

    def configure(self):
        if self.options.unit_tests:
            # gmock is unused: every KickOS seam is an extern "C" free function, so the linker
            # redirects it. Left True to share ../KickCAT's cached gtest binary; flipping it
            # forces a separate package id and a rebuild.
            self.options["gtest"].build_gmock = True
