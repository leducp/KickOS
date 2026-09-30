<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M10.2 -- the KickOS toolchain

> **Status: ACTIVE.** The design of M10.2, before any of it is built. `roadmap.md` assigns the
> milestone and carries the ruling; this page says how.

**KickOS builds its own toolchain, one per target family, from pinned sources, on the machine
that uses it.** Binutils, GCC, newlib and libstdc++ are built together, so each libstdc++ is
compiled against the newlib it links with, and C and full C++ work on every target. Today the
compilers come from five vendors, the newlib from `conan/newlib`, and the two are reconciled by
checks: the newlib release follows whatever the toolchain bundles, `cmake/cross_newlib.cmake`
swaps the toolchain's libc headers for the package's, and a desk on another vendor release builds
different bits from CI. M10.2 removes the vendors from the build.

## 1. The pinned sources

One release set for every family, bar RX:

| Source | Release | Where |
| --- | --- | --- |
| GCC | 16.2.0 | ftp.gnu.org, sha256 `e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e` |
| binutils | 2.47 | ftp.gnu.org, sha256 `154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff` |
| newlib | 4.5.0.20241231 | sourceware.org, sha256 `33f12605e0054965996c25c1382b3e463b0af91799001f5bb8c0630f2ec8c852` |
| GMP, MPFR, MPC, ISL | 6.3.0, 4.2.2, 1.3.1, 0.24 | the releases GCC 16.2's `contrib/download_prerequisites` names, with its digests |

newlib stays 4.5.0 by the M10.1 ruling: its `struct _reent` is 192 bytes smaller a thread than
4.6.0's.

**Xtensa builds from Espressif's sources**, which carry the ESP32 core and its errata: their GCC
at tag esp-16.2.0_20260914 (commit 96666a8b989e784ad3356b013e77e94a530fafe7), their binutils at
esp-2.47.0_20260914 (f5e7bfaf0786cd74c7138e32ca50359618967abf), and the `xtensa_esp32` core
overlay of their overlays repository (dd1cf19f6eb327a9db51043439974a6de13f5c7f). Espressif's own
build, their crosstool-NG sample for `xtensa-esp-elf` at the same tag, serves every Espressif chip
through a core plugin loaded at run time. KickOS targets the ESP32 alone, so the overlay is applied
to the sources and the compiler is fixed to that core, with no plugin.

**RX builds from Renesas's GNURX 14.2 sources**, the one family on another GCC release. Upstream
GCC knows the RXv1 cores only; the RXv3 instructions and the double-precision FPU the RX72M has,
and that `arch/rx/rxv3/switch.S` saves under `__RX_DFPU_INSNS__`, are Renesas's. Their download
page is behind a login, so the recipe takes a local copy of the source archive checked against
its sha256; mirroring the GPL sources as a release asset of this project is what lets CI build
RX.

## 2. The families

| Family | Triple | Multilibs | newlib reentrancy |
| --- | --- | --- | --- |
| Cortex-M | `arm-none-eabi` | GCC's `rmprofile` list, which holds every Cortex-M the chips name: M0 and M0+ and M3 soft-float, M4F and M7 softfp, M33 softfp and hard; each full and nano | static |
| Cortex-A53 | `aarch64-none-elf` | one | dynamic |
| RISC-V | `riscv64-none-elf` | rv32imac/ilp32 and rv64imac/lp64, both medany | static rv32, dynamic rv64 |
| RXv3 | `rx-elf` | `-misa=v3 -mdfpu` | static |
| ESP32 | `xtensa-esp32-elf` | one | dynamic |
| x86_64 | `x86_64-elf` | one, with libgcc built for the kernel too | dynamic |

The reentrancy column is today's `conan/newlib` contract, kept. One RISC-V compiler serves both
widths, as RISCstar's does now.

**Reentrancy is decided per multilib in one newlib build.** newlib's multilib build compiles every
multilib against one set of headers and one set of target flags, and today's recipe makes
reentrancy dynamic by appending `__DYNAMIC_REENT__` to that shared `sys/config.h`, which would
make rv32 dynamic too. The package puts the condition in the header instead: `sys/config.h`
defines `__DYNAMIC_REENT__`, and newlib's own `GETREENT_PROVIDED` for its build, where the
multilib being compiled is one the family marks dynamic, `__riscv_xlen == 64` on RISC-V. So one
build yields a static rv32 libc and a dynamic rv64 one, and every compile against the installed
header sees its multilib's layout. The two families with a single multilib, AArch64 and Xtensa,
take the unconditional form. The configure checks each board's library for the reentrancy its
toolchain file requires, by the libc's `__getreent` references and a header probe, as the Arm
family's are checked today.

