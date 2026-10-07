<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M10.2 -- the KickOS toolchain

> **Status: ACTIVE.** Sections 1 to 4 are the toolchain the M10.2.1 branch builds. Section 5 is
> the design of full C and C++ on q35, the rest of M10.2.4: the maintainer has accepted every
> decision. The three stages of decision 16 are built, full C++ included, once a KickOS patch to
> the `x86_64-elf` binutils gave the absolute words that name a weak definition the base
> relocation `ld -m i386pep` withheld (5.5, decision 21). The acceptance limit on the switch
> cost is ruled, at most 700 core cycles at p50 under KVM (decision 15).
> `roadmap.md` assigns the milestone and carries the ruling; this page says how.

**KickOS builds its own toolchain, one per target family, from pinned sources, on the machine
that uses it.** Binutils, GCC, newlib and libstdc++ are built together, so each libstdc++ is
compiled against the newlib it links with, and C and full C++ work on every target. Today the
compilers come from five vendors, the newlib from `conan/newlib`, and the two are reconciled by
checks: the newlib release follows whatever the toolchain bundles, `cmake/cross_newlib.cmake`
swaps the toolchain's libc headers for the package's, and a desk on another vendor release builds
different bits from CI. M10.2 removes the vendors from the build.

## 1. The pinned sources

One release set for every family:

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

**RX is the pinned set plus Renesas's changes ported onto it, carried as patches in this
repository and mirrored by the release** (maintainer, 2026-10-01). Upstream GCC knows the RXv1
cores only; the RXv3 instructions and the double-precision FPU the RX72M has, and that
`arch/rx/rxv3/switch.S` saves under `__RX_DFPU_INSNS__`, are Renesas's GNURX 14.2 changes, as is
the `-misa` and `-dfpu` its GCC passes the assembler, which upstream's gas does not take. Each is
ported onto GCC 16.2.0, binutils 2.47 and newlib 4.5.0 as one patch per component, which the
recipe applies after unpacking, for this family alone. Two GNURX changes are left out by
maintainer decision: the `-mrxpeephole` pass and a loop-exit relaxation. The patches are
Renesas's GPL code, vendored in `conan/toolchain/patches/` beside the x86_64 one (5.5), which
`tests/lib/gate.sh` keeps out of the tree gates, and byte for byte the files the release mirrors.

**One GNURX pattern is corrected by a KickOS patch over the port** (maintainer, 2026-10-01). Its
double-precision compare branches on ORDERED for UNORDERED, and its UN* codes take the branch on
every ordered pair, so under `-mdfpu` `isnan` reads 1.0 as a NaN and printf prints `nan` for it,
while `<` and `==` are right. `kickos-rx-dfpu-compare.patch`, applied after the multilib patch,
turns those branches round. `rx_dfpu_compare` steps the compiler's compares on the host, and
`fpclass` checks classification and printf of doubles on the board, which the RX72M passes
enforcing and flat (M10.2 exit (archived `M10.2_exit.md`), Silicon). A subnormal double reads as
zero there, which is the DFPU's and not the compiler's: `DPSW.DDN`, set from reset and in every
thread, handles a denormal operand as 0, and `fpclass` reads the bit before it expects either.
The bit stays set (maintainer, 2026-10-01): clearing it trades the flush for an
unimplemented-processing exception on every denormal operand, which KickOS would have to emulate.

**Every pinned source and every prebuilt package live in one GitHub release per toolchain
version** on this repository (maintainer, 2026-09-30). The recipe fetches each source from its
upstream first and the release second, and every patch from the copy it exports from this
repository. Distributing GCC binaries obliges us to make their exact sources available, which the
sources beside them do. A script assembles the release's files, the source archives, the patches,
one package archive per family and host and their sha256 list, for the maintainer to upload.

## 2. The families

| Family | Triple | Multilibs | newlib reentrancy |
| --- | --- | --- | --- |
| Cortex-M | `arm-none-eabi` | GCC's `rmprofile` list, which holds every Cortex-M the chips name: M0 and M0+ and M3 soft-float, M4F and M7 softfp, M33 softfp and hard; each full and nano | static |
| Cortex-A53 | `aarch64-none-elf` | one | dynamic |
| RISC-V | `riscv64-none-elf` | rv32imac/ilp32 and rv64imac/lp64, both medany | static rv32, dynamic rv64 |
| RXv3 | `rx-elf` | the default, `rxv3`, `64-bit-double/rxv3` and `64-bit-double/dfpu/rxv3`, which `-misa=v3 -mdfpu` selects | static |
| ESP32 | `xtensa-esp32-elf` | one | dynamic |
| x86_64 | `x86_64-elf` | one | dynamic |

The reentrancy column is today's `conan/newlib` contract, kept. One RISC-V compiler serves both
widths, as RISCstar's does now.

**Reentrancy is decided per multilib in one newlib build.** newlib's multilib build compiles every
multilib against one set of headers and one set of target flags, and today's recipe makes
reentrancy dynamic by appending `__DYNAMIC_REENT__` to that shared `sys/config.h`, which would
make rv32 dynamic too. The package puts the condition in the header instead: `sys/config.h`
defines `__DYNAMIC_REENT__` where the multilib being compiled is one the family marks dynamic,
`__riscv_xlen == 64` on RISC-V. So one build yields a static rv32 libc and a dynamic rv64 one, and
every compile against the installed header sees its multilib's layout. newlib's own
`GETREENT_PROVIDED` goes on its command line for the whole family, `getreent.c` testing it before
it includes anything: it leaves libc no fallback `__getreent`, which a static multilib never
calls. The two families with a single multilib, AArch64 and Xtensa,
take the unconditional form. The configure checks each board's library for the reentrancy its
toolchain file requires, by the libc's `__getreent` references and a header probe, as the Arm
family's are checked today.

A tree gate checks that every chip's CPU flags resolve to a
multilib the family's compiler carries, so a board with new flags fails its configure rather than
linking the default library.

**x86_64's binutils also writes PE32+.** It is configured with the `x86_64-pep` target beside
ELF, so `ld -m i386pep` produces the UEFI images on any host, where it took the Linux host's
binutils, and a KickOS patch gives an absolute word naming a weak definition its base
relocation (5.5). The kernel half calls no library (5.4); the app half links the package's
libraries, built `-fpie` and `-mno-red-zone`.

**Two defaults the Linux host compiler supplied are stated instead.** An `x86_64-elf` GCC puts
constructors in `.ctors`, so it is configured with `--enable-initfini-array`, the table the root
task runs. And it reaches external data under `-fpie` through a global offset table, which the
PE32+ image cannot hold. A KickOS patch to its `ix86_binds_local_p` binds every symbol but a weak
one locally under `-fpie` (5.5), so the libraries and every image reach data, typeinfo and
function addresses PC-relative, and `tools/check-x86_64-no-got.sh` refuses a survivor before
every link. The visibility header KickOS forced into every x86 C and C++ compile until then is
gone: built without it, every member of every installed archive passes that guard, which the
package's own build runs over each of them, the family being configured with `--disable-gcov`
so that libgcc's `-fpic` coverage runtime is not among them (5.5).

**Full C/C++ on x86_64 needs vector state in user threads.** The x86-64 ABI passes floating point
in SSE registers, so newlib and libstdc++ cannot be built without them, and this port saves no
vector state. A part of M10.2 gives user threads SSE and AVX and saves their state per switch with
XSAVE, which x86-64-v3 guarantees. The kernel stays built without vectors. Section 5 is that
design.

**ESP32's core lies where upstream newlib reads it.** Espressif's overlay writes the core's
`core-isa.h` where their newlib fork keeps it, `libc/sys/xtensa/include`; upstream reads it from
`libc/machine/xtensa/include`, so the recipe lays it there. newlib 4.5.0's generated
`Makefile.in` copies that directory one level deep, short of `xtensa/config`, where its
`Makefile.am` recurses, so newlib's own compile is given the directory. The stdio buffer stays at
the 128 bytes Espressif's newlib gives it, `__BUFSIZ__` in `sys/config.h`; the POSIX thread
declarations the old recipe added for Espressif's libstdc++ go, as this libstdc++ is built without
threads like every family's.

**RX has four sets of libraries.** GNURX's GCC defines 104 multilibs; a fifth patch, KickOS's
own, keeps the default, `rxv3`, `64-bit-double/rxv3` and `64-bit-double/dfpu/rxv3`. The RX72M's
`-misa=v3 -mdfpu`, with the 64-bit doubles `-mdfpu` implies, selects the last.

**The RX switch banks the DPFPU file at an alignment the compiler chose for it.** On GCC 16.2 the
RX72M's switch p50 read 160 cycles against GNURX 14.2's 128 (M10.2 exit (archived `M10.2_exit.md`)).
The switcher banks DR0-DR15 224 bytes below the USP it is taken on, the operand bus and the RAM
are 64 bits wide, and a bank at 4 mod 8 moves every double in two beats, 13 cycles a half. Nothing
in `switch.S` chose that residue: the caller's frame sizes did, and GCC 16.2 saving one register
fewer in `syscall_body` put the ping-pong at 4 mod 8. The trail:
- M10.2.3 aligns R0 down to 8 across the `mvtc` in `_arch_irq_restore`, which is where a switch
  pended under the lock is taken, so that path banks aligned whatever the frames above add up to.
- The other ways into the bank, preemption, the register-form call's fastpath and the first-run
  restore, were counted on silicon before anything else changed, per bench row and selftest list
  (M10.2.3 with the pended-switch fix, GCC 16.2, rx72m, a scratch counter build; entries / entries
  at 4 mod 8). In the bench only first-run restores are misaligned; in the selftest, preemptions
  under the uartirq list and the fastpath's refusals.

  | Path | Bench | Selftest, default | Selftest, uartirq |
  | --- | --- | --- | --- |
  | Switch pended under the lock, the realigned one | 666868 / 0 | 1363 / 0 | 27894 / 0 |
  | Preemption of user mode | 0 | 2 / 1 | 99 / 61 |
  | Preemption of kernel mode | 0 | 171 / 1 | 14279 / 221 |
  | Register-form call fastpath, taken (save, and the restore after it) | 60000 / 0 | 2 / 0 | 2 / 0 |
  | Register-form call fastpath, refused | 0 | 283 / 261 | 283 / 261 |
  | Restore after a pended switch | 666868 / 434 | 1536 / 224 | 42272 / 506 |
  | Other restore, `arch_start` | 1 / 1 | 1 / 1 | 1 / 1 |
- Ruled (maintainer, 2026-10-02): those paths stay, since realigning them costs every entry to
  save a few thousand cycles a run, and the fastpath's taken entries, aligned today only because
  the callers' frames add up that way, are watched. The RX bench counts each taken entry whose
  saved or restored bank is at 4 mod 8 (`KICKOS_BENCH_FASTPATH_WATCH`, `kickos/bench.h`), prints
  `fastpath-misaligned: N of M` once a boot, and `tools/bench/bench-capture.sh` refuses an rxv3
  capture whose N is not zero: the regression it exists for is a compiler change of frame size
  flipping that parity, about 0.4 percent of a call/reply, which no throughput row would refuse.

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
archives were built without the unwind path, which ours need not be. It passed every check on
`qemu-m3` built nano. `microbit`, the one board nano by default, stays off the `cxxtest` list:
its 1 KiB heap, sized under the first buffered stdout write, cannot hold the test's
allocations, though its exceptions unwind there.

## 5. Full C and C++ on q35 (M10.2.4)

Today q35 refuses every vector and x87 instruction: `kickos_x86_64_fp_trap` in
`arch/x86/x86_64/entry_x86_64.cc` sets CR0.EM, TS and MP and clears CR4.OSFXSR, OSXMMEXCPT and
OSXSAVE on every core, the whole tree is compiled `-mno-sse -mno-mmx -mno-80387`, and
`KICKOS_HAVE_LIBC` is 0. The x86-64 psABI passes floating point in SSE registers, and the
package's newlib and libstdc++ are SSE code: its `libc.a` alone decodes 2127 instructions naming
an XMM or YMM register. This section gives every thread x87, SSE and AVX, saves that state at
every switch, keeps the kernel half free of it, and ports newlib to q35. 5.1 to 5.5 are built,
with the arms and gates of 5.6 that test them: newlib, the C apps and `cxxtest`'s full C++ run on
every q35 preset. 5.7 records the maintainer's rulings and what stays open, and 5.8 the
measurements behind 5.5.

