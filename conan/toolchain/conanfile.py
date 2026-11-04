# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

import collections
import contextlib
import glob
import hashlib
import lzma
import os
import shutil
import subprocess
import tarfile
import threading
import time

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
# the extra newlib profile a family ships, installed beside the full one as nano.specs expects:
# libc_nano.a, libg_nano.a, libstdc++_nano.a and libsupc++_nano.a in every multilib directory,
# and newlib.h under include/newlib-nano; `nano_specs` installs NANO_SPECS as nano.specs in every
# multilib directory, in place of the family's own, with `nano_lib` ending its lib spec.
NANO = ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
        "--disable-newlib-fseek-optimization", "--disable-newlib-fvwrite-in-streamio",
        "--disable-newlib-mb", "--enable-newlib-nano-formatted-io",
        "--enable-newlib-nano-malloc", "--disable-newlib-unbuf-stream-opt",
        "--disable-newlib-io-c99-formats", "--disable-newlib-io-long-long",
        "--enable-newlib-reent-small", "--disable-newlib-register-fini",
        "--disable-newlib-wide-orient"]

FAMILIES = {
    # armv6m has no compare-and-swap, and its reference counts call the user runtime's atomics.
    "arm-none-eabi": {
        "patches": {"gcc": ["kickos-libstdcxx-atomic-calls"]},
        "gcc": ["--with-multilib-list=rmprofile"],
        "cflags": "-g -O2 -ffunction-sections -fdata-sections",
        "newlib": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                   "--enable-newlib-register-fini", "--enable-newlib-mb"],
        # The multilibs the link check below links a full and, where there is one, a nano C++
        # program for: a missing archive fails the package, not a board's first link.
        "check": [["-mcpu=cortex-m0", "-mfloat-abi=soft", "-mthumb"],
                  ["-mcpu=cortex-m4", "-mfpu=fpv4-sp-d16", "-mfloat-abi=softfp", "-mthumb"]],
        "nano": NANO,
    },
    "aarch64-none-elf": {
        "gcc": [],
        "cflags": "-g -O2 -ffunction-sections -fdata-sections",
        "newlib": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                   "--enable-newlib-register-fini", "--enable-newlib-mb"],
        "check": [["-mcpu=cortex-a53"]],
    },
    # One compiler for both widths: rv64 is the default multilib, rv32 the other, every library
    # medany, which rv64's image window needs and rv32's does not mind. Both have the A
    # extension, so shared_ptr counts are atomic, where libstdc++'s RISC-V default keeps a
    # mutex for another OS's ABI.
    "riscv64-none-elf": {
        "gcc": ["--with-arch=rv64imac_zicsr_zmmul_zaamo_zalrsc_zca", "--with-abi=lp64",
                "--with-cmodel=medany", "--with-libstdcxx-lock-policy=atomic",
                "--with-multilib-generator=rv64imac_zicsr_zmmul_zaamo_zalrsc_zca-lp64--;"
                "rv32imac_zicsr-ilp32--"],
        "cflags": "-g -Os -ftls-model=local-exec -ffunction-sections -fdata-sections",
        "newlib": ["--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
                   "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
                   "--enable-newlib-register-fini", "--enable-newlib-mb",
                   "--enable-newlib-atexit-dynamic-alloc"],
        "check": [["-march=rv32imac_zicsr", "-mabi=ilp32"],
                  ["-march=rv64imac_zicsr_zmmul_zaamo_zalrsc_zca", "-mabi=lp64",
                   "-mcmodel=medany"]],
        "nano": NANO,
        "nano_specs": True,
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
        "check": [["-mlongcalls"]],
        "link": ["-nostartfiles"],
        "nano": NANO,
        "nano_specs": True,
    },
    # The common sources with Renesas's changes ported onto them, as upstream's GCC and binutils
    # know no RXv3 or double-precision FPU. The multilib patch keeps four of GNURX's 104
    # multilibs; -misa=v3 -mdfpu selects 64-bit-double/dfpu/rxv3, as -mdfpu makes doubles
    # 64-bit. KickOS's compare patch corrects GNURX's unordered double-precision branches, its
    # atomics patch calls __atomic_*_4 for a 32-bit read-modify-write, as the inline form masks
    # interrupts, which user mode ignores, and the last counts libstdc++'s references through
    # those calls, which the user runtime defines. Each touched file is a generated one the
    # patches carry, made newer than its inputs so that no maintainer tool runs. newlib's
    # register_fini stays off: it needs `_fini`, which RX's crt0 lacks.
    "rx-elf": {
        "patches": {"gcc": ["gnurx-gcc", "rx-multilib", "kickos-rx-dfpu-compare",
                            "kickos-rx-irq-atomics", "kickos-libstdcxx-atomic-calls"],
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
        "nano": NANO,
        # The full lib spec's default linker script, which GNURX's nano.specs drops.
        "nano_specs": True,
        "nano_lib": "%{!T*: %{msim:%Trx-sim.ld}%{!msim:%Trx.ld}}",
    },
}