A tree gate checks that every chip's CPU flags resolve to a
multilib the family's compiler carries, so a board with new flags fails its configure rather than
linking the default library.

**x86_64's binutils also writes PE32+.** It is configured with the `x86_64-pep` target beside
ELF, so `ld -m i386pep` produces the UEFI images on any host, where today it takes the Linux
host's binutils. The kernel is built `-mno-red-zone` and without vectors, so the package carries a
libgcc built the same way for it.

**Full C/C++ on x86_64 needs vector state in user threads.** The x86-64 ABI passes floating point
in SSE registers, so newlib and libstdc++ cannot be built without them, and this port saves no
vector state. A part of M10.2 gives user threads SSE and AVX and saves their state per switch with
XSAVE, which x86-64-v3 guarantees. The kernel stays built without vectors.

## 3. The recipe

`conan/toolchain` is one Conan recipe with the family as an option. Conan keys a package on the
host's operating system and architecture and on the option, so one command builds or restores the
compiler this host runs for that family, and a second host gets its own. The build order is the
classic one: binutils; a first GCC with C only; newlib, full and, on Cortex-M, nano; the final GCC
with libstdc++ over that newlib, nano libstdc++ beside it on Cortex-M. GMP, MPFR, MPC and ISL are
built in GCC's tree from their pinned archives, so the host needs no development packages of its
own. Documentation is not built.

**The host is not locked.** Linux on any architecture and macOS build it natively with the host's
C and C++ compiler, GNU make and a POSIX shell; Windows builds it under MSYS2, which Conan drives
as its bash subsystem. CI publishes prebuilt packages for Linux x86_64 and macOS arm64, so a
common machine restores what another builds once, in the time a GCC build takes.

**One variable names it.** A consumer recipe installs the families a tree needs and writes one
file indexing where each family's compiler lives; the toolchain files read one variable naming the
folder that holds the index, in place of today's five toolchain variables and eleven newlib ones.
newlib is in the compiler's own sysroot, so `cmake/cross_newlib.cmake`'s header swap and link
trace go, and the reentrancy check becomes a stamp the package carries and configure reads.

The names this part proposes:

```text
conan/toolchain            the recipe; option family, one of the six triples above
conan/toolchains           the consumer: installs the families a tree needs, writes the index
KICKOS_TOOLCHAIN           the folder holding the index, read by every cmake/toolchain-*.cmake
kickos-toolchain.cmake     the index: one bin directory per family, and the package's stamp
```

## 4. What it deletes

The vendor downloads in `.github/actions/*-toolchain`, the hint URLs in `cmake/toolchain-*.cmake`,
`conan/newlib` and `conan/board`, `cmake/cross_newlib.cmake` down to the stamp check, the per-vendor
configure mirroring and its checks, and every `KICKOS_NEWLIB_*` variable. `kickos_cxx` exists on
every board, the nano profile included once the `cxxtest` gate passes on it: Arm's own nano C++
archives were built without the unwind path, which ours need not be.

## 5. The parts

| Part | Lands |
| --- | --- |
| M10.2.1 | this design |
| M10.2.2 | the recipe, the consumer and the one variable, and the Cortex-M family: full and nano, every Arm board on it, CI's Arm jobs on it |
| M10.2.3 | AArch64 and RISC-V |
| M10.2.4 | x86_64: the compiler with PE32+ binutils and the kernel's libgcc, then newlib, libstdc++ and user vector state |
| M10.2.5 | ESP32 and RX |
| M10.2.6 | the deletions and CI's prebuilt packages for Linux x86_64 and macOS arm64 |
| M10.2.7 | the exit: every CI preset and the fleet's silicon on the package, the red zones and size budgets re-measured |

**Clean-room.** Vendors' and Espressif's build configurations are read and cited by path, never
copied; the recipe's configure lines are this project's own.