### 5.1 The state, enabled per core

The trap is replaced by an enable, called where the trap is today: on the boot core after
ExitBootServices and on each core from `arch/x86/chip/q35/chip_q35.cc` before it runs a thread.
CR0 takes EM 0, TS 0, MP 1 and NE 1; CR4 takes OSFXSR, OSXMMEXCPT and OSXSAVE; XSETBV writes
XCR0 = 7, x87, SSE and AVX and nothing else, even on a processor with AVX-512. CPUID leaf 0xD
subleaf 0 then must report those three components supported and, in EBX, a save area of 832 bytes
for the XCR0 just written; anything else is a boot panic before the first thread.
`arch/x86/x86_64/floor_x86_64.cc` already refuses a processor without XSAVE or AVX, and
`KICKOS_X86_64_QEMU_CPU` carries both. It carries XSAVEOPT as well, which is no part of
x86-64-v3 and which the port never executes: QEMU 11.1's TCG spins on the CR4 write that sets
OSXSAVE for a model reporting XSAVE without it, measured, so no image reached its first thread.

TS stays clear for good: the save is eager (5.2), so no #NM handler exists. NE makes an x87 error
the #MF exception rather than the legacy external line. An #MF, an #XM from an unmasked SIMD
exception, or an #UD from an AVX-512 instruction at ring 3 is a vector below 32 with a ring 3 code
selector, which `arch_fault_is_user_thread` in `arch/x86/x86_64/arch_x86_64.cc` already contains
as a user fault. The kernel half executes none of these instructions, so the same vectors at
ring 0 remain the panic they are.

### 5.2 One save area per thread, on both switch paths

**The area** is XSAVE's standard format for XCR0 = 7: the 512-byte legacy region holding the x87
control and status words, MXCSR at byte 24, the eight x87 and sixteen XMM registers; the 64-byte
XSAVE header at 512; the upper halves of the sixteen YMM registers at 576, 256 bytes. 832 bytes,
which the instruction requires 64-byte aligned. It goes into `struct arch_context`
(`arch/x86/x86_64/include/kickos/arch/context.h`) after the existing members, aligned to 64, with
the header reordered so `trace_tid` sits beside `port_count` and the members before the area fit
64 bytes in every posture, the FS base of 5.5 included: the area lands at offset 64 and the
context is 896 bytes, as built. `sp` stays at 0, where `KOS_CTX_SP` reads it. The build pins the
area exactly, by static assertion beside `KOS_CTX_SP` in `arch_x86_64.cc`: its 64-byte alignment,
its 832-byte size, the context's 896 and the assembly's `KOS_CTX_*` offsets. `sizeof(Thread)` is
not pinned exactly on x86 (5.3): the Thread size pin keeps a 2 KiB ceiling there, and the size
the first build measured is recorded there.

**A new thread starts in the initial state.** `arch_context_init` zeroes the area and writes MXCSR
0x1F80. A header whose component bitmap is zero makes XRSTOR load the initial state of all three
components, x87 control word 0x37F and every register zero, but XRSTOR reads MXCSR from the area
whenever it restores SSE or AVX, so the seeded word is what masks every SIMD exception. Zeroing at
every create is what keeps a slot's previous occupant, possibly another task's thread, from
leaking its registers into the next one. `arch_ctx_redirect` rebuilds through `arch_context_init`,
so a death stub starts clean as well.

**`kickos_x86_64_switch_now`** (`arch/x86/x86_64/switch.S`) saves the outgoing state into the
outgoing area once the general-register frame is built, rax and rdx being in that frame already,
with XSAVE64 and edx:eax = 0:7. It restores the incoming area with XRSTOR64 directly after the
stack swap, before the bench close and `kickos_switch_unlock`. The unlock is what lets another
core take the outgoing thread, so its area is complete before then; the incoming area was
completed by whichever core parked that thread before its own unlock. Both instructions lie
inside the bench's switch window.

**The first thread of every core is entered on one path, and no switch is on it.** The boot core
reaches it from `kmain`'s `sched::start()`. Each AP reaches it once `ap_main`
(`arch/x86/chip/q35/chip_q35.cc`) has run `desc_init_secondary` and parked: the doorbell park
calls `kickos_kernel_core_start`, which calls `sched::start()` too. That function seats the first
thread without `switch_book`, so without advancing its `switch_count`, and ends in `arch_start`,
which calls `publish_current(first)` and then `kickos_x86_64_start(first)`. `publish_current`
writes the FS base (5.5). `kickos_x86_64_start` restores the first thread's area with XRSTOR64
after it loads `rsp` and before its `jmp kickos_x86_64_resume`, whose `iretq` enters ring 3. Both
precede the first thread's first instruction, on the boot core and on every AP. Which core's first
thread is unprivileged is not fixed on SMP: `kmain` releases the peers after root is added, and
whichever core picks root first enters it here.

**The interrupt-exit switch** in `kickos_x86_64_isr` (`arch/x86/x86_64/arch_x86_64.cc`), where it
stores the interrupted frame into the current context and returns the incoming context's `sp`,
saves into the current area, when there is a current context, and restores the incoming one, in
inline assembly. Its unlock runs in `arch/x86/x86_64/trap_x86_64.S` after that function returns,
so the ordering is the same, and nothing between the restore and the `iretq` touches vector state.

**An interrupt or a syscall that does not switch saves nothing.** The kernel half never touches
these registers, so a trapped thread's state stays in them across the trap. The two switch paths
are therefore the whole of the mechanism, and the per-switch save and restore the whole of its
time cost.

**Eager, not lazy.** A lazy port sets TS at each switch, takes #NM at a thread's first vector
instruction, and tracks per core whose state the registers hold. Above one core that owner's
state must reach memory before the thread runs elsewhere, by an interprocessor interrupt or a save
at switch-out, which is the eager cost again. And a stale owner's registers are readable under
speculation, the LazyFP disclosure (CVE-2018-3665), for which Linux dropped lazy switching. Eager
is two instructions on each path with no handler, no owner and no cross-core flush. It costs the
save and restore even for a thread that never touched vectors, which with libc linked is rarely
the case. Plain XSAVE, not XSAVEOPT: XSAVEOPT, which skips components left unmodified, is not part
of x86-64-v3, and the compacted XSAVEC and XSAVES change the layout. Privileged threads are saved
like every other; they never touch the state, and skipping them is an optimisation a measurement
can ask for later.

### 5.3 The costs

| Figure | Before | With the area, measured |
| --- | --- | --- |
| `sizeof(Thread)`, qemu-x86_64-smp12 | 496 | 1344 |
| `sizeof(Thread)`, qemu-x86_64 | 488 | 1344 |
| `sizeof(struct arch_context)` | 56 | 896 |
| `alignof(Thread)` | 8 | 64 |

The Thread figures were arithmetic, 840 more for the context, 8 for `Thread::reent` under
`KICKOS_LIBC_REENT` and the rest the rounding to 64, and the builds measured them as the
arithmetic gave, before `KICKOS_LIBC_REENT` is 1 on q35 and after, the reent pointer landing in
padding and the FS base in the context's first 64 bytes (2026-10-01, the compiler's own `sizeof`
and `alignof` under each preset's flags). The size pin in
`kernel/include/kickos/thread.h` is one assertion of exact size for every arch. It stays exact
on the other arches, whose memory is counted in KiB; on x86 it becomes a ceiling,
`sizeof(Thread)` at most 2 KiB, which catches an accidental large increase, and the measured size
is recorded after the build. At the 32 threads of qemu-x86_64-smp12's `KICKOS_MAX_THREADS` the
TCBs take about 27 KiB more at the estimate, and even at 2 KiB a Thread the slots would take about
64 KiB. Each core's boot context in `kernel/include/kickos/instance.h` carries an area too, though
`arch_start` abandons it on x86: about 10 KiB on twelve cores. Those areas stay in place this stage
(maintainer, 2026-10-01): dropping them would need a boot context of a different representation
from a thread's. A q35 machine has MiB to GiB of RAM, so its memory is not pinned to the byte as a
small target's is: "x86 is a big platform with MiB or GiB of RAM and hundreds of GiB of disk"
(maintainer, 2026-10-01). The larger context's time cost is read from the switch benchmarks.

**No kernel stack figure moves by construction.** The area lives in the context, never in a
frame; `KICKOS_X86_64_TRAP_FRAME` stays 176, the interrupt-exit save is inline assembly that
builds no frame, and `KICKOS_MIN_STACK_SIZE` does not move for it (2624 at one core and 3328
above, `Kconfig`). The trap red-zone gate measures
every x86 class again all the same, and
`arch/x86/x86_64/include/kickos/arch/x86_64_trap_stack.h` changes only where a measurement passes
its figure. Newlib's port did pass one, measured: with `KICKOS_LIBC_REENT` at 1 a timer's wake
primes the incoming thread's libc state inside `pick_and_seat`, through `reent_prime` and
`arch_aspace_acquire`, and IRQ, IRQK and IST measure 544 on qemu-x86_64 against 512. The
one-core figures take 576, the interrupt's whole extent 760, and the one-core
`KICKOS_MIN_STACK_SIZE` 2624 to cover a privileged syscall with that interrupt below it; the
SMP presets fit their figures already. User stacks move: the 16-byte cell of 5.5 comes off the
top of each, and app code built with vectors, `printf` of a `double` above all, runs deeper. The
stackdepth app re-measures; the 64 KiB default thread stack is far above any of it.

Per switch: one XSAVE and one XRSTOR of up to 832 bytes, and one WRMSR (5.5). Image size grows
by the libraries an app pulls, and each address space gets its own copy of the heap (5.5).

**The switch row measures the whole switch.** Its window opens today at
`kickos_x86_64_switch_now`'s entry (`arch/x86/x86_64/switch.S`), after `arch_switch` has called
`publish_current`, and closes once the stack is swapped, before `kickos_switch_unlock`; the WRMSR
of 5.5 would fall outside it. The open moves ahead of `publish_current`: as built, `arch_switch`
calls `kickos_x86_64_bench_open`, a body of its own in `switch.S` under `KICKOS_BENCH` holding the
same `lfence`, `rdtsc` and store to the per-core block's `sw_start`. The close stays where it
is, so the window encloses `publish_current` whole, the task-state segment, the I/O ports and the
per-core kernel stack as well as the FS base, then the register save, the XSAVE, the stack swap
and the XRSTOR. The wider window lands first and the row is taken with it
before anything of 5.1 or 5.2, so before and after read the same window. The row gains a variant
with dirty vector state: its threads write the YMM registers before every switch, so each XRSTOR
loads the SSE and AVX components rather than initialising them from a clear header bitmap. The
interrupt-exit switch has no window on x86 today and gains none. The rows are taken on
qemu-x86_64-bench before and after, and TCG's cost is no silicon's: the acceptance limit is set
on the rows under KVM below, this host's silicon, in the core cycles they are read in (5.7,
decision 15).

The rows, measured 2026-10-01 in one session of ten interleaved runs of each image, QEMU 11.1.1
pinned to two fast host cores: "before" is the widened window with no vector state, "after"
the area of 5.2 on both paths. Each run prints three `switch:` rows and, after, one
`switch-vec:` row whose players write all sixteen YMM registers before every switch; the
figures are the medians over those rows of each row's own p50 and p99.

| Row | Rows | p50 | p99 |
| --- | --- | --- | --- |
| `switch:` before | 30 | 288 cyc, 143 ns | 416 cyc, 207 ns |
| `switch:` after | 30 | 960 cyc, 479 ns | 1280 cyc, 640 ns |
| `switch-vec:` after | 10 | 960 cyc, 480 ns | 1280 cyc, 640 ns |

TCG executes XSAVE and XRSTOR as helpers over the whole area whatever the header says, so the
dirty variant reads as the clean one there; silicon is where the two can differ. A second session
of ten interleaved runs, after 5.5 was built, added its image, whose window also holds the FS base
WRMSR: `switch:` read 256, 960 and 960 cycles at p50 for before, after and with newlib's port,
and 384, 1280 and 1408 at p99; `switch-vec:` with the port 960 and 1344.