# Every _REENT use calls __getreent(), which KickOS answers per thread, on every family.
DYNAMIC_REENT = """
/* KickOS: dynamic reentrancy. */
#ifndef __DYNAMIC_REENT__
#define __DYNAMIC_REENT__
#endif
"""

# newlib's libgloss/arm/elf-nano.specs less its rdimon lines: the nano header first, and the nano
# archives in place of the full ones.
NANO_SPECS = """%rename link                nano_link
%rename link_gcc_c_sequence                nano_link_gcc_c_sequence
%rename cpp_unique_options                nano_cpp_unique_options

*cpp_unique_options:
-isystem =/include/newlib-nano %(nano_cpp_unique_options)

*nano_libc:
-lc_nano

*nano_libgloss:
%{specs=nosys.specs:-lnosys}

*link_gcc_c_sequence:
%(nano_link_gcc_c_sequence) --start-group %G %(nano_libc) %(nano_libgloss) --end-group

*link:
%(nano_link) %:replace-outfile(-lc -lc_nano) %:replace-outfile(-lg -lg_nano) \
%:replace-outfile(-lstdc++ -lstdc++_nano) %:replace-outfile(-lsupc++ -lsupc++_nano)

*lib:
%{!shared:%{g*:-lg_nano} %{!p:%{!pg:-lc_nano}}%{p:-lc_p}%{pg:-lc_p}}
"""

# The predefined macros of the multilibs whose KickOS user runtime defines the __atomic calls a
# read-modify-write makes there, as kickos-libstdcxx-atomic-calls.patch reads them.
USER_ATOMICS = ("__ARM_ARCH_6M__", "__RX__")

# Every component a family builds, and the source key each takes unless the family names another.
COMPONENTS = ("gcc", "binutils", "newlib")

# The patches every family applies after its own, and the generated files they carry, made newer
# than their inputs: the KickOS threads model in GCC, and newlib's locks.
COMMON = {"patches": {"gcc": ["kickos-gcc-threads"], "newlib": ["kickos-newlib-locks"]},
          "touch": {"gcc": ["gcc/configure", "libgcc/configure", "libstdc++-v3/configure"]}}
PREREQUISITES = ("gmp", "mpfr", "mpc", "isl")

# Every guard a family names, exported for every family, as the recipe revision is one for all.
GUARDS = sorted({g for family in FAMILIES.values() for g in family.get("guards", [])})

PREFIX = "kickos-toolchain-"

