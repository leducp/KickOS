# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import os
import re
import subprocess

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get, replace_in_file, save

# One entry per multilib a preset links. `flags` select the multilib and must match the
# multilib's own spelling; `cflags` and `configure` reproduce the toolchain vendor's newlib
# build, which the post-build newlib.h comparison checks.
MULTILIBS = {
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
    },
    "rv64imac_lp64": {
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
    },
}

DYNAMIC_REENT = """
/* KickOS: every _REENT use calls __getreent(), which the runtime answers per thread. */
#ifndef __DYNAMIC_REENT__
#define __DYNAMIC_REENT__
#endif

#endif /* __SYS_CONFIG_H__ */"""


class KickOSNewlib(ConanFile):
    """newlib for the KickOS cross boards, built so libc reaches its reentrant state through
    __getreent() and defines no __getreent of its own.

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
    description = "newlib with dynamic reentrancy for KickOS"
    package_type = "static-library"
    exports = "conandata.yml"

    options = {
        "multilib": list(MULTILIBS),
        "toolchain": ["ANY"],
    }
    default_options = {
        "multilib": "aarch64",
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

    def package_id(self):
        gcc = self._gcc(self._spec(info=True))
        self.info.options.toolchain = " ".join(
            [self._run(gcc, "-dumpmachine"), self._run(gcc, "--version").splitlines()[0]])

    def build(self):
        spec = self._spec()
        gcc = self._gcc()
        src = os.path.join(self.build_folder, "src")
        get(self, **self.conan_data["sources"][self._release()], destination=src, strip_root=True)
        bin_dir = os.path.dirname(gcc)
        prefix = f"{spec['triple']}-"
        config_h = os.path.join(src, "newlib", "libc", "include", "sys", "config.h")
        replace_in_file(self, config_h, "\n#endif /* __SYS_CONFIG_H__ */", DYNAMIC_REENT)

        env = dict(os.environ)
        env["CC_FOR_TARGET"] = " ".join([gcc] + spec["flags"])
        for tool in ("AR", "AS", "LD", "NM", "RANLIB", "OBJDUMP", "READELF", "STRIP"):
            env[f"{tool}_FOR_TARGET"] = os.path.join(bin_dir, prefix + tool.lower())
        # GETREENT_PROVIDED: libc carries no fallback __getreent, so an image whose runtime
        # does not answer the hook fails its link.
        env["CFLAGS_FOR_TARGET"] = spec["cflags"] + " -DGETREENT_PROVIDED"

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
        stock = self._defines(os.path.join(self._toolchain_include(), "newlib.h"))
        if ours != stock:
            raise ConanInvalidConfiguration(
                "this build's newlib.h disagrees with the toolchain's, so struct layouts its "
                f"libstdc++ was built against may differ:\n  ours : {ours}\n  stock: {stock}")
        libc = os.path.join(root, "lib", "libc.a")
        nm = self._run(os.path.join(bin_dir, f"{spec['triple']}-nm"), "-A", libc)
        callers = [line for line in nm.splitlines() if line.endswith(" U __getreent")]
        defines = [line for line in nm.splitlines()
                   if re.search(r" [TtWw] __getreent$", line)]
        if not callers or defines:
            raise ConanInvalidConfiguration(
                f"{libc} is not dynamic-reent: {len(callers)} member(s) call __getreent and "
                f"{len(defines)} define it.")
        header = self._run(os.path.join(bin_dir, f"{spec['triple']}-readelf"), "-h",
                           os.path.join(root, "lib", "libm.a"))
        if spec["machine"] not in header:
            raise ConanInvalidConfiguration(f"libm.a is not {spec['machine']} code.")

    def package(self):
        spec = self._spec()
        root = os.path.join(self.build_folder, "stage", "usr", spec["triple"])
        copy(self, "*", os.path.join(root, "include"), os.path.join(self.package_folder, "include"))
        for lib in ("libc.a", "libg.a", "libm.a"):
            copy(self, lib, os.path.join(root, "lib"), os.path.join(self.package_folder, "lib"))
        copy(self, "COPYING.NEWLIB", os.path.join(self.build_folder, "src"),
             os.path.join(self.package_folder, "licenses"))
        save(self, os.path.join(self.package_folder, "kickos-newlib.txt"),
             "\n".join([f"multilib={self.options.multilib}",
                        f"triple={spec['triple']}",
                        "flags=" + " ".join(spec["flags"]),
                        f"newlib={self._release()}",
                        "reent=dynamic",
                        ""]))

    def package_info(self):
        self.cpp_info.libs = ["c", "m"]