**The rows on silicon, under KVM.** Taken 2026-10-01 on this host's AMD Ryzen AI 9 HX PRO 370
(Zen 5, x86-64-v4, up to 5.16 GHz), QEMU 11.1.1 with `-accel kvm -cpu host` in place of the
preset's x86-64-v3 model, XCR0 staying 7 and the area 832 bytes, pinned with `taskset` to cores
2 and 3, fast threads of two different physical cores, in one session of ten interleaved runs of
each image. "Before" is the image of the commit "Open the q35 switch window before
publish_current", the widened window with no vector state; "after" is the final image,
the area of 5.2 on both paths, the FS base write of 5.5 and newlib linked. The cycles are TSC
ticks, which this host's TSC counts at 2 GHz under KVM, and the buckets of
`kernel/include/kickos/bench_hist.h` are 4 ticks wide at 40 and 32 wide at 256. One run of
"before" never reached the kernel, firmware finding no boot partition, and was taken again at
the session's end.

| Row | Rows | p50 | p99 |
| --- | --- | --- | --- |
| `switch:` before | 30 | 40 cyc, 20 ns | 40 cyc, 20 ns |
| `switch:` after | 30 | 256 cyc, 128 ns | 256 cyc, 128 ns |
| `switch-vec:` after | 10 | 256 cyc, 128 ns | 256 cyc, 128 ns |

The window grows by 216 ticks, 108 ns, about 550 cycles of the core's clock. The per-row p50s of
"after" spread from 240 to 288 and its p99s from 256 to 480; "before" from 20 to 40 and 40 to 80.
The same runs' ping-pong rows, the whole round trip per switch, read medians of 546 ns before and
678 ns after, 132 ns more, of which the window's growth is 108. As under TCG, the dirty
variant reads as the clean one: plain XSAVE writes every enabled component whatever the
registers hold, and this processor restores a dirty area within the same 32-tick bucket as a
clear one.

**The window's parts, under KVM.** Measured 2026-10-01 on the same host, QEMU, cores and
`-cpu host`, once the maintainer had ruled that the window be split before any limit is set
(decision 15). Scratch builds of qemu-x86_64-bench at the commit "Record the q35 switch rows
under KVM", none committed: eight keep the window as built and drop one of its three parts, the
XSAVE, the XRSTOR64 or the FS base WRMSR on `arch_switch`'s path, or all three, or put an
alternative in a part's place; seven move the window onto one part alone, an `lfence` and an
`rdtsc` on each side, one of them an empty bracket as control. "Before" is rebuilt at its commit
with the same instrumentation, which adds to the bench an exact count of every value below 1023
ticks and a clock probe. Each image ran ten times in one session, the order shuffled every round,
and every run again with the preset's own model, which KVM accepts as named.

Two things the rows above hide. The guest's TSC advances in steps of 20 ticks, 10 ns, so each
sample is a multiple of 20 and only a row's mean over its 40002 samples resolves below that. And
the cores' clock moved between 3.5 and 5.1 GHz in the session, other work loading the package,
while the window's cost in core cycles did not: each run times a dependent-add chain of known
length with the same TSC, and "after" read 641 cycles on its runs below 4.6 GHz and 641 on its
runs at 5 GHz, 298 ticks against 254. The cycles are therefore the unit here. The ticks are
those of the runs at 2.53 cycles per tick, 5.05 GHz, and the ns half the ticks. A part dropped
is read as the difference of the window's means with and without it; a part bracketed as its
bracket's mean less the empty bracket's, 24 ticks, 61 cycles, at p50 and p99 20 and 40.

| Part | Window without it, p50 / p99 | Dropped: ticks, ns, cycles | Bracket p50 / p99 | Bracketed: ticks, cycles |
| --- | --- | --- | --- | --- |
| XSAVE of the outgoing 832 bytes | 200 / 200 | 56, 28 ns, 143 | 60 / 80 | 45, 114 |
| XRSTOR64 of the incoming area | 120 / 120 | 141, 70 ns, 358 | 160 / 160 | 130, 329 |
| WRMSR of IA32_FS_BASE | 220 / 240 | 37, 18 ns, 93 | 40 / 60 | 17, 44 |
| everything else of stages 1 to 3 | 40 / 40, all three dropped | -1, 0 ns, 0 | | |
| the whole growth | 260 / 260 after, 40 / 40 before | 222, 111 ns, 566 | | |

The XRSTOR64 dominates, about 63% of the growth, the XSAVE about 25% and the WRMSR about 16%.
The window with all three dropped reads as "before" to the cycle, so the rest of the three
stages, the FS base's load from the context and the XCR0 operands among them, costs nothing
this measurement resolves. The three dropped one at a time sum to 594 cycles against the 566 of
all three, the parts overlapping by about 5%. The brackets read lower, 487 together: a bracket
sees a part's own latency and a dropped part also what it costs the instructions around it,
the WRMSR most of all, 44 cycles bracketed against 93 dropped. `switch-vec:` reads the same
within 10 cycles: XSAVE 134 dropped and 114 bracketed, XRSTOR64 351 and 325, the WRMSR 92 and 44.
With the preset's model every row reads within 3 cycles of `-cpu host`, "after" 641 and 636 for
the clean and dirty rows against 642 and 635, "before" 76 against 76; the model changes nothing.

The alternatives, each in its part's place on the voluntary path, the interrupt-exit path
keeping XSAVE. `-cpu host` reports FSGSBASE in CPUID leaf 7 EBX and XSAVEOPT and XSAVEC in leaf
0xD subleaf 1 EAX. The preset's model reports XSAVEOPT and neither of the others, under KVM and
under TCG. KVM adds both when they are named; QEMU 11.1.1's TCG adds FSGSBASE and refuses
XSAVEC with a warning. In the ping-pong each thread's area is the last its core restored, so
XSAVEOPT's modified optimisation applies. XSAVEC writes the compacted format, which for XCR0 7
is the standard layout, and XRSTOR64 takes either. WRFSBASE needs CR4.FSGSBASE, which also lets
ring 3 write its own FS and GS bases: decision 11 weighs that and keeps the WRMSR.

| Variant | Window, p50 / p99 clean | Window cycles, clean / dirty | Its part bracketed, cycles clean / dirty |
| --- | --- | --- | --- |
| as built: XSAVE, WRMSR | 260 / 260 | 642 / 635 | XSAVE 114 / 114, WRMSR 44 / 44 |
| XSAVEOPT for XSAVE | 240 / 270 | 618 / 630 | 91 / 104 |
| XSAVEC for XSAVE | 240 / 260 | 621 / 635 | 98 / 128 |
| WRFSBASE for WRMSR | 240 / 260 | 619 / 613 | 25 / 26 |

Each takes 20 to 24 cycles off the clean window, about 4%. XSAVEOPT and XSAVEC give most of that
back with dirty state, XSAVEC's bracket then costing more than XSAVE's; WRFSBASE keeps its 22
cycles either way. The preset's model, with the two features named, reads the same within 2
cycles.

**The limit, ruled (maintainer, 2026-10-01): the whole window, `switch:` and `switch-vec:`
alike, at most 700 core cycles at p50 under KVM**, measured with the cycles-per-tick calibration
above: each run times the dependent-add chain of fixed length with the same TSC and converts its
own window at its own ratio. As built the window is about 642 core cycles, against 76, 40 ticks
at p50, before stage 1; its p50 of 260 ticks at 2.53 cycles a tick is about 658. The limit is in
core cycles and not TSC ticks because the TSC counts at a fixed 2 GHz while the core's clock
moved between 3.5 and 5.1 GHz in one session: the same window read 254 ticks at 5.05 GHz and 298
below 4.6 GHz, 641 cycles both, so a limit in ticks would trip on the clock and not on the
window. No gate reads it. The committed bench prints TSC ticks and carries no calibration chain,
which the scratch builds of the split added, and the bench gates judge a report's structure,
not a row's value. It is checked by hand whenever the q35 switch rows are taken again, with that
instrumentation applied to the tree being measured.

### 5.4 The kernel half stays vector-free

**The flags split.** `cmake/toolchain-x86_64-uefi.cmake` puts `-mno-sse -mno-mmx -mno-80387` in
the global flags, so every target and every out-of-tree consumer is built without vectors. The
global posture becomes the app's: `-march=x86-64-v3` with x87, SSE and AVX. The kernel posture
adds `-mgeneral-regs-only` to exactly the targets `arch/x86/x86_64/pe_image.ld` claims for the
kernel half, `kickos_kernel`, `kickos_arch_x86_64`, `kickos_chip_q35` and the five x86_64 boot and
landing object libraries, and to the probe images' objects, which have no app half. That one GCC
flag refuses x87, MMX, SSE and AVX, and a floating-point type in a signature stays a compile error
there. `-mno-red-zone` and `-fpie` stay global; the forced visibility header that stood beside
them, `toolchain-x86_64-hidden.h`, is gone (5.5). `kickos_user`,
the system services and the apps take the app posture. As built, `cmake/x86_64_boot.cmake` reads
the kernel half's targets from the claims of `pe_image.ld` itself, so the flag and the claims
cannot drift, and the toolchain file strips the three `-mno-` flags from a build tree configured
before the split, CMake keeping a tree's first flags.

**The kernel calls no libc.** On a translating board `kickos_privatise_runtime` already renames
every runtime call the compiler emits in the kernel, arch and chip archives to the kernel's
private copy, and `tests/static/check_x86_64_app_split.py` refuses any kernel-half reference into
app text, which is where every libc member lands. A kernel `memcpy` resolving to newlib's, which
is vector code, fails the build.

**The no-vector gate is rescoped.** `tests/static/check_x86_64_no_vector.sh` reads every object
and every image whole, so it would refuse every app. Its corpus becomes the executable sections
of every image outside the app window, and the probe images whole: what `-mgeneral-regs-only`
cannot hold, assembly, toolchain members and linker-synthesised text, is all there. An
application image is one whose link wrote its map beside it (`tools/x86_64-link.sh`). Its
controls stay, and one joins them: the app half of the selftest image must decode at least one
vector instruction, or the split did not happen and the rescoped gate reads nothing new. The
vector-state arms of 5.6 are assembly in the app half, so the control counts only instructions
outside them, `selftest_vec_*` and `bench_vec_*`: 78 on qemu-x86_64 as built at the flag split,
and 142 once newlib links, with or without the visibility header. The
state instructions the port owns are admitted by site and counted: XSAVE64 and XRSTOR64 in the
two switch paths and the first-thread start, XSETBV and XGETBV in the enable. A missing site
refuses like an extra one. The gate's list names `xsave` and `xrstor` but not the spellings
objdump prints for the 64-bit forms, `xsave64`, `xrstor64`, `fxsave64`, `fxrstor64` and
`xsaveopt64`, nor `xsetbv`; the rescope adds them, as the gate today would pass all six.

### 5.5 newlib on q35

**The flags.** `CMakeLists.txt` stops excepting x86_64: `KICKOS_HAVE_LIBC` and
`KICKOS_LIBC_REENT` become 1 there, and `KICKOS_LINKER_WEAK_UNDEF` stays 0. `user/CMakeLists.txt`
gives x86_64 `user/src/newlib_stubs.cc`, `user/src/newlib_sbrk.cc` and `user/src/newlib_reent.cc`
as every other cross arch has, and the `exit` stub of a posture without a C library,
`nolibc_exit.cc`, goes. The stubs are the same syscalls on every arch. newlib builds x86_64-elf
with `MISSING_SYSCALL_NAMES`, its `configure.host` giving the target no syscall directory, so its
reentrant layer calls `close`, `read`, `write`, `sbrk` and the rest without the underscore; on
x86_64 the stubs also answer those names, as aliases of the same functions, and `_getentropy`
is built there as on RISC-V, the image keeping every `.eh_frame` it pulls. The app posture is
hosted, as every other target's apps are: `cmake/toolchain-x86_64-uefi.cmake` drops the global
`-ffreestanding`, under which libstdc++'s `<cstdlib>` declares no `strtol`, and KickOS's own
libraries keep it through `kickos_apply_freestanding`.

**The heap.** `pe_image.ld` already states `_kickos_heap_start` and `_kickos_heap_limit`, as one
address at the end of `.appbss`. It carves `KICKOS_USER_HEAP_SIZE` between them, as
`arch/arm64/chip/virt_arm64/virt_arm64.ld` does, with the same `KICKOS_HEAP_MIN` assert. The heap
is in the app window, so every address space gets its own copy, frames taken per task.

