# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import hashlib
import os
import re
import shutil
import subprocess

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get, replace_in_file, save, unzip

# `flags` select the multilib; the other options reproduce the toolchain vendor's newlib
# configuration, which the post-build newlib.h comparison checks.
ARM_SPEC = {
    "triple": "arm-none-eabi", "bin_env": "KICKOS_ARM_TOOLCHAIN_BIN",
    "cflags": "-g -O2 -ffunction-sections -fdata-sections",
    "configure": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                  "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                  "--enable-newlib-register-fini", "--enable-newlib-mb"],
    "machine": "ARM", "reent": "static",
}

# Match the Arm toolchain's newlib-nano header and its paired C++ archives. The
# package still builds its own libc, so KickOS's reentrancy contract remains ours.
ARM_NANO_CONFIGURE = [
    "--disable-newlib-fseek-optimization", "--disable-newlib-fvwrite-in-streamio",
    "--enable-lite-exit", "--disable-newlib-mb", "--enable-newlib-nano-formatted-io",
    "--enable-newlib-nano-malloc", "--disable-newlib-unbuf-stream-opt",
    "--disable-newlib-io-c99-formats", "--disable-newlib-io-long-long",
    "--enable-newlib-reent-small", "--disable-newlib-register-fini",
    "--disable-newlib-wide-orient",
]


def arm_spec(*flags):
    return {**ARM_SPEC, "flags": list(flags) + ["-mthumb"]}


MULTILIBS = {
    "armv6m": arm_spec("-mcpu=cortex-m0", "-mfloat-abi=soft"),
    "armv7m": arm_spec("-mcpu=cortex-m3", "-mfloat-abi=soft"),
    "armv7em_fp_softfp": arm_spec("-mcpu=cortex-m4", "-mfpu=fpv4-sp-d16",
                                  "-mfloat-abi=softfp"),
    "armv7em_dp_softfp": arm_spec("-mcpu=cortex-m7", "-mfpu=fpv5-d16",
                                  "-mfloat-abi=softfp"),
    "armv8m_fp_softfp": arm_spec("-mcpu=cortex-m33", "-mfpu=fpv5-sp-d16",
                                  "-mfloat-abi=softfp"),
    "armv8m_fp_hard": arm_spec("-mcpu=cortex-m33", "-mfpu=fpv5-sp-d16",
                                "-mfloat-abi=hard"),
    "aarch64": {
        "triple": "aarch64-none-elf",
        "bin_env": "KICKOS_AARCH64_TOOLCHAIN_BIN",
        "flags": [],
        "cflags": "-g -O2 -ffunction-sections -fdata-sections",
        "configure": [
            "--enable-newlib-retargetable-locking",
            "--enable-newlib-reent-check-verify",
            "--enable-newlib-io-long-long",
            "--enable-newlib-io-c99-formats",
            "--enable-newlib-register-fini",
            "--enable-newlib-mb",
        ],
        "machine": "AArch64",
        "reent": "dynamic",
    },
    "rv32imac_ilp32": {
        "triple": "riscv32-none-elf", "bin_env": "KICKOS_RISCV_TOOLCHAIN_BIN",
        "flags": ["-march=rv32imac_zicsr", "-mabi=ilp32"],
        "cflags": "-g -Os -ftls-model=local-exec",
        "configure": ["--enable-newlib-reent-check-verify", "--enable-newlib-io-long-long",
                      "--enable-newlib-io-c99-formats", "--enable-newlib-atexit-dynamic-alloc"],
        "machine": "RISC-V", "reent": "static",
    },
    "rv64imac_lp64": {
        # The RISC-V toolchain's only prefix: it carries the rv64 multilib as well.
        "triple": "riscv32-none-elf",
        "bin_env": "KICKOS_RISCV_TOOLCHAIN_BIN",
        "flags": ["-march=rv64imac_zmmul_zaamo_zalrsc_zca", "-mabi=lp64"],
        "cflags": "-g -Os -ftls-model=local-exec",
        "configure": [
            "--enable-newlib-reent-check-verify",
            "--enable-newlib-io-long-long",
            "--enable-newlib-io-c99-formats",
            "--enable-newlib-atexit-dynamic-alloc",
        ],
        "machine": "RISC-V",
        "reent": "dynamic",
    },
    "rxv3_dfpu": {
        "triple": "rx-elf", "bin_env": "KICKOS_RX_TOOLCHAIN_BIN",
        "flags": ["-misa=v3", "-mdfpu"],
        "cflags": "-g -O2",
        "configure": [],
        "machine": "Renesas RX", "reent": "static",
    },
    "xtensa_esp32": {
        "triple": "xtensa-esp32-elf", "bin_env": "KICKOS_XTENSA_BIN",
        "flags": [],
        "cflags": "-g -O2",
        "configure": ["--enable-newlib-atexit-dynamic-alloc", "--enable-newlib-iconv",
                      "--enable-newlib-nano-malloc", "--enable-newlib-retargetable-locking",
                      "--enable-newlib-reent-check-verify", "--enable-newlib-reent-small",
                      "--enable-newlib-reent-binary-compat", "--enable-newlib-io-c99-formats",
                      "--enable-newlib-io-long-long", "--enable-newlib-io-pos-args",
                      "--disable-newlib-wide-orient"],
        "machine": "Tensilica Xtensa", "reent": "dynamic",
    },
}