# The program every check multilib links, full and nano: an exception, the C++ thread library and
# a FILE opened and closed. It stands in for the KickOS user runtime, and for __dso_handle where
# no crtbegin.o gives one. It compiles only where the threads model is kickos, the newlib lock is
# the KickOS lock, reentrancy is dynamic and the newlib header is the profile's own
# (KICKOS_CHECK_NANO), and links only where every gthreads operation is a __kickos_gthr_* call and
# every FILE lock is a struct __lock in the FILE.
CHECK = r"""
#include <reent.h>
#include <stdio.h>
#include <sys/lock.h>
#include <time.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#if not defined(_GLIBCXX_HAS_GTHREADS) or not defined(_GLIBCXX_GCC_GTHR_KICKOS_H)
#error "the threads model is not kickos"
#endif
#if not defined(__DYNAMIC_REENT__)
#error "reentrancy is not dynamic"
#endif
#if defined(_NANO_FORMATTED_IO) != KICKOS_CHECK_NANO
#error "the newlib header is not the profile's"
#endif

static_assert(sizeof(__gthread_mutex_t) == 4 and alignof(__gthread_mutex_t) == 4, "mutex");
static_assert(sizeof(__gthread_recursive_mutex_t) == 8, "recursive mutex");
static_assert(sizeof(__gthread_cond_t) == 4 and sizeof(__gthread_once_t) == 4, "cond, once");
static_assert(sizeof(__gthread_key_t) == 4 and sizeof(__gthread_t) == 4, "key, thread");
static_assert(sizeof(struct __lock) == 8 and alignof(struct __lock) == 4, "newlib lock");
static_assert(__is_same(_flock_t, struct __lock), "FILE lock");

extern "C" {
struct _reent* __getreent(void) { return &_impure_data; }

#define G(name, ...) int __kickos_gthr_##name(__VA_ARGS__) { return 0; }
G(once, __gthread_once_t*, void (*)(void))
G(key_create, __gthread_key_t*, void (*)(void*))
G(key_delete, __gthread_key_t)
G(setspecific, __gthread_key_t, const void*)
G(mutex_destroy, __gthread_mutex_t*)
G(mutex_lock, __gthread_mutex_t*)
G(mutex_trylock, __gthread_mutex_t*)
G(mutex_timedlock, __gthread_mutex_t*, const __gthread_time_t*)
G(mutex_clocklock, __gthread_mutex_t*, __clockid_t, const __gthread_time_t*)
G(mutex_unlock, __gthread_mutex_t*)
G(recursive_mutex_destroy, __gthread_recursive_mutex_t*)
G(recursive_mutex_lock, __gthread_recursive_mutex_t*)
G(recursive_mutex_trylock, __gthread_recursive_mutex_t*)
G(recursive_mutex_timedlock, __gthread_recursive_mutex_t*, const __gthread_time_t*)
G(recursive_mutex_clocklock, __gthread_recursive_mutex_t*, __clockid_t, const __gthread_time_t*)
G(recursive_mutex_unlock, __gthread_recursive_mutex_t*)
G(cond_destroy, __gthread_cond_t*)
G(cond_broadcast, __gthread_cond_t*)
G(cond_signal, __gthread_cond_t*)
G(cond_wait, __gthread_cond_t*, __gthread_mutex_t*)
G(cond_timedwait, __gthread_cond_t*, __gthread_mutex_t*, const __gthread_time_t*)
G(cond_clockwait, __gthread_cond_t*, __gthread_mutex_t*, __clockid_t, const __gthread_time_t*)
G(cond_wait_recursive, __gthread_cond_t*, __gthread_recursive_mutex_t*)
G(create, __gthread_t*, void* (*)(void*), void*)
G(join, __gthread_t, void**)
G(detach, __gthread_t)
G(yield, void)
#if defined(__ARM_ARCH_6M__) or defined(__RX__)
#define KICKOS_CHECK_STR2(x) #x
#define KICKOS_CHECK_STR(x) KICKOS_CHECK_STR2(x)
unsigned kickos_check_fetch_add_4(volatile void*, unsigned, int)
    __asm__(KICKOS_CHECK_STR(__USER_LABEL_PREFIX__) "__atomic_fetch_add_4");
unsigned kickos_check_fetch_add_4(volatile void* p, unsigned v, int)
{
    unsigned volatile* w = static_cast<unsigned volatile*>(p);
    unsigned const was = *w;
    *w = was + v;
    return was;
}
#endif
void* __kickos_gthr_getspecific(__gthread_key_t) { return 0; }
__gthread_t __kickos_gthr_self(void) { return 1; }
int clock_gettime(clockid_t, struct timespec* ts) { ts->tv_sec = 0; ts->tv_nsec = 0; return 0; }
int nanosleep(const struct timespec*, struct timespec*) { return 0; }
long sysconf(int) { return 1; }
[[gnu::weak]] void* __dso_handle = &__dso_handle;

struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;
void __retarget_lock_init(_LOCK_T*) {}
void __retarget_lock_init_recursive(_LOCK_T*) {}
void __retarget_lock_close(_LOCK_T) {}
void __retarget_lock_close_recursive(_LOCK_T) {}
void __retarget_lock_acquire(_LOCK_T l) { l->__word = 1; }
void __retarget_lock_acquire_recursive(_LOCK_T l) { l->__depth++; }
int __retarget_lock_try_acquire(_LOCK_T l) { l->__word = 1; return 1; }
int __retarget_lock_try_acquire_recursive(_LOCK_T l) { l->__depth++; return 1; }
void __retarget_lock_release(_LOCK_T l) { l->__word = 0; }
void __retarget_lock_release_recursive(_LOCK_T l) { l->__depth--; }
}

static int run() {
  try { throw std::runtime_error(std::string("k")); }
  catch (const std::exception& e) { if (e.what()[0] != 'k') { return 1; } }
  std::mutex m;
  std::condition_variable cv;
  std::once_flag once;
  int calls = 0;
  std::call_once(once, [&] { calls++; });
  std::thread worker([&] {
    std::lock_guard<std::mutex> hold(m);
    calls++;
    cv.notify_one();
  });
  {
    std::unique_lock<std::mutex> hold(m);
    cv.wait_for(hold, std::chrono::milliseconds(1), [&] { return calls == 2; });
  }
  worker.join();
  std::timed_mutex timed;
  std::recursive_timed_mutex rtimed;
  if (not timed.try_lock_for(std::chrono::milliseconds(1))
      or not rtimed.try_lock_until(std::chrono::system_clock::now())) {
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::yield();
  static std::string guarded(std::chrono::steady_clock::now().time_since_epoch().count() & 1, 'k');
  FILE* f = fopen("k", "r");
  if (f) { fclose(f); }
  if (calls != 2 or guarded.size() > 1) { return 1; }
  return 0;
}

int main() { return run(); }
extern "C" [[gnu::weak]] void _start() { run(); }
"""