**Reentrancy is per thread, as on AArch64 and RV64.** The x86_64 newlib is built dynamic
(section 2), so libc calls `__getreent`; with `KICKOS_REENT_PER_THREAD` at 1, `newlib_reent.cc`
answers it with `__builtin_thread_pointer()`, which on x86-64 GCC is a load of `%fs:0`. So the
word at the FS base must be the running thread's `struct _reent` pointer. `arch_context_init`
reserves a 16-byte cell at the top of an unprivileged thread's own stack, the aligned top minus
16, and puts the return slot below it, so the entry still sees `rsp` 8 modulo 16.
`arch_context_seat_reent`, the hook RV64 implements and `kernel/thread/thread.cc` calls at create
after `arch_context_init`, writes the thread's `struct _reent` pointer into the cell's first word
through the kernel's view of the stack, as the return slot is written today, and sets
`arch_context::fs_base` to the cell's user address. A privileged context has no cell and keeps
`fs_base` at 0: the kernel calls no libc (5.4). `arch_ctx_redirect` keeps `fs_base` across its
rebuild, as RV64's keeps `reent_tp`. libc, `newlib_reent.cc` and the seam of
`kernel/include/kickos/reent.h` keep their form; that header's statement that x86_64 links no C
library goes. It is not `KICKOS_REENT_IN_TCB`: x86 selects no `ARCH_HAS_TLS`, its TLS is variant
II, data below the thread pointer, which the `KICKOS_TLS` carve does not build, and `thread_local`
on q35 is a later decision.

**`publish_current` writes the base.** It is the one function in
`arch/x86/x86_64/arch_x86_64.cc` that every switch-in already passes through to seat the
task-state segment and the per-core kernel stack: `arch_switch` calls it before
`kickos_x86_64_switch_now`, `kickos_x86_64_isr` when it takes `switch_to`, and `arch_start` before
`kickos_x86_64_start`, the first thread's path on every core (5.2). It writes `to->fs_base` to
IA32_FS_BASE, MSR 0xC0000100, unconditionally. Each caller runs at ring 0 with interrupts masked,
before the `iretq` that enters the thread, so the base is in place at the incoming thread's first
instruction on all three paths. The value is the context's own, fixed at create, so unlike the
XSAVE area it needs no ordering against `kickos_switch_unlock`. The write is never skipped against
a per-core copy of the last value: ring 3 can reload the FS selector, which reloads the base from
its descriptor, so a cached value would hand the next thread a stale base.

**The FS selector stays null.** `load_gdt` (`arch/x86/x86_64/desc_x86_64.cc`), which `desc_init`
runs on the boot core and `desc_init_secondary` on each AP, sets the FS selector to null, and
nothing in the kernel loads FS again. That load does not establish the base: in 64-bit mode a
null selector loaded into FS need not touch the hidden base, and AMD's APM states that it is left
unchanged, so after `load_gdt` the base is whatever firmware or an earlier stage left there. The
poison write that follows is what establishes a known base. Both precede every `publish_current`
on their core, so neither can undo the write. The null selector is also what keeps the write
across the return: an `iretq` to ring 3 loads the null selector into any data segment register
whose descriptor's DPL is below the new CPL, which can clear that base, and it leaves a null
selector as it is. A thread that loads its own data selector into FS reloads its base from that
descriptor, zero, and breaks only its own errno and stdio until its next switch-in writes the
base again: naming and not isolation, as `newlib_reent.cc` says of the bank already. CR4.FSGSBASE
stays clear, so ring 3 cannot write either base directly, and no user-chosen GS base meets the
kernel's `swapgs`.

**Each core seeds the base with a poison, and the first thread's is read back.** Directly after
`load_gdt`, each core's bring-up writes a poison to IA32_FS_BASE with WRMSR, a canonical address
in the kernel half that no cell has, so a ring 3 load through a base nobody wrote faults rather
than reading whatever the base held before. `arch_start` reads IA32_FS_BASE back after
`publish_current(first)` and panics before the `iretq` unless it equals `first->fs_base`. The
poison is what makes that check fail where the write is missing even for a privileged first
thread, whose base is 0, which an AP's idle is.

**Nothing else may address through FS.** GCC 16.2 compiles `__builtin_thread_pointer()` to
`mov %fs:0x0,%rax`, the load `newlib_reent.cc` relies on. It reaches a `thread_local` at an
offset below the thread pointer: a locally defined one takes an `R_X86_64_TPOFF32`. On q35 the
thread pointer is the cell's word, a `struct _reent` pointer, so an address formed from it lands
below the thread's own state, in another thread's slot of the bank, and a load made through `%fs:`
directly lands in the thread's stack below the cell. The package's five archives carry no TLS
relocation, measured. App code could, and the no-GOT guard passes `R_X86_64_TPOFF32`, which names
no GOT; it refuses only the initial-exec `R_X86_64_GOTTPOFF`, measured. On q35 the guard
therefore also refuses `TPOFF`, `DTPOFF`, `TLSGD` and `TLSLD` relocations until `thread_local`
there is decided.

**The PE link pulls archive members.** The image link (`tools/x86_64-link.sh` since M10.5) runs ld
itself, so no compiler driver adds libraries. Each leaf's group names `libc`, `libm` and `libgcc`,
and `libstdc++` and `libsupc++` for a full-C++ leaf, and the toolchain file's link rule carries
their directories, each found at configure through the compiler's `-print-file-name` under the
board's flags and fatal when missing. `-b elf64-x86-64` already makes `ld -m i386pep` extract ELF
archive members, and the group rescans, so newlib's `_sbrk_r` reaches `kickos_user`'s `_sbrk` and
libstdc++'s `operator new` reaches `malloc`. The fleet's `-Wl,-u,_exit` becomes ld's own `-u
_exit`. `tools/check-x86_64-no-got.sh` and the relocation-copy pass already read every group
member, so the package archives join their corpus.

**Exceptions.** `pe_image.ld` places `.eh_frame` in the kernel half, which ring 3's unwinder
cannot read; it moves into the app window's read-only data, bounded by `__eh_frame_start`, with
the zero word crtend would end it with. So do the LSDAs, which `-ffunction-sections` names
`.gcc_except_table.<function>` and which the script named only bare, so each landed in the
kernel half as an orphan section.
`user/src/root_entry.cc` registers the tables only under `KICKOS_LINKER_WEAK_UNDEF`. On q35 the
registration is a constructor of the highest priority in the `kickos_cxx_rt` object of
`system/cxx/vterminate.cc`, which only full-C++ images link, and `SORT(.init_array.*)` runs it
ahead of every other app constructor. The LSDAs' typeinfo pointers are absolute words in app data,
which `app_relocate` already converts.

**Full C++ takes a KickOS patch to binutils.** `app_relocate` converts the absolute words of the
app window that carry a DIR64 base relocation, and the pinned `ld -m i386pep` writes none for an
absolute word naming a weak definition. Its base-relocation pass, `generate_reloc` in binutils
2.47's `ld/pe-dll.c`, treats a symbol flagged weak at lines 1669 to 1692: where the symbol's hash
entry is undefined weak it consults the COFF weak-external auxiliary entry, and otherwise it gives
a record only where the entry is `bfd_link_hash_defined`, line 1691. A weak definition read from
an ELF input is `bfd_link_hash_defweak`, so its words get none. libstdc++ is full of such words:
the `type_info::__name` of every typeinfo its COMDAT groups define weak, `_ZTISt11logic_error`'s
among them, and every vtable and template static a C++ program emits weak. Linked by the pinned
ld, `cxxtest` carries 55 absolute words with no record, which the app-split gate refuses, and
the image faults at its first `catch`, `__do_catch` reading a name pointer still at the link
address. C, newlib included, carried no such word, measured over every C image.

The maintainer ruled the patch (2026-10-01, decision 21), against a link step adding the missing
records to both the image's `.reloc` and the copy the boot relocates from, and against
`-fno-weak`, which makes every template static an undefined reference and is not a libstdc++
build. `conan/toolchain/patches/kickos-x86_64-pe-defweak-reloc.patch`, for the `x86_64-elf`
family alone, lets that test at line 1691 accept `bfd_link_hash_defweak` as well; an undefined
weak symbol still gets no record, as before. Measured with the rebuilt package: a probe of three
absolute words, naming a strong definition, a weak definition and an undefined weak symbol, gets
a DIR64 record for the first alone from the pinned ld and for the first two from the patched
one; and the same `cxxtest` objects linked by each give 445 records and 55 refusals, then 500
records and none. `cxxtest` joins every q35 preset and passes at the 16 KiB heap (5.6). Its first
`throw` reaching its handler is also the witness of `vterminate.cc`'s registration: without
registered tables the unwinder finds no frame and `std::terminate` runs.

**The global offset table, for the libraries' own objects.** Under `-fpie` this GCC reaches every
symbol it cannot prove binds in the output through the GOT, and `ld -m i386pep` does not relax
that load: it resolves it to a load of the symbol's own bytes as a pointer, measured, which is why
`tools/check-x86_64-no-got.sh` exists. The package's archives carry 2919 such relocations,
`libc.a` 472, `libm.a` 71, `libstdc++.a` 2326 and `libsupc++.a` 50, `libgcc.a` none: 2674 reach
data, 237 take the address of a function, and 8 reach a weak symbol no archive defines. Each way
out was measured by rebuilding 38 members of those archives from the package build's own compile
lines (5.8):

| The 38 members built | GOT relocations | Members with one | No-GOT guard |
| --- | --- | --- | --- |
| as the package was: its GCC, the libraries' flags | 887 | 30 | fails |
| the hidden header forced | 758 | 22 | fails |
| the hidden header and `--disable-libstdcxx-visibility` | 77 | 16 | fails |
| a GCC whose configure is patched for copy relocations | 62 | 15 | fails |
| that GCC, the hidden header and `--disable-libstdcxx-visibility` | 15 | 8 | fails |
| the patched `ix86_binds_local_p` below, no header | 8, every one weak | 4 | fails |
| that GCC and the removals of the weak references below | 6, in `init.o` and `fini.o` | 2 | passes on the 36 kept |

**Copy relocations cannot be supplied from the recipe, and would not be enough.** GCC 16.2's
`gcc/configure.ac` assigns `gcc_cv_ld_pie_copyreloc=no` before its probe, which is no cached check,
and probes an external linker only for `i?86-*-linux*` and `x86_64-*-linux*`; an in-tree ld of 2.25
or later is the other way to `yes`. Configured with the variable preset, the `x86_64-elf` GCC still
reports `no` and defines `HAVE_LD_PIE_COPYRELOC 0`, measured. A one-line patch adding
`x86_64-*-elf*` to that case makes the probe pass against binutils 2.47, measured, but the macro
reaches code in two places of `gcc/config/i386/i386.cc`: `legitimate_pic_address_disp_p` takes a
PC-relative displacement to a symbol not known to be local only when the symbol is neither weak nor
a function, and `ix86_binds_local_p` passes it for uninitialised commons alone. Built so, the
compiler makes data and typeinfo PC-relative and leaves the address of a function on the GOT, and a
weak symbol: the 38 members keep 62 GOT relocations, `_strtol_r`, `_strtoul_r`, stdio's `__sread`
family and the destructors `throw` hands `__cxa_throw` among them. Debian's GCC 16.2.0, whose Linux
configure reached `yes`, gives the same code.

**`-mdirect-extern-access` is the default already and changes nothing here.** It is `Init(1)` in
16.2's `gcc/config/i386/i386.opt`; GCC documents it for code built without `-fpic`, and under
`-fpie` it only gates the copy-relocation path above. Given explicitly, or negated, it changes no
relocation of the probe.

**The hidden header in the libraries' build is not complete either.** The header,
`toolchain-x86_64-hidden.h`, was the `#pragma GCC visibility push(hidden)` KickOS then forced into
every x86 compile. Forced into `CFLAGS_FOR_TARGET` and `CXXFLAGS_FOR_TARGET`, it builds every member
tried and clears newlib and libm of every GOT load but the weak ones. libstdc++ escapes it three
ways. Namespace `std` carries `_GLIBCXX_VISIBILITY(default)`, which `--disable-libstdcxx-visibility`
turns off by writing `_GLIBCXX_HAVE_ATTRIBUTE_VISIBILITY 0` into the installed `c++config.h`.
`<new>`, `<typeinfo>`, `<cxxabi.h>` and four internal libsupc++ headers open a default-visibility
pragma region unconditionally, so `std::nothrow`, `std::bad_alloc`'s destructor and the terminate
handlers stay default whatever the build says. And the typeinfo objects of classes another member
defines are declarations the compiler makes itself, whose visibility GCC never infers for an
undefined symbol, so `typeinfo for std::runtime_error` stays on the GOT with the namespace
attribute gone. With copy relocations added, 15 survive, each a weak symbol or the address of a
function declared in such a region.