DYNAMIC_REENT = """
/* KickOS: every _REENT use calls __getreent(), which the runtime answers per thread. */
#ifndef __DYNAMIC_REENT__
#define __DYNAMIC_REENT__
#endif

#endif /* __SYS_CONFIG_H__ */"""


class KickOSNewlib(ConanFile):
    """Pinned newlib for the KickOS cross boards, matching each target's reentrancy ABI.

    The newlib release is the one the cross toolchain bundles, because its libstdc++ was built
    against those headers; conandata.yml pins each release's tarball. The cross compiler comes
    from the toolchain variable the KickOS toolchain files read, and never from PATH. The
    package is keyed on the multilib option and on the compiler's identity, since Conan
    settings do not describe a bare-metal multilib.
    """

    name = "kickos-newlib"
    version = "1.0"
    license = "BSD-3-Clause AND others (newlib COPYING.NEWLIB)"
    url = "https://sourceware.org/newlib/"
    description = "Pinned newlib for KickOS cross targets"
    package_type = "static-library"
    exports = "conandata.yml"

    options = {
        "multilib": list(MULTILIBS),
        "flavor": ["full", "nano"],
        "toolchain": ["ANY"],
    }
    default_options = {
        "multilib": "aarch64",
        "flavor": "full",
        "toolchain": "resolved",
    }

    def _spec(self, info=False):
        if info:
            return MULTILIBS[str(self.info.options.multilib)]
        return MULTILIBS[str(self.options.multilib)]

    def _gcc(self, spec=None):
        if spec is None:
            spec = self._spec()
        bin_dir = os.environ.get(spec["bin_env"], "")
        if bin_dir == "":
            raise ConanInvalidConfiguration(
                f"{spec['bin_env']} is unset. It must name the bin/ directory of the "
                f"{spec['triple']} toolchain the KickOS presets use; this recipe never falls "
                "back to a compiler on PATH.")
        gcc = os.path.join(bin_dir, f"{spec['triple']}-gcc")
        if not os.access(gcc, os.X_OK):
            raise ConanInvalidConfiguration(
                f"{spec['bin_env']}={bin_dir} holds no executable {spec['triple']}-gcc.")
        return gcc

    def _run(self, *argv):
        return subprocess.run(list(argv), check=True, capture_output=True,
                              text=True).stdout.strip()

    def _toolchain_include(self):
        gcc = self._gcc()
        probe = self._run(gcc, *self._spec()["flags"], "-print-file-name=include")
        # -print-file-name=include answers gcc's own include; the libc headers sit in the
        # target directory beside lib/.
        libc = self._run(gcc, *self._spec()["flags"], "-print-file-name=libc.a")
        if not os.path.isabs(libc):
            raise ConanInvalidConfiguration(f"{gcc} resolves no libc.a for this multilib.")
        root = os.path.dirname(libc)
        while root != os.path.dirname(root):
            if os.path.isfile(os.path.join(root, "include", "newlib.h")):
                return os.path.join(root, "include")
            root = os.path.dirname(root)
        raise ConanInvalidConfiguration(f"no newlib.h beside {libc} (gcc include {probe}).")

    def _toolchain_newlib_version(self):
        with open(os.path.join(self._toolchain_include(), "_newlib_version.h")) as f:
            m = re.search(r'#define\s+_NEWLIB_VERSION\s+"([^"]+)"', f.read())
        if m is None:
            raise ConanInvalidConfiguration("the toolchain's newlib states no _NEWLIB_VERSION.")
        return m.group(1)

    def _release(self):
        have = self._toolchain_newlib_version()
        for release in self.conan_data["sources"]:
            if release.split(".")[:3] == have.split("."):
                return release
        raise ConanInvalidConfiguration(
            f"the {self._spec()['triple']} toolchain bundles newlib {have}, and conandata.yml "
            "pins no release of it. Add that release's sourceware tarball and sha256.")

    def validate(self):
        self._release()
        if str(self.options.flavor) == "nano" and self._spec()["triple"] != "arm-none-eabi":
            raise ConanInvalidConfiguration(
                "the nano profile requires the Arm toolchain's matching newlib-nano "
                "headers and C++ archives; this multilib has no validated pair.")
        if str(self.options.flavor) == "nano":
            header = os.path.join(self._toolchain_include(), "newlib-nano", "newlib.h")
            if not os.path.isfile(header):
                raise ConanInvalidConfiguration(f"Arm toolchain has no {header}.")
            gcc = self._gcc()
            for name in ("nano.specs", "libstdc++_nano.a", "libsupc++_nano.a"):
                path = self._run(gcc, *self._spec()["flags"], f"-print-file-name={name}")
                if not os.path.isfile(path):
                    raise ConanInvalidConfiguration(
                        f"Arm multilib {self.options.multilib} has no {name} ({path}).")

    def package_id(self):
        gcc = self._gcc(self._spec(info=True))
        self.info.options.toolchain = " ".join(
            [self._run(gcc, "-dumpmachine"), self._run(gcc, "--version").splitlines()[0]])

    def build(self):
        spec = self._spec()
        gcc = self._gcc()
        src = os.path.join(self.build_folder, "src")
        source = self.conan_data["sources"][self._release()]
        archive = os.environ.get("KICKOS_NEWLIB_SOURCE_ARCHIVE", "")
        if archive:
            with open(archive, "rb") as f:
                digest = hashlib.file_digest(f, "sha256").hexdigest()
            if digest != source["sha256"]:
                raise ConanInvalidConfiguration(
                    f"{archive} has sha256 {digest}, expected {source['sha256']}.")
            unzip(self, archive, destination=src, strip_root=True)
        else:
            get(self, **source, destination=src, strip_root=True)
        bin_dir = os.path.dirname(gcc)
        prefix = f"{spec['triple']}-"
        config_h = os.path.join(src, "newlib", "libc", "include", "sys", "config.h")
        if spec["reent"] == "dynamic":
            replace_in_file(self, config_h, "\n#endif /* __SYS_CONFIG_H__ */", DYNAMIC_REENT)
        features = os.path.join(src, "newlib", "libc", "include", "sys", "features.h")
        if self._release().startswith("4.6") and spec["triple"] in (
                "arm-none-eabi", "aarch64-none-elf", "xtensa-esp32-elf"):
            # Arm 15.3 and Espressif 16.1 ship the C23 visibility correction.
            replace_in_file(self, features, "__ISO_C_VISIBLE >= 2020",
                            "__ISO_C_VISIBLE >= 2023")
            replace_in_file(self, features, "#define __ISO_C_VISIBLE\t\t2020",
                            "#define __ISO_C_VISIBLE\t\t2023")
        if str(self.options.multilib) == "xtensa_esp32":
            # Espressif built libstdc++ against these POSIX feature declarations. Its
            # gthr-default.h uses pthread prototypes from the newlib headers at compile time.
            replace_in_file(self, config_h, "/* This block should be kept in sync",
                            "#define __BUFSIZ__ 128\n#define _REENT_SMALL\n\n"
                            "/* This block should be kept in sync")
            replace_in_file(self, features, "\n#endif /* __CYGWIN__ */\n\n#ifdef __cplusplus", """
#endif /* __CYGWIN__ */

/* Espressif toolchain C++ headers require these feature declarations. */
#define _POSIX_THREADS                          1
#define _POSIX_TIMEOUTS                         1
#define _POSIX_TIMERS                           1
#define _POSIX_MONOTONIC_CLOCK                  200112L
#define _POSIX_CLOCK_SELECTION                  200112L
#define _UNIX98_THREAD_MUTEX_ATTRIBUTES         1
#define _POSIX_READER_WRITER_LOCKS              200112L

#ifdef __cplusplus""")

        env = dict(os.environ)
        env["CC_FOR_TARGET"] = " ".join([gcc] + spec["flags"])
        for tool in ("AR", "AS", "LD", "NM", "RANLIB", "OBJDUMP", "READELF", "STRIP"):
            env[f"{tool}_FOR_TARGET"] = os.path.join(bin_dir, prefix + tool.lower())
        # GETREENT_PROVIDED: libc carries no fallback __getreent, so an image whose runtime
        # does not answer the hook fails its link.
        env["CFLAGS_FOR_TARGET"] = spec["cflags"]
        if spec["reent"] == "dynamic":
            env["CFLAGS_FOR_TARGET"] += " -DGETREENT_PROVIDED"

        build_dir = os.path.join(self.build_folder, "obj")
        os.makedirs(build_dir, exist_ok=True)
        configure = [
            os.path.join(src, "configure"),
            f"--target={spec['triple']}",
            "--prefix=/usr",
            "--disable-multilib",
            "--disable-nls",
            "--disable-newlib-supplied-syscalls",
        ] + spec["configure"]
        if str(self.options.flavor) == "nano":
            configure += ARM_NANO_CONFIGURE
        subprocess.run(configure, cwd=build_dir, env=env, check=True)
        jobs = str(os.cpu_count() or 1)
        subprocess.run(["make", "-j", jobs, "MAKEINFO=true", "all-target-newlib"],
                       cwd=build_dir, env=env, check=True)
        stage = os.path.join(self.build_folder, "stage")
        subprocess.run(["make", "MAKEINFO=true", f"DESTDIR={stage}", "install-target-newlib"],
                       cwd=build_dir, env=env, check=True)
        self._verify(os.path.join(stage, "usr", spec["triple"]))

    def _defines(self, header):
        with open(header) as f:
            return sorted(line.strip() for line in f if line.startswith("#define"))

    def _verify(self, root):
        spec = self._spec()
        bin_dir = os.path.dirname(self._gcc())
        ours = self._defines(os.path.join(root, "include", "newlib.h"))
        stock_header = os.path.join(self._toolchain_include(), "newlib.h")
        if str(self.options.flavor) == "nano":
            stock_header = os.path.join(self._toolchain_include(), "newlib-nano", "newlib.h")
        stock = self._defines(stock_header)
        if ours != stock:
            raise ConanInvalidConfiguration(
                "this build's newlib.h disagrees with the toolchain's, so struct layouts its "
                f"libstdc++ was built against may differ:\n  ours : {ours}\n  stock: {stock}")
        if str(self.options.multilib) == "xtensa_esp32":
            header = os.path.join("include", "sys", "config.h")
            ours_config = self._defines(os.path.join(root, header))
            stock_config = self._defines(os.path.join(self._toolchain_include(), "sys", "config.h"))
            if ours_config != stock_config:
                raise ConanInvalidConfiguration(
                    f"Xtensa sys/config.h differs from Espressif's:\n  ours : {ours_config}"
                    f"\n  stock: {stock_config}")
        libc = os.path.join(root, "lib", "libc.a")
        nm = self._run(os.path.join(bin_dir, f"{spec['triple']}-nm"), "-A", libc)
        callers = [line for line in nm.splitlines() if line.endswith(" U __getreent")]
        defines = [line for line in nm.splitlines()
                   if re.search(r" [TtWw] __getreent$", line)]
        if spec["reent"] == "dynamic" and (not callers or defines):
            raise ConanInvalidConfiguration(
                f"{libc} is not dynamic-reent: {len(callers)} member(s) call __getreent and "
                f"{len(defines)} define it.")
        if spec["reent"] == "static" and callers:
            raise ConanInvalidConfiguration(
                f"{libc} unexpectedly calls __getreent in {len(callers)} member(s).")
        header = self._run(os.path.join(bin_dir, f"{spec['triple']}-readelf"), "-h",
                           os.path.join(root, "lib", "libm.a"))
        if spec["machine"] not in header:
            raise ConanInvalidConfiguration(f"libm.a is not {spec['machine']} code.")

    def package(self):
        spec = self._spec()
        root = os.path.join(self.build_folder, "stage", "usr", spec["triple"])
        copy(self, "*", os.path.join(root, "include"), os.path.join(self.package_folder, "include"))
        copy(self, "libm.a", os.path.join(root, "lib"),
             os.path.join(self.package_folder, "lib"))
        if str(self.options.flavor) == "nano":
            for name in ("libc", "libg"):
                shutil.copyfile(os.path.join(root, "lib", f"{name}.a"),
                                os.path.join(self.package_folder, "lib", f"{name}_nano.a"))
        else:
            for lib in ("libc.a", "libg.a"):
                copy(self, lib, os.path.join(root, "lib"),
                     os.path.join(self.package_folder, "lib"))
        copy(self, "COPYING.NEWLIB", os.path.join(self.build_folder, "src"),
             os.path.join(self.package_folder, "licenses"))
        save(self, os.path.join(self.package_folder, "kickos-newlib.txt"),
             "\n".join([f"multilib={self.options.multilib}",
                        f"triple={spec['triple']}",
                        "flags=" + " ".join(spec["flags"]),
                        f"newlib={self._release()}",
                        f"reent={spec['reent']}",
                        f"flavor={self.options.flavor}",
                        ""]))

    def package_info(self):
        self.cpp_info.libs = ["c_nano" if str(self.options.flavor) == "nano" else "c", "m"]
