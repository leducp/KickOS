# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import glob
import hashlib
import os
import shutil
import subprocess

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import get, save, unzip

# One entry per family (docs/design-m10-toolchain.md section 2). `gcc` and `newlib` are configure
# options beyond the ones every family takes; `nano` is the extra newlib profile a family ships,
# installed beside the full one as nano.specs expects: libc_nano.a, libg_nano.a, libstdc++_nano.a
# and libsupc++_nano.a in every multilib directory, and newlib.h under include/newlib-nano.
FAMILIES = {
    "arm-none-eabi": {
        "gcc": ["--with-multilib-list=rmprofile"],
        "cflags": "-g -O2 -ffunction-sections -fdata-sections",
        "newlib": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                   "--enable-newlib-register-fini", "--enable-newlib-mb"],
        # The multilibs the link check below links a full and, where there is one, a nano C++
        # program for: a missing archive fails the package, not a board's first link.
        "check": [["-mcpu=cortex-m0", "-mfloat-abi=soft", "-mthumb"],
                  ["-mcpu=cortex-m4", "-mfpu=fpv4-sp-d16", "-mfloat-abi=softfp", "-mthumb"]],
        "nano": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                 "--disable-newlib-fseek-optimization", "--disable-newlib-fvwrite-in-streamio",
                 "--enable-lite-exit", "--disable-newlib-mb", "--enable-newlib-nano-formatted-io",
                 "--enable-newlib-nano-malloc", "--disable-newlib-unbuf-stream-opt",
                 "--disable-newlib-io-c99-formats", "--disable-newlib-io-long-long",
                 "--enable-newlib-reent-small", "--disable-newlib-register-fini",
                 "--disable-newlib-wide-orient"],
    },
}

PREFIX = "kickos-toolchain-"