**`-fno-pic` is out.** It removes the GOT, but every address taken in code becomes an
`R_X86_64_32` or `R_X86_64_32S`, which `ld -m i386pep` turns into a HIGHLOW base relocation that
`app_relocate` refuses, "a relocation record is not DIR64", and which cannot hold the user alias at
all: linked at the selftest's rebased base of 1 TiB, the image stops at "relocation truncated to
fit", measured. `-mcmodel=large` would carry every address as a 64-bit immediate in text, each
call's included, one DIR64 record per site of the app's code.

*Proposed answer:* a KickOS patch to the `x86_64-elf` family's GCC, which the recipe applies as it
applies the RX family's: `ix86_binds_local_p` in `gcc/config/i386/i386.cc` answers true for every
declaration that is not weak, when compiling `-fpie` and not `-fpic`. Every image this compiler
builds is one static link with no dynamic symbol, so that is what binding locally means for it. A
weak declaration keeps the GOT, the one way PC-relative code can see the zero of an undefined weak
symbol. The patch is seven lines, comment included, and changes `i386.o` alone. Rebuilt with it at
the libraries' own flags and with no header, the 38 members keep 8 GOT relocations, each a weak
reference of the inventory below, and the probe's C++ translation unit, which names `std::cout`,
a typeinfo, a destructor's address and a template's static member, keeps none. App code is
covered by the same compiler, which the header could never do for `std`. The configure patch is
then unnecessary: the copy-relocation path is reached only for a symbol the patch has not made
local, which is a weak one, and it excludes weak symbols. The no-GOT guard keeps reading every
archive member and is what proves it. The header should then give no code the patched compiler
does not, and the maintainer kept it forced into every x86 compile until the complete rebuilt
archives, every member of every installed archive, passed the no-GOT guard without it
(2026-10-01). The 38 members supported the approach; they were a sample, not that gate, which
the package below passes. This changes the x86_64 family alone.

**As built**, recipe revision 82e92a9b and package 63fead08 of 2026-10-01, with every recipe
change below, the binutils patch above and `--disable-gcov`: 606 seconds for the family alone,
`make -j24` on 14 of this host's 24 threads. None of the eight archives the package installs,
`libc.a`, `libg.a`, `libm.a`, `libnosys.a`, `libstdc++.a`, `libstdc++exp.a`, `libsupc++.a` and
`libgcc.a`, 2129 members in all, carries a GOT, TLS or weak undefined relocation: both guards
pass over every member, where the unpatched package carried 472 GOT relocations in `libc.a`, 71
in `libm.a`, 2326 in `libstdc++.a` and 50 in `libsupc++.a`. The first build with the GCC patch,
revision d5db8ae7, also installed `libgcov.a`, the coverage runtime, whose 29 GOT and 3 TLS
relocations in four members came from libgcc's own `-fpic` compile of it, which the patch leaves
alone as it leaves every `-fpic` compile; no image linked it. The maintainer ruled it out of the
package (2026-10-01, decision 7): GCC's `--disable-gcov` builds neither `libgcov.a` nor the
`gcov` tools, so the guards' corpus stays every member of every installed archive. With that
gate passed, the header was removed from every x86 compile, along with its plumbing, and every
q35 preset rebuilt without it passes every guard and test it passed before, `cxxtest` added.

**The package runs both guards over every archive it installs.** The family's `guards` entry
names `tools/check-x86_64-no-got.sh` and `tools/check-x86_64-weak-undef.sh`, and once the build
has installed, the recipe runs each with the package's own `readelf` over every archive under
`x86_64-elf/lib` and `lib/gcc/x86_64-elf`, the eight above and any a later build adds; one refused
member fails the package. That is what holds "every member of every installed archive". The
image link (`tools/x86_64-link.sh`) runs the same guards over what an image links, its objects
and its link group's archives, which leaves out `libg.a`,
`libnosys.a` and `libstdc++exp.a`. The recipe's `export()` copies the two scripts from `tools/`
into its export folder rather than restating them, so each guard exists once, and they are part
of the recipe revision as the patches are: an edit to either rebuilds every family, and CI's
cache key hashes them beside `conan/toolchain`. As built, recipe revision d1c09053 and package
63fead08 of 2026-10-02, 4762 seconds for the family alone, `make -j24` on 8 of the host's 24
threads under `nice`: both guards pass over the eight archives, 2129 members, and `libc.a` and
`libg.a` sit in the one multilib directory, holding neither deleted member.

**The patches live in this repository, and the release mirrors them.** They are KickOS's own,
kept here for review beside Renesas's RX patches: `conan/toolchain/patches/` holds
`kickos-x86_64-binds-local.patch` against the pinned GCC 16.2.0 tree and
`kickos-x86_64-pe-defweak-reloc.patch` against binutils 2.47, each applied with `-p1` in its tree
as the recipe applies every patch (maintainer, 2026-10-01). Each applies cleanly to its pristine
tree, checked with `patch --dry-run -p1`; the GCC patch's body is the change the measurements of
5.8 were built with. Each changes a vendored component, so each carries that component's
licence, GPL-3.0-or-later, and standard three-line context; `tests/lib/gate.sh` lists their
directory as vendored, outside the tree gates that police KickOS's own files ("gcc patches and in
general vendored stuff should be removed from the gates", maintainer, 2026-10-01). Each has its
`conandata.yml` key with its sha256, and the x86_64 family's `patches` entry in `conanfile.py`
names the GCC one for `gcc` and the binutils one for `binutils`. The recipe exports
`conandata.yml` and every file under `patches/`, so the files are part of every family's recipe
revision and of CI's cache key, which hashes everything under `conan/toolchain` and the two
exported guards above: any edit to either file or to a patch gives the recipe a new revision for
every family, every family's package rebuilds, not x86_64's alone, and CI's key changes with it.
The recipe applies every patch from its own export, so none waits on an upload to the release
before the build matrix runs.

**A weak undefined reference links into wrong code, and is refused only at a high base.** PE32+
holds no undefined symbol, and `ld -m i386pep` resolves one to the link's address 0. At the default
`--image-base=0x400000` the link succeeds, measured with the package's binutils: a GOT load becomes
a load of the bytes at link address 0; a call becomes `call 0`; a `lea` yields the link-time zero,
which firmware's relocation and then the user alias move off zero, so `if (weak_fn)` reads true and
the call goes nowhere. Only an image linked at 2 GiB or above overflows the displacement, which is
the "relocation truncated to fit" the selftest's image rebased at 1 TiB reports. An absolute data
word, `.quad sym`, comes out zero with no base relocation and stays zero. So the line is held
before the link: the guard refuses a GOT load of a weak symbol in any member, and the package
holds no member that reaches a weak symbol PC-relative.

**A second guard reads every other form, because the first cannot.** The no-GOT guard sees GOT
relocations only, and the design relies on the absence of the rest: a weak undefined symbol
reached by a `call`, by a PC-relative `lea` or load, or by an absolute word links silently at the
default base, as above. A relocation-aware weak guard reads the same corpus as the no-GOT guard,
every member of every package archive, read when the package is built, and every object of
every q35 image, read at its link, and refuses any relocation in an allocated section against a
symbol that is undefined and weak in its own object, whatever the relocation's type. One case is
admitted, by symbol, type and section:
`__cxa_pure_virtual` as an `R_X86_64_64` word in a vtable's `.data.rel.ro`, which links to zero
with no base relocation into a slot only a pure virtual call reaches (below). Its controls plant a
weak call, a weak `lea` and a weak data word naming another symbol, each of which must refuse, and
the admitted vtable word, which must pass. Today's q35 objects carry no weak symbol at all,
measured, so in the tree it has nothing to refuse until the package joins its corpus.

**The package's weak undefined references, and how each goes.** All of them, by member:

| Member | Weak symbols | Reached by | Source | Removed by |
| --- | --- | --- | --- | --- |
| `libc.a` `libc_a-__call_atexit.o` | `__libc_fini` | GOT | `register_fini`, `newlib/libc/stdlib/__call_atexit.c`, under `_WANT_REGISTER_FINI` | `--disable-newlib-register-fini` |
| `libc.a` `libc_a-init.o` | `__preinit_array_start`, `__preinit_array_end`, `__init_array_start`, `__init_array_end` | GOT | `__libc_init_array`, `newlib/libc/misc/init.c` | the member deleted |
| `libc.a` `libc_a-fini.o` | `__fini_array_start`, `__fini_array_end` | GOT | `__libc_fini_array`, `newlib/libc/misc/fini.c` | the member deleted |
| `libstdc++.a` `cow-stdexcept.o` | `_ITM_RU1`, `_ITM_RU8`, `_ITM_memcpyRtWn`, `_ITM_memcpyRnWt`, `_ITM_addUserCommitAction`, `_ZGTtnam`, `_ZGTtdlPv` | call | the Transactional Memory TS clones, `src/c++11/cow-stdexcept.cc`, under `_GLIBCXX_USE_WEAK_REF` | `-D_GLIBCXX_USE_WEAK_REF=0` |
| `libstdc++.a` `tzdb.o` | `__gnu_cxx::zoneinfo_dir_override()` | GOT and call | `zoneinfo_file`, `src/c++20/tzdb.cc`, built because the package embeds tzdata | `--with-libstdcxx-zoneinfo=no` |
| six `libstdc++.a` members and `libsupc++.a` `eh_exception.o` | `__cxa_pure_virtual` | data word | GCC's C++ front end declares it weak (`gcc/cp/decl.cc`), in every abstract class's vtable | nothing: no GOT, a zero slot |

`libgcc.a` and `libm.a` hold none. The weak references one would expect elsewhere are absent by
construction: libstdc++'s pthread references belong to `gthr-posix.h`, and this compiler's thread
model is `single`; `__cxa_thread_atexit_impl` is named only under
`_GLIBCXX_MAY_HAVE___CXA_THREAD_ATEXIT_IMPL`, which a newlib build does not define;
`__register_frame_info` belongs to crtstuff, which no q35 image links.

**No removal changes a run.** No KickOS linker script defines `__libc_fini`, so `register_fini`
registers nothing in any image today. `__libc_init_array` and `__libc_fini_array` are crt0's, which
KickOS does not link: the root task runs the app's own constructor range itself, the loop over
`__kickos_app_init_array_start` in `user/src/root_entry.cc`. On q35 the `__init_array_start` that
`__libc_init_array` would walk is moreover the kernel's constructor range in `.kosinit`
(`arch/x86/x86_64/pe_image.ld`), which it would run at ring 3, so `init.o` goes. `fini.o` goes with
it, but only once `register_fini` has gone: `register_fini` hands `__libc_fini_array` to `atexit`
(`newlib/libc/stdlib/__call_atexit.c` line 52), a strong reference every image calling `exit`
pulls, so deleting `fini.o` while `--enable-newlib-register-fini` stands breaks every such link.
Decision 20 therefore depends on decision 8's first removal.

**`libg.a` is deleted from as well.** In the archives the members are `libc_a-init.o` and
`libc_a-fini.o`, the names `ar t` lists in both archives of every measured `x86_64-elf` package.
The recipe deletes both with the family's `ar` after newlib installs, from `libc.a` and from
`libg.a`: newlib's install (`newlib/Makefile.am` lines 261 and 262) makes `libg.a` a hard link to
`libc.a` and falls back to a copy where the link fails, and in every measured package `libg.a` is
a file of its own. An edit of `libc.a` made in place would reach a hard link but never a copy, so
the recipe does not rely on either: it deletes from both archives by name, and the package's gate
fails unless both archives exist in every multilib directory the built compiler's
`-print-multi-lib` names, and if either name remains in either.