# This version's release is the tag toolchain-<version>-<PACKAGE_REVISION>. The shell tools and
# the release workflow read this line and `version` by pattern, and conan/toolchains reads
# `version`.
PACKAGE_REVISION = 1

# The GitHub release's one archive of every pinned source and patch (design section 1). A source
# conandata.yml gives a `tar_sha256` is stored as .tar.xz in place of its .tar.gz, and checked by
# the hash of its uncompressed tar.
RELEASE = "https://github.com/leducp/KickOS/releases/download/toolchain-{version}-{revision}/"
SOURCES = "kickos-toolchain-sources-{version}-{revision}"


class KickOSToolchain(ConanFile):
    """One family of the KickOS toolchain: binutils, GCC, newlib and libstdc++ built together from
    the pinned sources in conandata.yml, for the host this package runs on.

    Exported once per family under the family's name, kickos-toolchain-<triple>, so a consumer
    can require several families at once: Conan refuses one package required twice with
    different options. Conan keys the package on the host's os and arch; the compiler that
    built it is no part of what it is.
    """

    version = "1.2.0"
    license = "GPL-3.0-or-later WITH GCC-exception-3.1 AND BSD-3-Clause (newlib COPYING.NEWLIB)"
    description = "The KickOS cross toolchain for one target family"
    package_type = "application"
    settings = "os", "arch"
    # `exports`, not `exports_sources`: `conan cache save --no-source` keeps the export folder.
    exports = "conandata.yml", "patches/*"
    _jobserver = None

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

    def _patches(self, component):
        family = FAMILIES[self._target()]
        return (family.get("patches", {}).get(component, [])
                + COMMON["patches"].get(component, []))

    def _touched(self, component):
        family = FAMILIES[self._target()]
        return family.get("touch", {}).get(component, []) + COMMON["touch"].get(component, [])

    def _sources(self):
        family = FAMILIES[self._target()]
        overlay = ["xtensa-overlays"] if "overlay" in family else []
        patches = [k for c in COMPONENTS for k in self._patches(c)]
        return [self._key(c) for c in COMPONENTS] + list(PREREQUISITES) + overlay + patches

    def _archive(self, key):
        source = self.conan_data["sources"][key]
        return source["filename"] if "filename" in source else os.path.basename(source["url"])

    def _stored(self, key):
        # The name the sources archive holds the source under: conandata.yml's `stored`, which
        # tools/kickos-toolchain-release.sh reads too, else the archive's own.
        return self.conan_data["sources"][key].get("stored", self._archive(key))

    def _sources_top(self):
        return SOURCES.format(version=self.version, revision=PACKAGE_REVISION)

    def _local(self, key, destination):
        # The copy KICKOS_TOOLCHAIN_SOURCES holds, a pinned archive or the release's sources
        # archive, checked as a download is, or "".
        local = os.environ.get("KICKOS_TOOLCHAIN_SOURCES", "")
        if not local:
            return ""
        path = os.path.join(local, self._archive(key))
        if os.path.isfile(path):
            return self._checked(key, path)
        archive = os.path.join(local, self._sources_top() + ".tar.xz")
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
                member = tar.getmember(f"{self._sources_top()}/{name}")
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
                               self._sources_top() + ".tar.xz")
        if not os.path.isfile(archive):
            release = RELEASE.format(version=self.version, revision=PACKAGE_REVISION)
            download(self, release + os.path.basename(archive), archive)
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
            for component in COMPONENTS:
                tree = os.path.join(self.source_folder, component)
                for key in self._patches(component):
                    self._patch(key, tree)
                for generated in self._touched(component):
                    if os.path.isfile(os.path.join(tree, generated)):
                        os.utime(os.path.join(tree, generated))
        finally:
            sources = os.path.join(os.path.dirname(self.source_folder),
                                   self._sources_top() + ".tar.xz")
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

    def _run(self, argv, cwd, env=None, pass_fds=()):
        os.makedirs(cwd, exist_ok=True)
        path = os.path.join(cwd, "kickos-build.log")
        with open(path, "a") as log:
            log.write("$ " + " ".join(argv) + "\n")
            log.flush()
            rc = subprocess.run(argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT,
                                pass_fds=pass_fds)
        if rc.returncode != 0:
            with open(path, errors="replace") as log:
                tail = collections.deque(log, maxlen=80)
            print(f"--- the last {len(tail)} lines of {path} ---\n" + "".join(tail), flush=True)
            raise ConanInvalidConfiguration(f"{argv[0]} failed in {cwd}; its output is in {path}")

    @staticmethod
    def _cpus():
        # The CPUs this process may use, which an affinity mask or a cpuset makes fewer than
        # the machine's.
        return getattr(os, "process_cpu_count", os.cpu_count)() or 1

    def _make(self, cwd, *targets, env=None, jobs=None):
        if jobs is None and self._jobserver:
            # A make given -j would start a jobserver of its own instead of joining this one.
            read, write = self._jobserver
            env = dict(os.environ if env is None else env)
            env["MAKEFLAGS"] = f"-j{self._cpus()} --jobserver-auth={read},{write}"
            self._run(["make", "MAKEINFO=true", *targets], cwd, env, pass_fds=(read, write))
            return
        if jobs is None:
            jobs = self._cpus()
        self._run(["make", "-j", str(jobs), "MAKEINFO=true", *targets], cwd, env)

    def _free(self):
        return f"{shutil.disk_usage(self.build_folder).free / (1 << 30):.1f}G free"

    @contextlib.contextmanager
    def _timed(self, stage):
        start = time.monotonic()
        self.output.info(f"{time.strftime('%H:%M:%S')} {stage}: begins, {self._free()}")
        outcome = "failed after"
        try:
            yield
            outcome = "done in"
        finally:
            minutes, seconds = divmod(int(time.monotonic() - start), 60)
            self.output.info(f"{time.strftime('%H:%M:%S')} {stage}: {outcome} "
                             f"{minutes}m{seconds:02d}s, {self._free()}")

    def _together(self, *stages):
        # Each (name, function) at once, their makes sharing one jobserver of a job per CPU; the
        # first failure is raised once all have ended. A make runs one job without a token, and a
        # stage that ends hands that job on.
        if self._cpus() < len(stages):
            for name, function in stages:
                with self._timed(name):
                    function()
            return
        failures = []
        read, write = os.pipe()
        os.write(write, b"+" * max(0, self._cpus() - len(stages)))
        self._jobserver = (read, write)

        def run(name, function):
            try:
                with self._timed(name):
                    function()
            except BaseException as e:
                failures.append(e)
            finally:
                os.write(write, b"+")

        try:
            threads = [threading.Thread(target=run, args=stage) for stage in stages]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
        finally:
            self._jobserver = None
            os.close(read)
            os.close(write)
        if failures:
            raise failures[0]

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
        with self._timed("binutils"):
            self._run([os.path.join(src, "binutils", "configure"), f"--target={target}",
                       f"--prefix={stage}", "--disable-nls", "--disable-werror", "--disable-gdb",
                       "--disable-gdbserver", "--disable-sim", "--disable-gprofng",
                       *family.get("binutils", [])], binutils, env)
            self._make(binutils, env=env)
            # One job: each manual's install-info rewrites share/info/dir, and two at once lose
            # an entry.
            self._make(binutils, "install-strip", env=env, jobs=1)
        shutil.rmtree(binutils)

        # The one compiler: installed alone to build newlib, then its target libraries in the
        # same tree once newlib is in the sysroot.
        gcc = os.path.join(self.build_folder, "gcc")
        with self._timed("gcc"):
            self._run([os.path.join(src, "gcc", "configure"),
                       *self._gcc_options(stage, os.path.join(stage, target)),
                       "--enable-languages=c,c++"], gcc, env)
            self._make(gcc, "all-gcc", env=env)
            self._make(gcc, "install-strip-gcc", env=env)

        addition = DYNAMIC_REENT
        if "config_h" in family:
            addition += f"\n/* KickOS: the family's own configuration. */\n{family['config_h']}\n"
        config_h = os.path.join(src, "newlib", "newlib", "libc", "include", "sys", "config.h")
        with open(config_h) as f:
            text = f.read()
        if "/* KickOS: " not in text:
            replace_in_file(self, config_h, "\n#endif /* __SYS_CONFIG_H__ */",
                            addition + "\n#endif /* __SYS_CONFIG_H__ */")
        # GETREENT_PROVIDED on the command line: getreent.c tests it before it includes any
        # header. It leaves libc no fallback __getreent: the KickOS runtime defines it.
        newlib_env = dict(env)
        newlib_env["CFLAGS_FOR_TARGET"] += " -DGETREENT_PROVIDED"
        if "newlib_include" in family:
            newlib_env["CFLAGS_FOR_TARGET"] += " -I" + os.path.join(src, family["newlib_include"])

        # A build tree goes once nothing reads it: a CI runner's disk does not hold them all.
        def newlib():
            where = os.path.join(self.build_folder, "newlib")
            self._newlib(where, stage, family["newlib"], newlib_env)
            self._drop_members(stage, family.get("drop", []))
            shutil.rmtree(where)

        def newlib_nano():
            where = os.path.join(self.build_folder, "newlib-nano")
            self._newlib(where, nano, family["nano"], newlib_env)
            shutil.rmtree(where)

        nano = os.path.join(self.build_folder, "nano-stage")
        stages = [("newlib", newlib)]
        if "nano" in family:
            stages.append(("newlib-nano", newlib_nano))
        self._together(*stages)
        if "nano" in family:
            self._install_nano(nano, stage)

        with self._timed("libgcc"):
            self._reconfigure_gcc(gcc, env)
            self._make(gcc, "all-gcc", "all-target-libgcc", env=env)
        # Neither make rebuilds what the other reads: the compiler and libgcc are left alone.
        built = ["-o", "all-gcc", "-o", "all-target-libgcc"]
        stages = [("libstdc++", lambda: self._make(gcc, *built, "all-target-libstdc++-v3",
                                                   env=env))]
        if "nano" in family:
            stages.append(("libstdc++-nano", lambda: self._nano_cxx(gcc, nano, built, env)))
        self._together(*stages)
        with self._timed("install"):
            self._make(gcc, env=env)
            self._make(gcc, "install-strip", env=env)

        shutil.rmtree(gcc)
        nano_cxx = os.path.join(self.build_folder, "nano-cxx-stage")
        if "nano" in family:
            self._install_nano_cxx(stage)
            shutil.rmtree(nano)
        with self._timed("checks"):
            self._check_dropped(stage, family.get("drop", []))
            self._check_guards(stage, family.get("guards", []))
            self._check_thread_model(stage)
            self._check_reent(stage)
            self._check_lock_policy(stage, stage)
            self._check_random_device(stage, stage)
            self._check_clock_waits(stage, stage)
            if "nano" in family:
                self._check_lock_policy(stage, nano_cxx)
                self._check_random_device(stage, nano_cxx)
                self._check_clock_waits(stage, nano_cxx)
                self._check_nano(stage)
            self._check_links(stage, "nano" in family)
        if "nano" in family:
            shutil.rmtree(nano_cxx)

    def _reconfigure_gcc(self, gcc, env):
        # GCC's configure, run before newlib was in the sysroot, took the target to have no C
        # library headers (inhibit_libc), and its build made limits.h and the fixed headers
        # without them. Configured again through the tree's own rule, from its cache, and both
        # made again from the headers now installed. A configargs.h left in place appends
        # "(reconfigured)" to what gcc -v prints.
        host = os.path.join(gcc, "gcc")
        for made in ("Makefile", "configargs.h", "stmp-fixinc", "stmp-int-hdrs"):
            if os.path.isfile(os.path.join(host, made)):
                os.remove(os.path.join(host, made))
        self._make(gcc, "configure-gcc", env=env)

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
                "--disable-shared", "--enable-threads=kickos", "--disable-nls", "--disable-libssp",
                "--disable-libgomp", "--disable-libquadmath", "--disable-libstdcxx-pch",
                "--enable-checking=release", *FAMILIES[self._target()]["gcc"]]

    def _nano_cxx(self, gcc, nano, built, env):
        # libstdc++ and libsupc++ again, every multilib, from the same compiler over the nano
        # newlib's sysroot, in a target directory of their own under a prefix of their own. Run
        # from a folder of their own, which holds their log.
        target = self._target()
        overrides = ["-C", gcc, f"prefix={os.path.join(self.build_folder, 'nano-cxx-stage')}",
                     f"TARGET_SUBDIR={target}-nano",
                     f"SYSROOT_CFLAGS_FOR_TARGET=--sysroot={os.path.join(nano, target)}"]
        # Each multilib's configure tests the threads model's header in libgcc's build folder
        # beside its own, which only the other target directory holds.
        libgcc = os.path.join(gcc, target)
        for header in glob.glob(os.path.join(libgcc, "**", "libgcc", "gthr-default.h"),
                                recursive=True):
            mirror = os.path.join(gcc, f"{target}-nano", os.path.relpath(header, libgcc))
            os.makedirs(os.path.dirname(mirror), exist_ok=True)
            shutil.copyfile(header, mirror)
        where = os.path.join(self.build_folder, "nano-cxx")
        self._make(where, *overrides, *built, "all-target-libstdc++-v3", env=env)
        self._make(where, *overrides, "-o", "install-target-libgcc",
                   "install-target-libstdc++-v3", env=env)
        shutil.rmtree(where)

    def _install_nano_cxx(self, stage):
        target = self._target()
        lib = os.path.join(self.build_folder, "nano-cxx-stage", target, "lib")
        for archive in glob.glob(os.path.join(lib, "**", "libstdc++.a"), recursive=True):
            here = os.path.dirname(archive)
            dest = os.path.join(stage, target, "lib", os.path.relpath(here, lib))
            shutil.copyfile(archive, os.path.join(dest, "libstdc++_nano.a"))
            shutil.copyfile(os.path.join(here, "libsupc++.a"),
                            os.path.join(dest, "libsupc++_nano.a"))

    def _check_thread_model(self, stage):
        gcc = os.path.join(stage, "bin", f"{self._target()}-gcc")
        out = subprocess.run([gcc, "-v"], capture_output=True, text=True, check=True)
        if "Thread model: kickos\n" not in out.stderr:
            raise ConanInvalidConfiguration(f"{gcc} -v names no kickos thread model")

    def _check_reent(self, stage):
        # Every member reaches its struct _reent through __getreent(), none through _impure_ptr,
        # spelled with the target's prefix to C names.
        target = self._target()
        nm = os.path.join(stage, "bin", f"{target}-nm")
        gcc = os.path.join(stage, "bin", f"{target}-gcc")
        macros = subprocess.run([gcc, "-E", "-dM", "-x", "c", os.devnull], capture_output=True,
                                text=True, check=True).stdout
        prefix = ""
        for line in macros.splitlines():
            if line.startswith("#define __USER_LABEL_PREFIX__ "):
                prefix = line.split(" ", 2)[2].strip()
        impure = f" U {prefix}_impure_ptr"
        roots = [os.path.join(stage, target, "lib"), os.path.join(stage, "lib", "gcc", target)]
        for archive in sorted(a for root in roots
                              for a in glob.glob(os.path.join(root, "**", "*.a"), recursive=True)):
            out = subprocess.run([nm, "-A", archive], capture_output=True, text=True, check=True)
            users = [line.split(":")[1] for line in out.stdout.splitlines()
                     if line.endswith(impure)]
            if users:
                raise ConanInvalidConfiguration(
                    f"{archive} reaches _impure_ptr from {', '.join(sorted(set(users)))}")

    def _cxx_configs(self, stage, headers):
        # Each multilib's flags and its c++config.h, in the libstdc++ whose headers the prefix
        # `headers` holds.
        target = self._target()
        gcc = os.path.join(stage, "bin", f"{target}-gcc")
        out = subprocess.run([gcc, "-print-multi-lib"], capture_output=True, text=True,
                             check=True)
        version = subprocess.run([gcc, "-dumpversion"], capture_output=True, text=True,
                                 check=True).stdout.strip()
        configs = []
        for line in out.stdout.split():
            where, flags = line.split(";", 1)
            configs.append((["-" + f for f in flags.split("@") if f],
                            os.path.normpath(os.path.join(headers, target, "include", "c++",
                                                          version, target, where, "bits",
                                                          "c++config.h"))))
        return configs

    def _check_lock_policy(self, stage, headers):
        # shared_ptr counts take the atomic lock policy in every multilib whose compiler has the
        # compare-and-swap libstdc++ asks for. In every other, a 4-byte read-modify-write is a
        # call, and a reference count is one of those calls where the KickOS user runtime
        # defines them (USER_ATOMICS) and an update under libstdc++'s mutex elsewhere.
        gcc = os.path.join(stage, "bin", f"{self._target()}-gcc")
        probe = "int w; int f(int v) { return __atomic_fetch_add(&w, v, __ATOMIC_ACQ_REL); }"
        for flags, config in self._cxx_configs(stage, headers):
            macros = subprocess.run([gcc, *flags, "-E", "-dM", "-x", "c", os.devnull],
                                    capture_output=True, text=True, check=True).stdout
            swap = all(f"#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_{n} 1" in macros
                       for n in (2, 4))
            calls = any(f"#define {m} 1" in macros for m in USER_ATOMICS)
            with open(config) as f:
                text = f.read()
            atomic = "\n#define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1\n" in text
            builtins = "\n#define _GLIBCXX_ATOMIC_WORD_BUILTINS 1\n" in text
            if swap and not atomic:
                raise ConanInvalidConfiguration(
                    f"{config}: shared_ptr counts take a mutex where the multilib has a "
                    "compare-and-swap")
            if atomic and not swap:
                raise ConanInvalidConfiguration(
                    f"{config}: shared_ptr counts are atomic where the multilib has no "
                    "compare-and-swap")
            if swap:
                continue
            asm = subprocess.run([gcc, *flags, "-O2", "-S", "-o", "-", "-x", "c", "-"],
                                 input=probe, capture_output=True, text=True, check=True).stdout
            if "__atomic_fetch_add_4" not in asm:
                raise ConanInvalidConfiguration(
                    f"{config}: a 4-byte read-modify-write is inline where the multilib has no "
                    "compare-and-swap")
            if calls and not builtins:
                raise ConanInvalidConfiguration(
                    f"{config}: reference counts take libstdc++'s mutex where the KickOS user "
                    "runtime defines the __atomic calls")
            if builtins and not calls:
                raise ConanInvalidConfiguration(
                    f"{config}: reference counts call __atomic builtins the KickOS user runtime "
                    "does not define")

    def _check_random_device(self, stage, headers):
        # std::random_device draws from getentropy and from nothing else, in every multilib.
        others = ("_GLIBCXX_HAVE_ARC4RANDOM", "_GLIBCXX_X86_RDRAND", "_GLIBCXX_X86_RDSEED",
                  "_GLIBCXX_USE_DEV_RANDOM")
        for _, config in self._cxx_configs(stage, headers):
            with open(config) as f:
                text = f.read()
            if "\n#define _GLIBCXX_HAVE_GETENTROPY 1\n" not in text:
                raise ConanInvalidConfiguration(f"{config}: std::random_device has no getentropy")
            found = [m for m in others if f"\n#define {m} " in text]
            if found:
                raise ConanInvalidConfiguration(
                    f"{config}: std::random_device also draws from {', '.join(found)}")

    def _check_clock_waits(self, stage, headers):
        # A timed mutex and a condition variable wait on the clock their deadline names, through
        # the threads model's clock waits, in every multilib.
        wanted = ("_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT", "_GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK")
        for _, config in self._cxx_configs(stage, headers):
            with open(config) as f:
                text = f.read()
            missing = [m for m in wanted if f"\n#define {m} 1\n" not in text]
            if missing:
                raise ConanInvalidConfiguration(
                    f"{config}: no clock wait, {', '.join(missing)} undefined")

    def _check_nano(self, stage):
        # Every multilib directory holds the nano archives, and nano.specs where the family
        # installs its own.
        names = ["libc_nano.a", "libg_nano.a", "libstdc++_nano.a", "libsupc++_nano.a"]
        if FAMILIES[self._target()].get("nano_specs"):
            names.append("nano.specs")
        for where in self._multilib_dirs(stage):
            missing = [n for n in names if not os.path.isfile(os.path.join(where, n))]
            if missing:
                raise ConanInvalidConfiguration(f"{where} lacks {', '.join(missing)}")

    def _check_links(self, stage, nano):
        # Each profile links its own C and C++ archives and none of the other's, as the link map
        # names the archives that gave a member.
        target = self._target()
        where = os.path.join(self.build_folder, "check")
        os.makedirs(where, exist_ok=True)
        source = os.path.join(where, "check.cc")
        save(self, source, CHECK)
        cxx = os.path.join(stage, "bin", f"{target}-g++")
        full = ["libc.a", "libstdc++.a"]
        small = ["libc_nano.a", "libstdc++_nano.a"]
        profiles = [(["-DKICKOS_CHECK_NANO=0"], full, small)]
        if nano:
            profiles.append((["-DKICKOS_CHECK_NANO=1", "--specs=nano.specs"], small, full))
        link_map = os.path.join(where, "check.map")
        for flags in FAMILIES[target].get("check", [[]]):
            for profile, wanted, unwanted in profiles:
                argv = [cxx, *flags, *profile, *FAMILIES[target].get("link", []),
                        "--specs=nosys.specs", source, "-o", os.path.join(where, "check.elf"),
                        f"-Wl,-Map={link_map}"]
                self._run(argv, where)
                with open(link_map) as f:
                    used = {os.path.basename(line.split("(", 1)[0]) for line in f
                            if not line[:1].isspace() and ".a(" in line}
                faults = [f"no member of {a}" for a in wanted if a not in used]
                faults += [f"members of {a}" for a in unwanted if a in used]
                if faults:
                    raise ConanInvalidConfiguration(
                        f"{' '.join(argv)} linked {' and '.join(faults)}")

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
            if FAMILIES[target].get("nano_specs"):
                specs = NANO_SPECS
                if "nano_lib" in FAMILIES[target]:
                    specs = f"{specs.rstrip()} {FAMILIES[target]['nano_lib']}\n"
                save(self, os.path.join(dest, "nano.specs"), specs)
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
