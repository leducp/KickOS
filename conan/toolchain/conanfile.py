# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import collections
import glob
import hashlib
import lzma
import os
import shutil
import subprocess
import tarfile

from conan import ConanFile
from conan.errors import ConanException, ConanInvalidConfiguration
from conan.tools.files import copy, download, replace_in_file, save, unzip

# One entry per family (docs/design-m10-toolchain.md section 2). `gcc` and `newlib` are configure
# options beyond the ones every family takes; `cxxflags` are added to `cflags` for the C++
# libraries alone; `patches` name the conandata keys of files in patches/, which the recipe
# exports, applied to a component's sources after unpacking, in order; `drop` names libc
# members deleted from libc.a and libg.a once newlib installs, and the package fails where a
# multilib directory lacks either archive or either still holds one; `guards` names scripts in
# tools/, which the recipe also exports, each run with the package's own readelf over every
# archive the package installs, and the package fails where one refuses a member; `nano` is
# the extra newlib profile a family ships, installed
# beside the full one as nano.specs expects: libc_nano.a, libg_nano.a, libstdc++_nano.a and
# libsupc++_nano.a in every multilib directory, and newlib.h under include/newlib-nano.
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
    "aarch64-none-elf": {
        "gcc": [],
        "cflags": "-g -O2 -ffunction-sections -fdata-sections",
        "newlib": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                   "--enable-newlib-register-fini", "--enable-newlib-mb"],
        "dynamic_reent": "1",
        "check": [["-mcpu=cortex-a53"]],
    },
    # One compiler for both widths: rv64 is the default multilib, rv32 the other, every library
    # medany, which rv64's image window needs and rv32's does not mind.
    "riscv64-none-elf": {
        "gcc": ["--with-arch=rv64imac_zicsr_zmmul_zaamo_zalrsc_zca", "--with-abi=lp64",
                "--with-cmodel=medany",
                "--with-multilib-generator=rv64imac_zicsr_zmmul_zaamo_zalrsc_zca-lp64--;"
                "rv32imac_zicsr-ilp32--"],
        "cflags": "-g -Os -ftls-model=local-exec -ffunction-sections -fdata-sections",
        "newlib": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                   "--enable-newlib-register-fini", "--enable-newlib-mb",
                   "--enable-newlib-atexit-dynamic-alloc"],
        "dynamic_reent": "defined(__riscv) && __riscv_xlen == 64",
        "check": [["-march=rv32imac_zicsr", "-mabi=ilp32"],
                  ["-march=rv64imac_zicsr_zmmul_zaamo_zalrsc_zca", "-mabi=lp64",
                   "-mcmodel=medany"]],
    },
    # x86_64's binutils also writes PE32+, the UEFI image q35 boots. The libraries are
    # position-independent, as every x86_64 image is relocated, and keep no red zone, as a
    # privileged thread takes interrupts on its own stack. x86_64-elf ships no crt0, and the
    # check supplies its own _start. A bare x86_64-elf GCC would put constructors in .ctors,
    # and the root task runs the app's .init_array (user/src/root_entry.cc).
    #
    # A PE32+ image holds no global offset table and no undefined symbol (design section 5.5).
    # The GCC patch binds every symbol but a weak one locally under -fpie, and the libraries
    # carry no weak undefined reference: no register_fini, no transactional clones, no
    # time-zone override, and neither newlib's init.o, whose __libc_init_array would walk the
    # kernel's constructors on q35, nor fini.o. No libgcov.a either, which libgcc compiles
    # -fpic. The binutils patch gives a weak definition's absolute words their base
    # relocation, which C++'s typeinfo and vtables need to move with the image.
    "x86_64-elf": {
        "binutils": ["--enable-targets=x86_64-pep"],
        "patches": {"gcc": ["kickos-x86_64-binds-local"],
                    "binutils": ["kickos-x86_64-pe-defweak-reloc"]},
        "gcc": ["--disable-multilib", "--enable-initfini-array", "--with-libstdcxx-zoneinfo=no",
                "--disable-gcov"],
        "cflags": "-g -O2 -fpie -mno-red-zone -ffunction-sections -fdata-sections",
        "cxxflags": "-D_GLIBCXX_USE_WEAK_REF=0",
        "newlib": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                   "--disable-newlib-register-fini", "--enable-newlib-mb"],
        "drop": ["libc_a-init.o", "libc_a-fini.o"],
        "guards": ["check-x86_64-no-got.sh", "check-x86_64-weak-undef.sh"],
        "dynamic_reent": "1",
        "check": [["-march=x86-64-v3"]],
        "link": ["-nostartfiles"],
    },
    # Espressif's GCC and binutils with the ESP32 core overlaid, so the compiler knows that one
    # core and loads no plugin. The overlay's newlib core header goes where upstream newlib
    # reads it, not where Espressif's fork does. newlib 4.5.0's generated Makefile.in copies
    # that header's directory one level deep only, short of xtensa/config, so its libm is
    # given the directory itself. The libraries take the boards' -mlongcalls, as an image's
    # IRAM and flash text lie further apart than a call reaches. __BUFSIZ__ keeps each stdio
    # buffer at the 128 bytes Espressif's newlib gives it rather than newlib's 1024.
    "xtensa-esp32-elf": {
        "sources": {"gcc": "gcc-esp", "binutils": "binutils-esp"},
        "overlay": {"xtensa_esp32/binutils": "binutils", "xtensa_esp32/gcc": "gcc",
                    "xtensa_esp32/newlib/newlib/libc/sys/xtensa/include":
                        "newlib/newlib/libc/machine/xtensa/include"},
        "newlib_include": "newlib/newlib/libc/machine/xtensa/include",
        "gcc": [],
        "cflags": "-g -O2 -mlongcalls -ffunction-sections -fdata-sections",
        "newlib": ["--enable-newlib-atexit-dynamic-alloc", "--enable-newlib-iconv",
                   "--enable-newlib-nano-malloc", "--enable-newlib-retargetable-locking",
                   "--enable-newlib-reent-check-verify", "--enable-newlib-reent-small",
                   "--enable-newlib-reent-binary-compat", "--enable-newlib-io-c99-formats",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-pos-args",
                   "--disable-newlib-wide-orient"],
        "config_h": "#define __BUFSIZ__ 128",
        "dynamic_reent": "1",
        "check": [["-mlongcalls"]],
        "link": ["-nostartfiles"],
    },
    # The common sources with Renesas's changes ported onto them, as upstream's GCC and binutils
    # know no RXv3 or double-precision FPU. The multilib patch keeps four of GNURX's 104
    # multilibs; -misa=v3 -mdfpu selects 64-bit-double/dfpu/rxv3, as -mdfpu makes doubles
    # 64-bit. KickOS's compare patch, last, corrects GNURX's unordered double-precision
    # branches. Each touched file is a generated one the patches carry, made newer than its
    # inputs so that no maintainer tool runs. newlib's register_fini stays off: it needs `_fini`,
    # which RX's crt0 lacks.
    "rx-elf": {
        "patches": {"gcc": ["gnurx-gcc", "rx-multilib", "kickos-rx-dfpu-compare"],
                    "binutils": ["gnurx-binutils"],
                    "newlib": ["gnurx-newlib"]},
        "touch": {"gcc": ["configure", "gcc/configure"],
                  "binutils": ["gas/config/rx-parse.c", "gas/config/rx-parse.h",
                               "opcodes/rx-decode.c", "bfd/bfd-in2.h"],
                  "newlib": ["newlib/Makefile.in", "libgloss/Makefile.in"]},
        "gcc": [],
        "cflags": "-g -O2 -ffunction-sections -fdata-sections",
        "newlib": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                   "--disable-newlib-register-fini", "--enable-newlib-mb"],
        "check": [["-misa=v3", "-mdfpu"]],
    },
}