class KickOSToolchain(ConanFile):
    """One family of the KickOS toolchain: binutils, GCC, newlib and libstdc++ built together from
    the pinned sources in conandata.yml, for the host this package runs on.

    Exported once per family under the family's name, kickos-toolchain-<triple>, so a consumer
    can require several families at once: Conan refuses one package required twice with
    different options. Conan keys the package on the host's os and arch; the compiler that
    built it is no part of what it is.
    """

    version = "1.0"
    license = "GPL-3.0-or-later WITH GCC-exception-3.1 AND BSD-3-Clause (newlib COPYING.NEWLIB)"
    description = "The KickOS cross toolchain for one target family"
    package_type = "application"
    settings = "os", "arch"
    exports = "conandata.yml"

    def _target(self):
        if not self.name or not self.name.startswith(PREFIX) or \
                self.name[len(PREFIX):] not in FAMILIES:
            raise ConanInvalidConfiguration(
                f"export this recipe as {PREFIX}<family>, one of: {', '.join(FAMILIES)}")
        return self.name[len(PREFIX):]

    def validate(self):
        self._target()

    def layout(self):
        # Apart: every stage builds out of tree, and GCC and newlib refuse a source tree
        # that a build has configured.
        self.folders.source = "src"
        self.folders.build = "build"

    def _fetch(self, key, destination):
        source = self.conan_data["sources"][key]
        local = os.environ.get("KICKOS_TOOLCHAIN_SOURCES", "")
        archive = os.path.join(local, os.path.basename(source["url"])) if local else ""
        if archive and os.path.isfile(archive):
            with open(archive, "rb") as f:
                digest = hashlib.file_digest(f, "sha256").hexdigest()
            if digest != source["sha256"]:
                raise ConanInvalidConfiguration(
                    f"{archive} has sha256 {digest}, expected {source['sha256']}.")
            unzip(self, archive, destination=destination, strip_root=True)
        else:
            get(self, **source, destination=destination, strip_root=True)

    def source(self):
        for key in ("gcc", "binutils", "newlib"):
            self._fetch(key, os.path.join(self.source_folder, key))
        # GCC builds these in its own tree when they sit beside its sources.
        for key in ("gmp", "mpfr", "mpc", "isl"):
            self._fetch(key, os.path.join(self.source_folder, "gcc", key))

    def _run(self, argv, cwd, env=None):
        os.makedirs(cwd, exist_ok=True)
        with open(os.path.join(cwd, "kickos-build.log"), "a") as log:
            log.write("$ " + " ".join(argv) + "\n")
            log.flush()
            rc = subprocess.run(argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        if rc.returncode != 0:
            raise ConanInvalidConfiguration(
                f"{argv[0]} failed in {cwd}; its output is in {cwd}/kickos-build.log")

    def _make(self, cwd, *targets, env=None):
        jobs = str(os.cpu_count() or 1)
        self._run(["make", "-j", jobs, "MAKEINFO=true", *targets], cwd, env)

    def _newlib(self, where, prefix, options, env):
        target = self._target()
        src = self.source_folder
        self._run([os.path.join(src, "newlib", "configure"), f"--target={target}",
                   f"--prefix={prefix}", "--disable-nls", "--disable-newlib-supplied-syscalls",
                   *options], where, env)
        self._make(where, env=env)
        self._make(where, "install", env=env)

    def build(self):
        target = self._target()
        family = FAMILIES[target]
        src = self.source_folder
        stage = os.path.join(self.build_folder, "stage")
        env = dict(os.environ)
        env["PATH"] = os.path.join(stage, "bin") + os.pathsep + env["PATH"]
        env["CFLAGS_FOR_TARGET"] = family["cflags"]
        env["CXXFLAGS_FOR_TARGET"] = family["cflags"]

        binutils = os.path.join(self.build_folder, "binutils")
        self._run([os.path.join(src, "binutils", "configure"), f"--target={target}",
                   f"--prefix={stage}", "--disable-nls", "--disable-werror", "--disable-gdb",
                   "--disable-gdbserver", "--disable-sim", "--disable-gprofng"], binutils, env)
        self._make(binutils, env=env)
        self._make(binutils, "install-strip", env=env)

        gcc_common = self._gcc_options(stage, os.path.join(stage, target))
        gcc1 = os.path.join(self.build_folder, "gcc1")
        self._run([os.path.join(src, "gcc", "configure"), *gcc_common, "--enable-languages=c",
                   "--without-headers"], gcc1, env)
        self._make(gcc1, "all-gcc", "all-target-libgcc", env=env)
        self._make(gcc1, "install-strip-gcc", "install-target-libgcc", env=env)

        self._newlib(os.path.join(self.build_folder, "newlib"), stage, family["newlib"], env)
        if "nano" in family:
            nano = os.path.join(self.build_folder, "nano-stage")
            self._newlib(os.path.join(self.build_folder, "newlib-nano"), nano, family["nano"],
                         env)
            self._install_nano(nano, stage)

        gcc = os.path.join(self.build_folder, "gcc")
        self._run([os.path.join(src, "gcc", "configure"), *gcc_common,
                   "--enable-languages=c,c++"], gcc, env)
        self._make(gcc, env=env)
        self._make(gcc, "install-strip", env=env)

        if "nano" in family:
            self._install_nano_cxx(nano, stage, env)
        self._check_links(stage, "nano" in family)

    def _gcc_options(self, prefix, sysroot):
        # No --disable-tls: without it GCC falls back to emulated TLS, and KickOS seats every
        # thread's thread_local block itself (kernel/thread/tls.cc). The sysroot is the target
        # directory newlib installs into, inside the prefix so GCC relocates it with the
        # package: nano.specs names its header as =/include/newlib-nano.
        return [f"--target={self._target()}", f"--prefix={prefix}", "--with-newlib",
                f"--with-sysroot={sysroot}", "--with-native-system-header-dir=/include",
                "--disable-shared", "--disable-threads", "--disable-nls", "--disable-libssp",
                "--disable-libgomp", "--disable-libquadmath", "--disable-libstdcxx-pch",
                "--enable-checking=release", *FAMILIES[self._target()]["gcc"]]

    def _install_nano_cxx(self, nano, stage, env):
        # libstdc++ and libsupc++ again, against the nano newlib's headers: a second GCC whose
        # sysroot is the nano install builds its target libraries, and only they are kept.
        target = self._target()
        prefix = os.path.join(self.build_folder, "nano-cxx-stage")
        where = os.path.join(self.build_folder, "gcc-nano")
        self._run([os.path.join(self.source_folder, "gcc", "configure"),
                   *self._gcc_options(prefix, os.path.join(nano, target)),
                   "--enable-languages=c,c++"], where, env)
        self._make(where, "all-gcc", "all-target-libgcc", "all-target-libstdc++-v3", env=env)
        self._make(where, "install-target-libstdc++-v3", env=env)
        lib = os.path.join(prefix, target, "lib")
        for archive in glob.glob(os.path.join(lib, "**", "libstdc++.a"), recursive=True):
            here = os.path.dirname(archive)
            dest = os.path.join(stage, target, "lib", os.path.relpath(here, lib))
            shutil.copyfile(archive, os.path.join(dest, "libstdc++_nano.a"))
            shutil.copyfile(os.path.join(here, "libsupc++.a"),
                            os.path.join(dest, "libsupc++_nano.a"))

    def _check_links(self, stage, nano):
        target = self._target()
        where = os.path.join(self.build_folder, "check")
        os.makedirs(where, exist_ok=True)
        source = os.path.join(where, "check.cc")
        save(self, source, "#include <stdexcept>\n#include <string>\n"
             "int main() { try { throw std::runtime_error(std::string(\"k\")); }\n"
             "  catch (const std::exception& e) { return e.what()[0] == 'k' ? 0 : 1; } }\n")
        cxx = os.path.join(stage, "bin", f"{target}-g++")
        profiles = [[]] + ([["--specs=nano.specs"]] if nano else [])
        for flags in FAMILIES[target].get("check", [[]]):
            for profile in profiles:
                self._run([cxx, *flags, *profile, "--specs=nosys.specs", source,
                           "-o", os.path.join(where, "check.elf")], where)

    def _install_nano(self, nano, stage):
        target = self._target()
        nano_lib = os.path.join(nano, target, "lib")
        for libc in glob.glob(os.path.join(nano_lib, "**", "libc.a"), recursive=True):
            multi = os.path.relpath(os.path.dirname(libc), nano_lib)
            dest = os.path.join(stage, target, "lib", multi)
            os.makedirs(dest, exist_ok=True)
            shutil.copyfile(libc, os.path.join(dest, "libc_nano.a"))
            shutil.copyfile(os.path.join(os.path.dirname(libc), "libg.a"),
                            os.path.join(dest, "libg_nano.a"))
        header = os.path.join(stage, target, "include", "newlib-nano")
        os.makedirs(header, exist_ok=True)
        shutil.copyfile(os.path.join(nano, target, "include", "newlib.h"),
                        os.path.join(header, "newlib.h"))

    def package(self):
        shutil.copytree(os.path.join(self.build_folder, "stage"), self.package_folder,
                        dirs_exist_ok=True, symlinks=True)
        sources = self.conan_data["sources"]
        save(self, os.path.join(self.package_folder, "kickos-toolchain.txt"),
             "".join(f"{k}={os.path.basename(v['url'])}\n" for k, v in sources.items())
             + f"target={self._target()}\n")

    def package_info(self):
        self.cpp_info.bindirs = ["bin"]