libstdc++ drops the transactional clones the same way for Cygwin (`config/os/newlib/os_defines.h`,
PR 69506), and no KickOS code is built `-fgnu-tm`, the only caller they serve.

**Without zoneinfo, `std::chrono` keeps a minimal time-zone database.**
`--with-libstdcxx-zoneinfo=no` defines neither `_GLIBCXX_ZONEINFO_DIR` nor
`_GLIBCXX_STATIC_TZDATA`, so `src/c++20/tzdb.cc` is built with `TZDB_DISABLED`. The first
`std::chrono::get_tzdb()` then gets libstdc++'s fallback database,
version "ersatz", which holds `Etc/UTC` and `Etc/GMT` and their aliases, `UTC`, `Zulu`, `GMT` and
`Greenwich` among them; every other zone is unavailable to `locate_zone`, and only `reload_tzdb()`
and `remote_version()` throw, "tzdb: support for loading tzdata is disabled". The embedded tzdata
leaves `tzdb.o`, which shrinks from 198971 to 56291 bytes, about 139 KiB: a reduction of that
member, which an image saves only where its time-zone code pulls `tzdb.o`. KickOS reads no time
zone.

`__cxa_pure_virtual` reaches no GOT and keeps the zero slot it has on every family, a call through
which is a contained user fault; an image could link `-u __cxa_pure_virtual` to name libsupc++'s
instead.

Rebuilt with the patched compiler and these four changes emulated, the 38 members keep GOT loads in
`init.o` and `fini.o` alone, which the recipe deletes, and the guard passes on the other 36. Its
scope stays what it is: every member of every package archive, read whole.

**Were one of them to stay, the guard's scope would narrow to what a link pulls, with a proof read
from the link.** The image link would pass `-Map`, whose "Archive member included to
satisfy reference by file (symbol)" lines name every member the link took, and the guard would run
over exactly those members, extracted from their archives, before the image counts. The proof that
the image is then safe is on the input side, because the PE image keeps no relocation for
PC-relative code and so cannot itself be scanned for a GOT load: ld adds no instruction, every
instruction of the image comes from a pulled member, and every pulled member passed. A member
pulled for a section the link later discards must pass as well, which errs safe. The plan above
needs none of this.

**The recipe's `x86_64-elf` entry changes with it.** It gains the GCC patch and the binutils
patch, read from the repository; its newlib options trade `--enable-newlib-register-fini` for
`--disable-newlib-register-fini`; its GCC takes `--with-libstdcxx-zoneinfo=no` and
`--disable-gcov`; libstdc++'s compile takes `-D_GLIBCXX_USE_WEAK_REF=0`, a C++-only target flag
that needs a key of its own, because the recipe gave `CXXFLAGS_FOR_TARGET` the family's C flags;
and `libc_a-init.o` and `libc_a-fini.o` are deleted from `libc.a` and `libg.a` after newlib
installs. As built, all of it; section 2's account of external data is rewritten for it, the
hidden header's removal included.

### 5.6 The gates and arms

- **The boot witness.** `tools/run-qemu-x86_64.sh` keeps the `fp found` line and replaces the
  pinned `fp trapped` line with an `fp enabled` one, read back from the machine: em=0 ts=0 mp=1
  ne=1 osfxsr=1 osxmmexcpt=1 osxsave=1 xcr0=7 xsave=832.
- **The per-core arm.** `fp_trapped_every_core` (`user/apps/common/selftest/selftest_smp.cc`)
  becomes an enabled-every-core arm: on each core, from ring 3, SMSW reads EM 0, TS 0 and MP 1,
  and XGETBV reads XCR0 = 7.
- **State survives a switch on one core**, on every x86 preset: two threads on one core, each
  with its own pattern in the sixteen YMM registers whole, the x87 stack, the MXCSR rounding
  field and the x87 precision field, compare after being switched out. One arm blocks, the
  voluntary path; one spins past its slice and checks it was preempted, the interrupt-exit path.
- **State survives a move between cores**, on the SMP presets: a thread loads its pattern, pins
  itself to another core and compares there, `KOS_SCHED_OP_CORE` confirming the core changed.
- **A new thread starts clean:** a thread in a slot a patterned thread vacated, and one spawned
  in another task, read the initial state, MXCSR 0x1F80, x87 control word 0x37F and every
  register zero. That proves the seeding and the absence of a leak across tasks.
- **A floating-point fault is contained:** a child task unmasks divide-by-zero in MXCSR and
  divides by zero, and one leaves an x87 exception pending; each task dies and the kernel and its
  parent run on. QEMU 11.1's TCG raises the #MF and never the #XM: the divide sets MXCSR.ZE with
  ZM clear and the thread runs on, measured. The arm then reports that MXCSR and is a PARTIAL,
  which the q35 selftest gate permits by name, so the SIMD half waits for silicon.
- **The first thread's FS, read back:** `arch_start`'s check of 5.5, on every core and in every
  image, which the poison makes fail where the write is missing, an AP's idle included.
- **libc on a core's first thread, before any switch:** `errnoprobe`, which the q35 presets join,
  gains arm F. Its first statement in `main`, ahead of every call that can block, runs `strtok`
  over a local buffer and `strtol` past `LONG_MAX`, and requires the two tokens, `errno` equal to
  `ERANGE`, `__getreent()` equal to the word `movq %fs:0` reads and different from
  `_GLOBAL_REENT`, and its own `switch_count`, read through a new scheduler probe op, equal to 0,
  which proves no switch-in has seated anything yet, and its core, read through
  `KOS_SCHED_OP_CORE`; it prints `F first core <n> switches 0`. The witness is those four
  together: no switch, the core, the FS cell's word and libc's state. On qemu-x86_64 root is the
  boot core's first thread, and the gate requires core 0. For an AP, one CI build of
  qemu-x86_64-smp2 sets a new configuration value, root's core mask, to core 1: `kmain` creates
  root with that core mask, core 1 enters it through `arch_start` while the boot core enters its
  idle, and the gate requires core 1. As built, `KOS_SYS_SCHED_PROBE` answers `KOS_SCHED_OP_CORE`
  and `KOS_SCHED_OP_SWITCHES` at one kernel core as well, its placement ops staying above one;
  the build of decision 17 is the `qemu-x86_64-smp2root1` preset, `KICKOS_ROOT_CORE_MASK` 0x2,
  which CI's `qemu-x86_64-ap` job runs for this arm.
- **Root's core mask is validated twice.** At configure, beside the isolated-core check of
  `cmake/isolated_cores.cmake`, the value must be 0, the default, or a mask whose every bit names
  a core below `KICKOS_KERNEL_CORES`, and a non-zero value is refused outside the shared
  multicore model; the refusal names the defconfig, as that check's does. `kmain` asserts the same
  before it creates root, so a value that reached the image another way panics at boot with a
  diagnostic rather than leaving root on no core that runs.
- **libc runs.** The apps `user/apps/common/CMakeLists.txt` gates on `KICKOS_HAVE_LIBC` build and
  run on q35, the per-thread reentrancy arm the SMP selftest gates on `KICKOS_LIBC_REENT` runs
  on the q35 SMP presets, and the q35 presets join the `cxxtest` list. As built, `cxxtest` runs
  on all seven q35 presets: throw and catch by exact type and by base, a local destructor run by
  the unwind, `std::vector` and `std::string` on the heap, virtual dispatch, `dynamic_cast` both
  ways and `typeid`, in an unprivileged worker. It names no iostream, and none of 5.5's gates
  asks one.
- **At the proposed heap:** `cxxtest` and the libc apps that allocate pass on every q35 preset at
  the 16 KiB default of 5.5, before that default stands; a failure there reopens decision 14. As
  built, every q35 image links that heap, `KICKOS_USER_HEAP_SIZE` 16384, and every one of them
  passes.
- **The C library's own exit, after the deletions:** the package's gate requires both archives,
  `libc.a` and `libg.a`, in every multilib directory, lists each with `ar t`, and fails if one is
  missing or holds a member named exactly `libc_a-init.o` or `libc_a-fini.o` (`libc_a-mbsinit.o`
  stays). Then `libc_exit` links on q35 with one addition to
  `user/apps/common/libc_exit/main.cc`: root registers an `atexit` handler that prints a marker
  line, after its sleep has let the worker exit and after it prints `root: exit()`, directly
  before `exit(EXIT_CODE)`. `tests/integration/check_libc_exit.sh` then requires the marker
  after `root: exit()` in the output, and the run's status 7.
  Registered any earlier, the handler sits on the one list `__register_exitproc` keeps for the
  whole image, the worker's `exit()` runs it and removes it, and the marker would print without
  root's exit having run a handler. The test is not changed until stage 3. As built, every
  emulated board's gate passes `--atexit` and requires the marker; the sim's does not, its
  `exit()` routing onto `KOS_SYS_EXIT` (`user/src/sim_exit.cc`) without running the host libc's
  handlers.
- **The gates:** the rescoped no-vector gate of 5.4, the no-GOT guard over every member of every
  package archive, refusing TLS relocations as well on q35, the weak guard of 5.5 over the same
  corpus, the app-split check, the trap red zones re-measured, and the Thread size pin: exact on
  the other arches, on x86 a `static_assert` that `sizeof(Thread)` is at most 2 KiB, beside exact
  assertions of the area's 64-byte alignment, its 832-byte size and the `KOS_CTX_*` offsets; the
  measured Thread size is recorded after the build.

The names this section proposes:

```text
kickos_x86_64_fp_enable        replaces kickos_x86_64_fp_trap: CR0, CR4 and XCR0 on one core,
                               then the CPUID 0xD check
KICKOS_X86_64_XCR0             7: x87, SSE and AVX
KICKOS_X86_64_XSAVE_SIZE       832, the standard-format area for that XCR0
KICKOS_X86_64_MXCSR_INIT       0x1F80, seeded into every new area
arch_context::xsave            the area, 64-byte aligned, at offset 64
arch_context::fs_base          the user address of the thread's cell, 0 for a privileged
                               context; publish_current writes it to IA32_FS_BASE
KOS_CTX_XSAVE                  the switch.S displacement, asserted in arch_x86_64.cc
KICKOS_X86_64_MSR_FS_BASE      0xC0000100
KICKOS_X86_64_FS_POISON        the canonical kernel-half address each core seeds IA32_FS_BASE
                               with after load_gdt, never a cell
KICKOS_X86_64_KERNEL_FLAGS     -mgeneral-regs-only, the kernel half's posture
kickos-x86_64-binds-local     the recipe's key for conan/toolchain/patches/
                               kickos-x86_64-binds-local.patch, wired in at stage 3
kickos-x86_64-pe-defweak-reloc
                               the recipe's key for conan/toolchain/patches/
                               kickos-x86_64-pe-defweak-reloc.patch (decision 21)
tools/check-x86_64-weak-undef.sh
                               the weak guard: any relocation against an undefined weak
                               symbol, but __cxa_pure_virtual's vtable words
KOS_SCHED_OP_SWITCHES          6: the caller's Thread::switch_count, a pure read
KICKOS_ROOT_CORE_MASK          root's core mask at create; 0, the default, is the task's set;
                               every bit below KICKOS_KERNEL_CORES, checked at configure and
                               asserted in kmain
fp enabled                     the boot witness line replacing fp trapped
fp_enabled_every_core          the arm replacing fp_trapped_every_core
vector_survives_block          one core, the voluntary switch
vector_survives_preempt        one core, the interrupt-exit switch
vector_survives_migrate        across cores
vector_starts_clean            a reused slot and another task
vector_fault_contained         #XM and #MF at ring 3
errnoprobe arm F               libc on a core's first thread, before any switch
```

### 5.7 The decisions

The maintainer ruled on decisions 1 to 16 on 2026-10-01, and on the revision of 7, 8, 10 and 11 and
on 17 to 20 the same day, then on the revision of 20, on what the build left open in 7 and 8,
with decision 21, and last on decision 15's limit and the WRFSBASE alternative to decision 11.
Every decision is accepted.

1. **Eager save on every switch**, accepted, against lazy switching with TS and #NM.
2. **Plain XSAVE**, accepted, against XSAVEOPT where CPUID offers it, or the compacted XSAVEC.
3. **Privileged threads saved like every other**, accepted, against skipping them.
4. **XCR0 fixed at 7**, accepted: AVX-512 stays a contained #UD even where present, and an
   x86-64-v4 posture later needs a 2696-byte area.