# Where `dynamic_reent` holds, every _REENT use calls __getreent(), which KickOS answers per
# thread. A condition rather than a define: newlib's multilib build compiles every multilib
# against this one header, so a family whose multilibs differ in reentrancy decides it per
# multilib here.
DYNAMIC_REENT = """
/* KickOS: dynamic reentrancy where the multilib compiled is one the toolchain marks so. */
#if {condition}
#ifndef __DYNAMIC_REENT__
#define __DYNAMIC_REENT__
#endif
#endif
"""

# Every component a family builds, and the source key each takes unless the family names another.
COMPONENTS = ("gcc", "binutils", "newlib")
PREREQUISITES = ("gmp", "mpfr", "mpc", "isl")

# Every guard a family names, exported for every family, as the recipe revision is one for all.
GUARDS = sorted({g for family in FAMILIES.values() for g in family.get("guards", [])})

PREFIX = "kickos-toolchain-"

# The GitHub release's one archive of every pinned source and patch (design section 1). A source
# conandata.yml gives a `tar_sha256` is stored as .tar.xz in place of its .tar.gz, and checked by
# the hash of its uncompressed tar.
RELEASE = "https://github.com/leducp/KickOS/releases/download/toolchain-{version}/"
SOURCES = "kickos-toolchain-sources-{version}.tar.xz"


class KickOSToolchain(ConanFile):
    """One family of the KickOS toolchain: binutils, GCC, newlib and libstdc++ built together from
    the pinned sources in conandata.yml, for the host this package runs on.

    Exported once per family under the family's name, kickos-toolchain-<triple>, so a consumer
    can require several families at once: Conan refuses one package required twice with
    different options. Conan keys the package on the host's os and arch; the compiler that
    built it is no part of what it is.
    """

    version = "1.1"
    license = "GPL-3.0-or-later WITH GCC-exception-3.1 AND BSD-3-Clause (newlib COPYING.NEWLIB)"
    description = "The KickOS cross toolchain for one target family"
    package_type = "application"
    settings = "os", "arch"
    # `exports`, not `exports_sources`: `conan cache save --no-source` keeps the export folder.
    exports = "conandata.yml", "patches/*"

    def export(self):
        # The guards the tree's images run, copied rather than restated so there is one of each.
        # In the export folder they are part of the recipe revision: editing one rebuilds every
        # family, and a saved package's recipe carries the guards it passed.
        tools = os.path.join(self.recipe_folder, "..", "..", "tools")
        for guard in GUARDS:
            if not copy(self, guard, tools, os.path.join(self.export_folder, "tools")):
                raise ConanInvalidConfiguration(f"no {guard} in {os.path.normpath(tools)}")

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

    def _key(self, component):
        return FAMILIES[self._target()].get("sources", {}).get(component, component)

    def _sources(self):
        family = FAMILIES[self._target()]
        overlay = ["xtensa-overlays"] if "overlay" in family else []
        patches = [k for c in COMPONENTS for k in family.get("patches", {}).get(c, [])]
        return [self._key(c) for c in COMPONENTS] + list(PREREQUISITES) + overlay + patches

    def _archive(self, key):
        source = self.conan_data["sources"][key]
        return source["filename"] if "filename" in source else os.path.basename(source["url"])

    def _stored(self, key):
        # The name the sources archive holds the source under: conandata.yml's `stored`, which
        # tools/kickos-toolchain-release.sh reads too, else the archive's own.
        return self.conan_data["sources"][key].get("stored", self._archive(key))

    def _local(self, key, destination):
        # The copy KICKOS_TOOLCHAIN_SOURCES holds, a pinned archive or the release's sources
        # archive, checked as a download is, or "".
        local = os.environ.get("KICKOS_TOOLCHAIN_SOURCES", "")
        if not local:
            return ""
        path = os.path.join(local, self._archive(key))
        if os.path.isfile(path):
            return self._checked(key, path)
        archive = os.path.join(local, SOURCES.format(version=self.version))
        if os.path.isfile(archive):
            return self._extract(key, archive, destination)
        return ""

    def _checked(self, key, path):
        with open(path, "rb") as f:
            digest = hashlib.file_digest(f, "sha256").hexdigest()
        expected = self.conan_data["sources"][key]["sha256"]
        if digest != expected:
            raise ConanInvalidConfiguration(f"{path} has sha256 {digest}, expected {expected}.")
        return path

    def _extract(self, key, archive, destination):
        # The member of the sources archive, checked against what conandata.yml pins: the
        # archive's own word for it is no check.
        source = self.conan_data["sources"][key]
        name = self._stored(key)
        os.makedirs(destination, exist_ok=True)
        path = os.path.join(destination, name)
        with tarfile.open(archive, "r:xz") as tar:
            try:
                member = tar.getmember(f"kickos-toolchain-sources-{self.version}/{name}")
            except KeyError:
                member = None
            if member is None or not member.isfile():
                raise ConanInvalidConfiguration(f"{name} is not in the sources archive")
            with tar.extractfile(member) as src, open(path, "wb") as dst:
                shutil.copyfileobj(src, dst)
        if "tar_sha256" not in source:
            return self._checked(key, path)
        digest = hashlib.sha256()
        with lzma.open(path) as tar:
            for block in iter(lambda: tar.read(1 << 20), b""):
                digest.update(block)
        if digest.hexdigest() != source["tar_sha256"]:
            raise ConanInvalidConfiguration(
                f"{name} holds a tar of sha256 {digest.hexdigest()}, expected "
                f"{source['tar_sha256']}.")
        return path

    def _release_sources(self):
        # Downloaded once per package, and only where an upstream failed.
        archive = os.path.join(os.path.dirname(self.source_folder),
                               SOURCES.format(version=self.version))
        if not os.path.isfile(archive):
            download(self, RELEASE.format(version=self.version) + os.path.basename(archive),
                     archive)
        return archive

    def _fetch(self, key, destination):
        source = self.conan_data["sources"][key]
        name = self._archive(key)
        scratch = os.path.join(os.path.dirname(self.source_folder), "fetch")
        try:
            archive = self._local(key, scratch)
            if not archive and "url" in source:
                # Only a failed download falls to the release: a mismatch is no reason to.
                try:
                    download(self, source["url"], os.path.join(scratch, name))
                    archive = os.path.join(scratch, name)
                except ConanException:
                    archive = ""
                if archive:
                    self._checked(key, archive)
            if not archive:
                archive = self._extract(key, self._release_sources(), scratch)
            unzip(self, archive, destination=destination, strip_root=True)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)

    def source(self):
        family = FAMILIES[self._target()]
        try:
            for component in COMPONENTS:
                self._fetch(self._key(component), os.path.join(self.source_folder, component))
            # GCC builds these in its own tree when they sit beside its sources.
            for key in PREREQUISITES:
                self._fetch(key, os.path.join(self.source_folder, "gcc", key))
            if "overlay" in family:
                # Each overlay tree lies over the sources it names, replacing the core
                # description they carry (xtensa-config.h, xtensa-modules.c, core-isa.h).
                overlays = os.path.join(self.source_folder, "overlays")
                self._fetch("xtensa-overlays", overlays)
                for tree, where in family["overlay"].items():
                    shutil.copytree(os.path.join(overlays, tree),
                                    os.path.join(self.source_folder, where), dirs_exist_ok=True)
            for component, keys in family.get("patches", {}).items():
                tree = os.path.join(self.source_folder, component)
                for key in keys:
                    self._patch(key, tree)
                for generated in family.get("touch", {}).get(component, []):
                    if os.path.isfile(os.path.join(tree, generated)):
                        os.utime(os.path.join(tree, generated))
        finally:
            sources = os.path.join(os.path.dirname(self.source_folder),
                                   SOURCES.format(version=self.version))
            if os.path.isfile(sources):
                os.remove(sources)

    def _patch(self, key, tree):
        # A stamp, not a reverse dry run: a later patch over the same lines hides the earlier.
        stamp = os.path.join(tree, f".kickos-patched-{key}")
        if os.path.isfile(stamp):
            return
        # The exported copy alone, never the sources archive's.
        path = self._checked(key, os.path.join(self.recipe_folder, "patches", self._archive(key)))
        self._run(["patch", "-p1", "--no-backup-if-mismatch", "-i", path], tree)
        save(self, stamp, "")

    def _run(self, argv, cwd, env=None):
        os.makedirs(cwd, exist_ok=True)
        path = os.path.join(cwd, "kickos-build.log")
        with open(path, "a") as log:
            log.write("$ " + " ".join(argv) + "\n")
            log.flush()
            rc = subprocess.run(argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        if rc.returncode != 0:
            with open(path, errors="replace") as log:
                tail = collections.deque(log, maxlen=80)
            print(f"--- the last {len(tail)} lines of {path} ---\n" + "".join(tail), flush=True)
            raise ConanInvalidConfiguration(f"{argv[0]} failed in {cwd}; its output is in {path}")

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
        env["CXXFLAGS_FOR_TARGET"] = " ".join(f for f in (family["cflags"],
                                                          family.get("cxxflags")) if f)

        binutils = os.path.join(self.build_folder, "binutils")
        self._run([os.path.join(src, "binutils", "configure"), f"--target={target}",
                   f"--prefix={stage}", "--disable-nls", "--disable-werror", "--disable-gdb",
                   "--disable-gdbserver", "--disable-sim", "--disable-gprofng",
                   *family.get("binutils", [])], binutils, env)
        self._make(binutils, env=env)
        self._make(binutils, "install-strip", env=env)

        gcc_common = self._gcc_options(stage, os.path.join(stage, target))
        gcc1 = os.path.join(self.build_folder, "gcc1")
        self._run([os.path.join(src, "gcc", "configure"), *gcc_common, "--enable-languages=c",
                   "--without-headers"], gcc1, env)
        self._make(gcc1, "all-gcc", "all-target-libgcc", env=env)
        self._make(gcc1, "install-strip-gcc", "install-target-libgcc", env=env)

        addition = ""
        if "dynamic_reent" in family:
            addition += DYNAMIC_REENT.format(condition=family["dynamic_reent"])
        if "config_h" in family:
            addition += f"\n/* KickOS: the family's own configuration. */\n{family['config_h']}\n"
        config_h = os.path.join(src, "newlib", "newlib", "libc", "include", "sys", "config.h")
        with open(config_h) as f:
            text = f.read()
        if addition and "/* KickOS: " not in text:
            replace_in_file(self, config_h, "\n#endif /* __SYS_CONFIG_H__ */",
                            addition + "\n#endif /* __SYS_CONFIG_H__ */")
        # GETREENT_PROVIDED on the command line: getreent.c tests it before it includes any
        # header. It leaves libc no fallback __getreent, which a static multilib never calls.
        newlib_env = dict(env)
        if "dynamic_reent" in family:
            newlib_env["CFLAGS_FOR_TARGET"] += " -DGETREENT_PROVIDED"
        if "newlib_include" in family:
            newlib_env["CFLAGS_FOR_TARGET"] += " -I" + os.path.join(src, family["newlib_include"])
        self._newlib(os.path.join(self.build_folder, "newlib"), stage, family["newlib"],
                     newlib_env)
        self._drop_members(stage, family.get("drop", []))
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
        self._check_dropped(stage, family.get("drop", []))
        self._check_guards(stage, family.get("guards", []))
        self._check_links(stage, "nano" in family)

    def _libc_archives(self, stage):
        lib = os.path.join(stage, self._target(), "lib")
        return sorted(glob.glob(os.path.join(lib, "**", "libc.a"), recursive=True)
                      + glob.glob(os.path.join(lib, "**", "libg.a"), recursive=True))

    def _members(self, stage, archive):
        ar = os.path.join(stage, "bin", f"{self._target()}-ar")
        out = subprocess.run([ar, "t", archive], capture_output=True, text=True, check=True)
        return out.stdout.split()

    def _drop_members(self, stage, names):
        # From libc.a and libg.a both, by name: newlib installs libg.a as a hard link to libc.a
        # where it can and a copy where it cannot, and an edit of one reaches only a link.
        if not names:
            return
        ar = os.path.join(stage, "bin", f"{self._target()}-ar")
        for archive in self._libc_archives(stage):
            present = [m for m in names if m in self._members(stage, archive)]
            if present:
                self._run([ar, "d", archive, *present], os.path.join(self.build_folder, "drop"))

    def _multilib_dirs(self, stage):
        # Every library directory the built compiler selects, from its own -print-multi-lib.
        target = self._target()
        gcc = os.path.join(stage, "bin", f"{target}-gcc")
        out = subprocess.run([gcc, "-print-multi-lib"], capture_output=True, text=True,
                             check=True)
        lib = os.path.join(stage, target, "lib")
        return [os.path.normpath(os.path.join(lib, line.split(";")[0]))
                for line in out.stdout.split()]

    def _check_dropped(self, stage, names):
        # Both archives in every multilib directory, required before their members are read:
        # the deletion is a claim about each, and a missing one would pass unread.
        if not names:
            return
        for where in self._multilib_dirs(stage):
            for archive in (os.path.join(where, "libc.a"), os.path.join(where, "libg.a")):
                if not os.path.isfile(archive):
                    raise ConanInvalidConfiguration(
                        f"{archive} is missing, so the deletion of {', '.join(names)} "
                        "from it cannot be checked")
                kept = [m for m in names if m in self._members(stage, archive)]
                if kept:
                    raise ConanInvalidConfiguration(
                        f"{archive} still holds {', '.join(kept)}, which this family deletes")

    def _check_guards(self, stage, guards):
        # Every archive the package installs, newlib's and libstdc++'s under the target's lib
        # and libgcc.a under GCC's, whether or not an image links it today.
        if not guards:
            return
        target = self._target()
        roots = [os.path.join(stage, target, "lib"), os.path.join(stage, "lib", "gcc", target)]
        archives = sorted(a for root in roots
                          for a in glob.glob(os.path.join(root, "**", "*.a"), recursive=True))
        if not archives:
            raise ConanInvalidConfiguration(f"no archive under {' or '.join(roots)} to guard")
        readelf = os.path.join(stage, "bin", f"{target}-readelf")
        where = os.path.join(self.build_folder, "guards")
        os.makedirs(where, exist_ok=True)
        for guard in guards:
            script = os.path.join(self.recipe_folder, "tools", guard)
            for archive in archives:
                run = subprocess.run(["sh", script, readelf, archive], capture_output=True,
                                     text=True)
                with open(os.path.join(where, "kickos-build.log"), "a") as log:
                    log.write(f"$ sh {script} {readelf} {archive}\n{run.stdout}{run.stderr}")
                if run.returncode != 0:
                    lines = run.stderr.splitlines()
                    shown = lines[:20] + (["..."] + lines[-1:] if len(lines) > 21 else lines[20:])
                    raise ConanInvalidConfiguration(
                        f"{guard} refuses {archive}:\n" + "\n".join(shown)
                        + f"\nits whole output is in {where}/kickos-build.log")
                self.output.info(f"{os.path.relpath(archive, stage)}: {run.stdout.strip()}")

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
        # __getreent is the runtime's where the libc is dynamic-reent, and unused elsewhere.
        save(self, source, "#include <reent.h>\n#include <stdexcept>\n#include <string>\n"
             "extern \"C\" struct _reent* __getreent(void) { return _impure_ptr; }\n"
             "static int run() { try { throw std::runtime_error(std::string(\"k\")); }\n"
             "  catch (const std::exception& e) { return e.what()[0] == 'k' ? 0 : 1; } }\n"
             "int main() { return run(); }\n"
             "extern \"C\" [[gnu::weak]] void _start() { run(); }\n")
        cxx = os.path.join(stage, "bin", f"{target}-g++")
        profiles = [[]] + ([["--specs=nano.specs"]] if nano else [])
        for flags in FAMILIES[target].get("check", [[]]):
            for profile in profiles:
                self._run([cxx, *flags, *profile, *FAMILIES[target].get("link", []),
                           "--specs=nosys.specs", source,
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
        save(self, os.path.join(self.package_folder, "kickos-toolchain.txt"),
             "".join(f"{k}={self._archive(k)}\n" for k in self._sources())
             + f"target={self._target()}\n")

    def package_info(self):
        self.cpp_info.bindirs = ["bin"]