5. **The area inside `struct arch_context`**, accepted (2026-10-01) without an exact Thread size
   pin on x86: "I was too strict about the exact 1344-byte Thread size." The area's 64-byte
   alignment, its XSAVE size and the assembly offsets stay asserted exactly; `sizeof(Thread)` on
   x86 takes a 2 KiB ceiling and its measured size is recorded after the build; the other arches
   keep the exact pin. The build measured 896 bytes for the context and 1344 for `Thread` on
   qemu-x86_64 and qemu-x86_64-smp12, as the estimate gave (5.3). Against
   carving it from the top of the thread's kernel block or a separate per-slot array, which keep
   `Thread` at alignment 8. x86's unused boot contexts keep their areas this stage, about 10 KiB
   over twelve cores: removing them would need a separate context representation. The switch
   benchmarks are the check on the larger context's time cost.
6. **`-mgeneral-regs-only` for the kernel half**, accepted subject to the rescoped no-vector gate
   of 5.4, against keeping the three `-mno-` flags.
7. **The GOT: a KickOS patch to the `x86_64-elf` GCC's `ix86_binds_local_p`**, accepted with
   the patch, so that every symbol but a weak one binds locally under `-fpie`; against patching
   the configure for copy relocations and forcing the hidden header, with
   `--disable-libstdcxx-visibility`, into the libraries' build, which together still leave the
   address of a function declared in a default-visibility region on the GOT, 15 relocations in the
   38 members measured, and leave app code's references into `std` to the header. Copy
   relocations cannot be supplied as a configure result: `gcc/configure` assigns it before its
   probe. The visibility header stays forced into every x86 compile until the complete
   rebuilt archives, every member of every installed archive, pass the no-GOT guard without it;
   the 38-member experiment supports the approach and is not that gate. The first build left
   `libgcov.a`, compiled `-fpic` by libgcc's own build, the one installed archive failing that
   gate; ruled (2026-10-01): the family is configured with `--disable-gcov`, so it installs no
   `libgcov.a`, and the no-GOT and weak guards pass over every member of every installed archive.
   The header is then removed with its plumbing, every q35 preset passing without it (5.5).
8. **Weak undefined references removed at the source**, accepted: `--disable-newlib-register-fini`,
   `-D_GLIBCXX_USE_WEAK_REF=0` and `--with-libstdcxx-zoneinfo=no`, for this family alone, with the
   deletion of `init.o` and `fini.o` under decision 20, so the guard keeps reading every member
   whole; against narrowing the guard to the members each link's map names, which stays the
   fallback should one reference have to stay. The ruling adds the weak guard of 5.5, which
   refuses an unresolved weak call, address or word the no-GOT guard cannot see, and admits
   `__cxa_pure_virtual`'s vtable words by name. Whether q35 images link `-u __cxa_pure_virtual`
   was not ruled; the guard admits the zero slot either way.
   The build found libstdc++ linking into no q35 image, `ld -m i386pep` writing no base
   relocation for an absolute word that names a weak definition; decision 21 answers it (5.5).
9. **The libraries' instruction set**: built at the default x86-64 as today, accepted, against
   `-march=x86-64-v3`, which the apps get.
10. **Reentrancy through the FS base and a stack-top cell**, accepted, the first thread's setup
    and its tests explicit in 5.2, 5.5 and 5.6. TLS relocations are refused on q35 meanwhile;
    against `KICKOS_TLS` on x86 with a variant II carve and `KICKOS_REENT_IN_TCB`, which would
    also bring `thread_local` to q35.
11. **IA32_FS_BASE written in `publish_current`**, accepted: at every switch-in and at every
    core's first thread, never against a cached value, with the FS selector null, the base
    established by each core's poison write rather than by the null load, and read back by
    `arch_start`; against FSGSBASE's ring 3 read, which needs CR4.FSGSBASE and lets ring 3 write
    both bases. *Ruled (maintainer, 2026-10-01):* WRFSBASE with CR4.FSGSBASE in the WRMSR's
    place is not adopted, and the WRMSR of IA32_FS_BASE stays. It takes 22 to 23 cycles off the
    window under KVM, about 4% (5.3), which does not justify letting ring 3 write its own FS and
    GS bases.
12. **Unwind tables registered by a priority constructor in `kickos_cxx_rt`**, accepted, against
    a strong call in `root_entry.cc` that would pull libgcc's unwinder into every q35 image.
13. **`-mno-red-zone` kept for ring 3 code**, accepted, although every ring 3 entry changes stack
    and the red zone would be safe there; the package's libraries are built without one.
14. **The heap default**: q35 takes the fleet's 16 KiB, copied per address space, accepted, with
    the C++ and heap tests verified at that size, the gate 5.6 adds.
15. **The switch cost**: "add a dirty-vector benchmark variant" and measure the whole switch,
    which 5.3 does by opening the window before `publish_current`, so it holds the WRMSR.
    Measured on this host's processor under KVM (5.3, 2026-10-01): the window grows from 40 to
    256 TSC ticks at p50 and at p99, 20 to 128 ns, the dirty variant reading the same, and the
    ping-pong round trip by 132 ns, from 546 to 678. A limit in ticks was proposed first: the
    whole-switch window at most 320 ticks, 160 ns, at p50 and 512 ticks, 256 ns, at p99 on a
    processor of this class, the bucket edges above the slowest rows of one session.
    *Ruled (maintainer, 2026-10-01):* before any limit is set, the window is split into its
    parts; measurement only, optimisations a later decision. Split under KVM (5.3): of the 566
    core cycles the window grows by, the XRSTOR64 takes 358, the XSAVE 143 and the FS base WRMSR
    93, each measured by dropping it, and the rest of stages 1 to 3 nothing measurable; clean and
    dirty state read the same. XSAVEOPT, XSAVEC and WRFSBASE in their parts' places each take
    20 to 24 cycles off the clean window, the first two not off the dirty one; WRFSBASE is not
    adopted (decision 11). A limit in TSC ticks moves with the core's clock: the same window read
    254 ticks at 5.05 GHz and 298 below 4.6. *Ruled (maintainer, 2026-10-01), the limit:* the
    whole window, `switch:` and `switch-vec:` alike, takes at most 700 core cycles at p50 under
    KVM, measured with 5.3's cycles-per-tick calibration, each run converting its window at the
    ratio its own dependent-add loop of fixed length gives. As built it is about 642 core
    cycles, 76 of them, 40 ticks, the window before stage 1. Stated in core cycles and not TSC
    ticks, replacing the proposal in ticks, because the TSC is fixed at 2 GHz while the core's
    clock moved between 3.5 and 5.1 GHz. No gate reads it: it is checked by hand whenever the q35
    switch rows are taken again (5.3).
16. **The sequencing**: "use the proposed staged sequence, with a bootable gate at each stage".
    - *The vector state.* The bench window of 5.3 widened and the rows taken, then the enable of
      5.1 and the area of 5.2 on both switch paths and at the first thread's start, with their
      arms. Gate: every q35 preset boots and prints `fp enabled`; the selftest passes
      `fp_enabled_every_core` and the `vector_` arms, `vector_survives_migrate` on the SMP
      presets; the area's assertions hold, `sizeof(Thread)` is within the 2 KiB ceiling and
      recorded; the rows are taken again. The tree
      is still compiled without vectors, so the no-vector gate still reads every image whole: it
      admits the port's state instructions by site, as 5.4 does, and the arms' vector
      instructions by name until the next stage.
    - *The flag split.* The two postures of 5.4 and the rescoped no-vector gate. Gate: every q35
      preset boots and passes the selftest, the first stage's arms included; the rescoped gate
      passes with its new control; the app-split check passes; the trap red zones re-measured.
    - *newlib and libstdc++.* First the package, with the recipe changes of 5.5. Gate, the
      recipe's own: the no-GOT guard and the weak guard over every member of every archive, TLS
      relocations refused; `libc.a` and `libg.a` in every multilib directory, neither holding
      `libc_a-init.o` or `libc_a-fini.o`; every q35 preset, still linking no library, built
      with the new compiler, boots and passes its selftest.
      Then the port of 5.5: flags, stubs, heap, link, FS base and `.eh_frame`. Gate: every q35
      preset boots with `arch_start`'s read-back on every core; `errnoprobe` passes, arm F on
      core 0 and, in the build of decision 17, on core 1; the libc apps, the per-thread
      reentrancy arm and `cxxtest` pass at the 16 KiB heap; `libc_exit` prints root's `atexit`
      marker after `root: exit()` and shuts down with status 7; the no-GOT guard, the weak
      guard and the app-split check pass over the images that now link the libraries.
17. **The AP's first-thread libc witness**, accepted with mask validation: root pinned to core 1
    by a new configuration value, root's core mask, in one qemu-x86_64-smp2 CI build, the value
    checked at configure and asserted in `kmain` against the cores the kernel drives (5.6), so an
    invalid one cannot leave root unrunnable. The witness is arm F's: a `switch_count` of 0, the
    core, the FS cell's word and libc's state. Against leaving the AP to `arch_start`'s read-back,
    which proves the write there but not that libc reads it.
18. **A KickOS patch to GCC in the `x86_64-elf` recipe**, accepted, with its supply changed: the
    patch is KickOS-authored, so it lives in this repository for review,
    `conan/toolchain/patches/kickos-x86_64-binds-local.patch`, and the release mirrors it (5.5).
    Wiring it in, at stage 3, changes the recipe's revision, so every family's package rebuilds,
    and changes CI's cache key; any later edit to the recipe, its `conandata.yml` or a patch does
    the same. The recipe applies every patch from its own export, never from the release.
19. **`std::chrono` time zones on q35**, accepted: `--with-libstdcxx-zoneinfo=no` leaves
    libstdc++ its "ersatz" database, `UTC`, `GMT` and their aliases with every other zone
    unavailable, and takes about 139 KiB of embedded tzdata out of `tzdb.o`, which only an image
    that pulls that member saves (5.5). Against keeping the tzdata, which keeps `tzdb.o`'s weak
    GOT load and so needs the narrowed guard of 5.5.
20. **`init.o` and `fini.o` deleted from the x86_64 `libc.a` and `libg.a`**, accepted
    (2026-10-01) with the archives' own member names and an ordered `libc_exit` witness.
    `init.o` goes because `__libc_init_array` would walk the kernel's constructor range on q35,
    while `root_entry.cc` runs the app's range itself. `fini.o` goes only with decision 8's
    `--disable-newlib-register-fini`, since `register_fini` names `__libc_fini_array`. The
    members are `libc_a-init.o` and `libc_a-fini.o`, as `ar t` lists them; the recipe deletes
    both from `libc.a` and from `libg.a`, which newlib may install as a copy, and the package's
    gate requires both archives in every multilib directory and fails if either name remains in
    either. `libc_exit` then registers a printing `atexit` handler in root only after the
    worker has exited, just before root's `exit(7)`, and the gate requires the handler's line
    after `root: exit()` and before the status-7 shutdown: newlib's handler list is one list
    for the whole image, so a handler registered earlier could be run, and consumed, by the
    worker's `exit()`, and the gate would pass without witnessing root's path (5.6). Against keeping them, which needs the narrowed guard
    of 5.5.
21. **A KickOS patch to binutils in the `x86_64-elf` recipe**, ruled (2026-10-01): one test in
    `generate_reloc` (binutils 2.47's `ld/pe-dll.c`, line 1691) accepts a weak definition,
    `bfd_link_hash_defweak`, beside a strong one, so `ld -m i386pep` gives the absolute words that
    name it the DIR64 base relocation `app_relocate` moves by; an undefined weak symbol still gets
    none. It is `conan/toolchain/patches/kickos-x86_64-pe-defweak-reloc.patch`, vendored as the
    GCC patch of decision 18 is, for this family alone. Against a link step adding the missing
    records to the image's `.reloc` and its boot copy, and against `-fno-weak`. With it
    `cxxtest`'s 55 unrelocated words get their records and full C++ runs on every q35 preset
    (5.5, 5.6).

### 5.8 The measurements behind 5.5

Every figure of 5.5 was measured on 2026-10-01 against the package
`kickos-toolchain-x86_64-elf/1.0`, recipe revision a849b914, package 63fead08, and the pinned
GCC 16.2.0 source; nothing in the repository or the package changed. The scripts and their
outputs are not kept: each figure is reproducible by the method stated with it.

**GCC's source, read.** The copy-relocation probe and its define are `gcc/configure.ac` lines 6135
to 6181, the generated `gcc/configure` lines 32764 to 32808: the result is assigned `no`
unconditionally ahead of a check that is no `AC_CACHE_CHECK`, so a preset is overwritten. The
macro's readers in `gcc/config/i386/i386.cc` are `legitimate_pic_address_disp_p`, line 11886, its
branch at lines 11955 to 11966, and `ix86_binds_local_p`, lines 27437 to 27452, which hands it to
`default_binds_local_p_3` as the uninitialised-common argument alone. In `gcc/varasm.cc`,
`default_binds_local_p_3` never makes an undefined weak symbol local (line 7952), infers no
visibility for an undefined symbol (line 7957), and does not make an external default-visibility
declaration local. `-mdirect-extern-access` is `gcc/config/i386/i386.opt` lines 1212 to 1214,
documented in `gcc/doc/invoke.texi`. libstdc++'s unconditional default-visibility regions open at
`libsupc++/new` line 55, `libsupc++/typeinfo` line 44, `libsupc++/cxxabi.h` line 48,
`unwind-cxx.h` line 53, `eh_atomics.h` line 41, `cxxabi_forced.h` line 38 and
`cxxabi_init_exception.h` line 38; `acinclude.m4` lines 3916 to 3942 define
`--disable-libstdcxx-visibility`, which `include/Makefile.am` line 1435 writes into `c++config.h`.

The weak references' sources: `newlib/libc/stdlib/__call_atexit.c` lines 22 to 60, under
`_WANT_REGISTER_FINI`, which `newlib/configure.ac` lines 50 to 57 and 433 tie to the register-fini
option; `newlib/libc/misc/init.c` lines 19 to 22 and `fini.c` lines 17 and 18;
`libstdc++-v3/src/c++11/cow-stdexcept.cc` line 201, whose `_GLIBCXX_USE_WEAK_REF` defaults to
`__GXX_WEAK__` in `bits/c++config` lines 755 and 756 and is set to 0 for Cygwin in
`config/os/newlib/os_defines.h` lines 56 and 57; `src/c++20/tzdb.cc` lines 86 to 105 and 1345 to
1366, compiled because the package defines `_GLIBCXX_STATIC_TZDATA`, which
`--with-libstdcxx-zoneinfo=no` does not (`acinclude.m4` lines 5564 to 5640, its `no` branch at
5602 to 5605); `gcc/cp/decl.cc` lines 5776 to 5780 for `__cxa_pure_virtual`. Without either
define, `tzdb.cc` sets `TZDB_DISABLED` at line 106; `reload_tzdb()` and `remote_version()` then
throw (lines 2037 and 1774), and `_S_init_tzdb`, line 1818, builds the "ersatz" database of
`Etc/UTC`, `Etc/GMT` and their links through line 1880.

The deletions' dependencies: `register_fini` passes `__libc_fini_array` to `atexit` at
`newlib/libc/stdlib/__call_atexit.c` line 52; newlib's `install-data-local` makes `libg.a` at
`newlib/Makefile.am` lines 261 and 262, `ln` with a `cp` fallback; `user/src/root_entry.cc` line 44
is the root task's own loop over the app's constructor range. Every `atexit` handler goes onto the
one list `__atexit`, defined at `newlib/libc/stdlib/__call_atexit.c` line 20 and extended by
`__register_exitproc` (`newlib/libc/stdlib/__atexit.c`, from line 63); `__call_exitprocs`, line 68
of `__call_atexit.c`, removes each entry before it calls it (line 103), so the first `exit()` in
the image consumes every handler registered before it.

**The configure, run.** The package's own GCC build reports no for "linker PIE support with copy
reloc" and defines `HAVE_LD_PIE_COPYRELOC 0`, and the unpatched configure run with
`gcc_cv_ld_pie_copyreloc=yes` in its environment reports the same. With `x86_64-*-elf*` added to
the probe's case it reports yes and defines 1. The compiler built for the rows below was
configured as the package's final GCC, its tools found through `--with-build-time-tools`; its
`auto-host.h` differs from the package's in that macro and in `HAVE_isl`, isl not having been
built, which touches only graphite passes no default level enables. A first build configured with
`--with-as` and `--with-ld` probed several assembler features differently and wrote a writable
`.eh_frame`, so it was discarded.

**The probe.** One C and one C++ translation unit, each naming every reference form once: strong
data, a strong read-only array, a called function, a function whose address is taken, weak data, a
weak function and hidden data; `std::cout` with `std::endl`, `typeid(std::runtime_error)`, a
template's static data member under `extern template`, a `throw std::runtime_error`, and an
abstract class's vtable. Each compile takes the libraries' flags, `-g -O2 -fpie -mno-red-zone
-ffunction-sections -fdata-sections`, plus the row's; "hidden" forces the pragma of the
visibility header, `toolchain-x86_64-hidden.h`, and "no visibility" puts first on the include
path a copy of the package's `bits/c++config.h` with `_GLIBCXX_HAVE_ATTRIBUTE_VISIBILITY 0`,
which is what `--disable-libstdcxx-visibility` installs. GOT relocations per translation unit:

| Compiler | Flags beyond the libraries' | C | C++ |
| --- | --- | --- | --- |
| package GCC | none | 5 | 5 |
| package GCC | `-mdirect-extern-access` | 5 | 5 |
| package GCC | `-mno-direct-extern-access` | 5 | 5 |
| package GCC | hidden | 2 | 4 |
| package GCC | hidden, no visibility | 2 | 2 |
| package GCC | `-fno-pic`, small or large code model | 0 | 0 |
| copy-relocation GCC | none | 3 | 1 |
| copy-relocation GCC | hidden, no visibility | 2 | 0 |
| Debian GCC 16.2.0 | none | 3 | 1 |
| patched `ix86_binds_local_p` | none | 2 | 0 |
| patched `ix86_binds_local_p` | hidden, no visibility | 2 | 0 |

The package GCC keeps, in C, the array, the data, the function address and both weak symbols, and
in C++ `std::cout`, the typeinfo twice, the template's member and the destructor. The
copy-relocation GCC and Debian's keep the function address, the weak symbols and the destructor.
The patched GCC keeps the two weak symbols alone: `std::cout`, the typeinfo, the destructor and the
template's member become `R_X86_64_PC32`, and the address of a function becomes a `lea` where the
package's GCC loads it from the GOT. `-fno-pic` emits `R_X86_64_32` or `R_X86_64_32S` for every
address taken, the personality routine's in `.eh_frame` included, and `-mcmodel=large` an
`R_X86_64_64` in text for each; linked by `ld -m i386pep` at 0x400000 one becomes a HIGHLOW base
relocation, and at 0x10000000000 "relocation truncated to fit". `arch/x86/x86_64/apprel_x86_64.cc`
refuses every record but ABSOLUTE and DIR64.

**The archives, classified.** Every GOT relocation of the five archives, by the type of its target
where some archive defines it:

| Archive | Data | Function | Undefined |
| --- | --- | --- | --- |
| `libc.a` | 347 | 118 | 7 |
| `libm.a` | 71 | 0 | 0 |
| `libstdc++.a` | 2220 | 105 | 1 |
| `libsupc++.a` | 36 | 14 | 0 |
| `libgcc.a` | 0 | 0 | 0 |

The 8 undefined targets are the weak symbols of 5.5. The functions whose address is taken most
often are `std::filesystem::filesystem_error`'s destructor, 37 times in each ABI, `_strtoul_r` 32,
`_strtol_r` 19, `_wcstoul_r` 18, `std::bad_alloc`'s destructor 13 and `__seofread` 11. Outside
debug information the archives hold no 32-bit absolute relocation and no TLS relocation, and no
text section an absolute one: every other relocation is `R_X86_64_PC32`, `R_X86_64_PLT32` or an
`R_X86_64_64` data word.

**The 38 members, rebuilt.** Each member's own compile line, the one the package build ran, with
the driver replaced, the row's flags appended and the output redirected to a scratch directory:
libstdc++'s and libsupc++'s from the `libtool: compile:` lines of the package's GCC build log,
newlib's from `make -n` in a copy of its build directory. No file of the package or its build tree
changed. The members:

```text
newlib     findfp svfiscanf nl_langinfo tzset_r strtod __call_atexit init fini vfprintf impure
           errno strtok; libm e_asinl fenv
libstdc++  ios_init globals_io locale_init locale-inst wlocale-inst sstream-inst functexcept
           cow-stdexcept tzdb system_error string-inst ostream-inst
libsupc++  eh_personality eh_terminate eh_aux_runtime eh_throw eh_alloc vec new_op new_opa guard
           pbase_type_info atexit_thread pure
```

Every row of 5.5's table built all 38. The first reproduces the package's members, `findfp` 15,
`svfiscanf` 32, `nl_langinfo` 26, `e_asinl` 29, `sstream-inst` 146, `wlocale-inst` 141,
`locale-inst` 128 and `locale_init` 91 among them. The 15 of the copy-relocation row with the
header are `std::bad_cast`'s and `std::bad_typeid`'s destructors in `eh_aux_runtime.o` and
`functexcept.o`, `recursive_init_error`'s in `guard.o`, `operator new[]` and `operator delete[]` in
`vec.o`, `zoneinfo_dir_override` in `tzdb.o`, and the 7 weak references of `__call_atexit.o`,
`init.o` and `fini.o`. The 8 of the binds-local row are the weak references alone: `__libc_fini`,
the four bounds of `init.o`, the two of `fini.o` and `zoneinfo_dir_override`.

**ld and a weak undefined symbol.** One undefined weak reference per form, linked with KickOS's PE
flags:

| Form | At `--image-base=0x400000` | At 0x80000000 or 0x140000000 |
| --- | --- | --- |
| a GOT load | links: a load from link address 0 | relocation truncated to fit |
| `call weak@PLT` | links: `call 0` | relocation truncated to fit |
| `lea weak(%rip)` | links: the link-time zero | relocation truncated to fit |
| `.quad weak` | links: 0, no base relocation | links |

The selftest's rebased image is linked at 0x10000000000
(`user/apps/common/selftest/CMakeLists.txt`); every other image at 0x400000.

**The removals, emulated.** On the 38 members: `-D_GLIBCXX_USE_WEAK_REF=0` on every line; first on
the include path, a copy of the build's `bits/c++config.h` without `_GLIBCXX_STATIC_TZDATA`, which
is what `--with-libstdcxx-zoneinfo=no` generates, and a copy of newlib's `newlib.h` without
`_WANT_REGISTER_FINI`, which is what `--disable-newlib-register-fini` generates; `init.o` and
`fini.o` left out of the guard's input. `cow-stdexcept.o` then defines none of its 65
transactional clones and names no `_ITM_` symbol; `tzdb.o` names no `zoneinfo_dir_override` and
shrinks from 198971 to 56291 bytes; `__call_atexit.o` names neither `__libc_fini` nor
`__libc_fini_array`; the only weak references left are `__cxa_pure_virtual`'s vtable words; and
the guard reports the 36 members kept clean.

**The thread pointer and TLS, compiled.** At the libraries' flags, `__builtin_thread_pointer()`
compiles to `mov %fs:0x0,%rax`; a locally defined `__thread` variable to the same load and an
`R_X86_64_TPOFF32`; an external one to an `R_X86_64_GOTTPOFF`. The guard refuses the last and
passes the second.

## 6. The parts

| Part | Lands |
| --- | --- |
| M10.2.1 | this design |
| M10.2.2 | the recipe, the consumer and the one variable, and the Cortex-M family: full and nano, every Arm board on it, CI's Arm jobs on it |
| M10.2.3 | AArch64 and RISC-V |
| M10.2.4 | x86_64: the compiler with PE32+ binutils, then newlib and libstdc++ linked into images with user vector state (section 5) |
| M10.2.5 | ESP32 and RX |
| M10.2.6 | the deletions and CI's prebuilt packages for Linux x86_64 and macOS arm64 |
| M10.2.7 | the exit: every CI preset and the fleet's silicon on the package, the red zones and size budgets re-measured |

**Clean-room.** Vendors' and Espressif's build configurations are read and cited by path, never
copied; the recipe's configure lines are this project's own.
