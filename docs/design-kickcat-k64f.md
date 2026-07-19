<!-- SPDX-License-Identifier: CECILL-C -->
# Design brief: running the KickCAT slave on KickOS/K64F

Scoping for the north-star integration (see `reference/architecture.md`, "Sim end-goal"):
run KickCAT's `freedom-k64f` EtherCAT slave example on KickOS -- first on the sim over
`EmulatedESC`, then on K64F over a real LAN9252 ESC via the unprivileged DSPI0 driver.
KickCAT lives at `../KickCAT` (used, not vendored). Stage A (the sim slave) has LANDED as
`user/apps/kickcat_slave` (a CI test); the K64F hardware path below is still the plan.

## Verdict

The sim path has LANDED (Stage A, `user/apps/kickcat_slave`). The K64F hardware path is not
runnable today. The de-risking fact: KickCAT already cross-compiles for
Cortex-M4F (its own `examples/slave/nuttx/lan9252/freedom-k64f` target), so the library builds
for our exact chip. The blockers below were the original KickOS gaps; all are now resolved
(recorded here as the design record). The consumer app itself now lives in the KickCAT tree
(the KickOS-side `examples/freedom-slave` was removed) -- see "The ESC transport" below.

1. **Full-C++ userspace opt-in (RESOLVED).** KickCAT is exception-using, STL/heap-using
   C++17. Full C++ is now a per-app opt-in: an app links the exported `kickos_cxx` target
   (`-fexceptions -frtti`, `libstdc++`/`libsupc++` kept) instead of the freestanding `kickos`.
   Proven on real K64F silicon -- the slave reached `OPERATIONAL` under SYSMPU enforcement.
2. **The `k64dspi` driver is a real transport (RESOLVED).** It exposes callable
   `spi_transfer`/`spi_enable_cs`/`spi_disable_cs` (+ `spi_driver_start`), exported as the
   `kickos_k64dspi` lib with the public header `<kickos/driver/k64dspi.h>` (source at
   `user/driver/k64dspi`). An out-of-tree consumer links it on top of the OS.
3. **`k64dspi` silicon (RESOLVED).** The polled-FIFO transport (~10 MHz) reached `OPERATIONAL`
   against a real LAN9252. K64F peripheral isolation stays coarse (AIPS ceiling --
   driver-in-userspace holds, but no per-thread peripheral boundary): an accepted, documented
   bound, not a gap.

## The KickCAT OS backend (small)

The single-slave loop (`while(true){ slave.routine(); }`) needs a **time-only** backend -- no
threads/mutex/condvar in the slave path (grep-confirmed). KickCAT's hardware seam is
`AbstractSPI` (the `Lan9252` `AbstractESC` is portable over a `shared_ptr<AbstractSPI>`).

| KickCAT OS symbol | Maps to |
|---|---|
| `kickcat::sleep(ns)` | `kos_sleep_ns(ns)` |
| `since_epoch()` / `elapsed_time()` | `kos_clock_now()` (monotonic ns; the slave only takes deltas) |

Delta: one new `lib/src/OS/KickOS/Time.cc` + an `elseif(KICKOS)` branch in KickCAT's
`lib/CMakeLists.txt` (Time only; skip `Socket.cc`). **Gap:** KickOS `newlib_stubs.cc` has no
`_gettimeofday`/`clock_gettime`/pthreads, so `std::chrono` clocks and any POSIX-time backend
return 0 -- the KickOS backend MUST route time through `kos_clock_now()` directly (cannot reuse
`OS/Unix/Time.cc` or the default `SinceEpoch.cc`). (Optional later, for multi-threaded use:
`kickcat::Thread`->`kos::thread::spawn`, `Mutex`->binary `kos::Semaphore` -- priority models
differ; not needed for freedom-k64f.)

## The ESC transport (LAN9252 over DSPI0) -- k64dspi loopback -> transport delta

`AbstractSPI` pure virtuals: `open/close/transfer(tx,rx,size)/enableChipSelect/disableChipSelect/
setMode/setBaudRate`. Framing contract from `Lan9252.cc`: a CSR access holds **CS across two
`transfer()` calls** (`enableChipSelect` -> write 3-byte cmd [1 instr + 2 addr, big-endian] ->
read/write payload -> `disableChipSelect`); `read`=`transfer(nullptr,rx,n)` (shift dummy 0x00),
`write`=`transfer(tx,nullptr,n)` (discard rx); MSB-first, SPI **mode 0** (matches the k64dspi
CTAR0). What `k64dspi` must gain:

1. A callable **`spi_transfer(tx,rx,len)`** (blocking: enqueue descriptor to the driver thread,
   block on a semaphore) -- specced in the DSPI brief, not built.
2. **CS-hold**: map `enableChipSelect`/`disableChipSelect` to DSPI `PUSHR.CONT` (hardware PCS0,
   no extra GPIO/region) so CS stays asserted across header+payload; the demo releases per frame.
3. **Multi-byte + null-buffer**: loop frames for `len>1` (4-entry TX FIFO), send 0x00 on null tx,
   discard on null rx; one EOQ wake per logical transfer.
4. **Real baud/timing**: the demo runs a slow loopback baud; LAN9252 wants ~10 MHz + datasheet
   CS/inter-frame delays.
5. **App<->driver wiring**: the KickCAT slave thread and the DSPI driver thread are separate
   unprivileged threads over the AIPS-opened slot; `AbstractSPI::transfer` calls `spi_transfer`
   which does the cross-thread request/reply -- the real new construct.

Bench-verify items (not design blockers): LAN9252 SPI mode/baud boot constants; an
ungranted/supervisor-only DSPI access surfaces as a **BusFault** (K64F is bus-slave-side, not a
SYSMPU MemManage); W1C `SR.EOQF` before re-arm or the line storms.

## Build integration

The dependency-inversion path is ready: a KickOS app does `find_package(KickOS)` +
`target_link_libraries(app PRIVATE kickos kickcat)`. Real work: (a) the **full-C++ opt-in** in
the `kickos` package (see "Libc strategy" below); (b) **heap**: `_sbrk` is a fixed 64 KB arena on
MCU -- the LAN9252 path allocates modestly (CoE OD, `Mailbox(1024)`, some `std::vector`/
`shared_ptr`); plausible on the 256 KB-SRAM K64F but must be measured; (c) port the example `main`
off NuttX-isms (`/dev/spi0`, accel, LEDs).

## Libc strategy for the full-C++ opt-in (the Stage B gate)

Layered, two tiers -- NOT a global libc-brand choice:

- **Freestanding (default).** A plain app links the minimal KickOS libc (`lib/libc`), no
  libstdc++, `-fno-exceptions -fno-rtti -nostdlib++`. Every arch, uniform, zero-overhead.
- **Full C++ (per-app opt-in).** Link the TOOLCHAIN's `libstdc++`/`libsupc++` over the TOOLCHAIN's
  OWN libc, so libstdc++ always sits on the libc it was built against -- no cross-libc shim, zero
  ABI mismatch. The fleet is on **pinned vendor toolchains that are all newlib**: Arm GNU
  Toolchain (ARM), RISCStar (RISC-V), GNURX (RX). So the opt-in rides **newlib on every arch** --
  the SAME seam, no per-toolchain libc deltas -- and each toolchain ships its own newlib-built
  libstdc++.
- **(rejected) libstdc++ over the KickOS own libc.** The fragile "host the toolchain C++ runtime
  on your own libc" path (the NuttX lesson) -- we never do it. It would also fail for KickCAT: its
  slave core uses `std::stringstream`/`std::to_string` (-> locale), so there is no narrow subset.

**The seam (newlib, uniform across arches):** operator new/free over the heap,
`__cxa_atexit`/guards, `abort`, the libgcc unwinder (`_Unwind_*`, libc-independent), plus newlib's
bottom edge -- `_sbrk` + `_impure_ptr`/reent, which `user/src/newlib_stubs.cc` ALREADY provides;
`__malloc_lock`/`__malloc_unlock` only when userspace threads libstdc++. Because newlib is uniform,
there is no per-toolchain seam variant -- the Stage-B delta is small: the same stubs already in the
tree serve every arch.

**Caveats to document on the opt-in (both vendor toolchains build single-thread --
`--disable-threads` / `--enable-threads=no`):** no thread-safe function-local statics --
initialize statics single-threaded (before spawning) or keep the app single-threaded;
`shared_ptr`'s control-block refcount is **non-atomic** -- never copy/destroy one `shared_ptr`
across threads (the pointee was never protected regardless). Both are fine for the single-threaded
slave loop.

**Footprint is not a differentiator:** the costs that matter (EH/unwind tables ~10-15 KB; locale
if `stringstream` survives) are libc-independent; all options fit K64F. **Provisioning:** none --
libstdc++ ships WITH the pinned vendor toolchain (no apt package, no separate install), so
`#include <vector>` compiles out of the box. The Stage-B gate is therefore just the
`-fexceptions`/`-frtti` opt-in plumbing in the `kickos` package, not a libc/libstdc++ install.

**Boundary discipline (why the mixed link is sound).** A FULL_CXX app is `-fexceptions`; the
kernel/lib/arch/libc are `-fno-exceptions` (no unwind tables). This is safe by *how it fails*, not
by luck: the only kernel->app entries are `kickos_app_main` and the thread trampoline, and the IRQ
model is wait-on-semaphore (no kernel-invoked app callbacks), so there is no throw path through an
ISR. An exception that escapes an app entry (or an uncaught `bad_alloc`, or a throwing global ctor)
reaches a frame with no FDE/`.exidx` entry; both the DWARF and ARM-EHABI unwinders fail in *phase
1* -- before any frame is popped -- so `__cxa_throw` calls `std::terminate` -> `abort` ->
`kos_exit`, with no half-unwound kernel state. Consequences to document for app authors: catch your
own exceptions; an escape from a *worker* thread terminates the whole image (not just the thread);
static dtors / `atexit` handlers never run (returning from `kickos_app_main` goes
`root_entry -> arch_shutdown`, never `exit()`); a throwing ctor before the scheduler may wedge
rather than exit. On RISC-V the `.eh_frame` table is registered at boot via a *weak*
`__register_frame` (freestanding images never pull the FDE machinery + heap; a FULL_CXX link does).
Full-C++ *under enforcement* is DONE on K64F: `mk64f.ld` routes `libkickos_user.a`'s heap arena +
the libstdc++ writable state into `.appdata`/`.appbss` via `archive:member` colon-inclusion of the
closed kernel set (a bare `*user*` selector missed the archive members), and the EHABI `.exidx`
tables are read-only in flash, reached through the code grant -- so an unprivileged thread runs the
full runtime (the freedom-k64f slave reached OPERATIONAL under SYSMPU; cxxtest ALL PASS on K64F
silicon under enforcement). The colon-inversion is now fanned out to all 6 enforcement chip linker
scripts (m2-review-followups #1+#2; xmc4800/esp32c6 silicon-confirmed 20/20), so the runtime's
writable state lands app-side everywhere. Full C++ under enforcement is then heap-size-gated,
NOT a RAM ceiling: the default 64K `s_heap` forces a 128K pow2 `.appdata` window, but the heap is a
provisioning knob (`KICKOS_HEAP_SIZE`). xmc4800 sets 16K (a 32K window) and runs cxxtest under PMSA
enforcement on silicon (ALL PASS) -- full C++ proven on a second arch beyond K64F. Only a part whose
RAM cannot spare the pow2 window for its actual heap need is truly gated. On RISC-V/PMP the DWARF FDE
registry + libc globals are a further, distinct concern. See `docs/m2-review-followups.md`.

## Staged plan

- **A -- Sim slave on `EmulatedESC` (no hardware, CI-provable, smallest) -- LANDED (`user/apps/kickcat_slave`).** Add the Time-only
  KickOS backend; enable full C++ on the sim app (host libstdc++, near-free); a sim app that
  builds `EmulatedESC` + `PDO` + `slave::Slave` and runs `routine()`. Proves OS-abstraction +
  KickCAT-on-KickOS-libc + the slave state machine, under CTest. (Single instance first;
  multi-slave via invariant #7 later.)
- **B -- Full-C++ opt-in on MCU.** The gating enabler: `-fexceptions -frtti` + toolchain
  libstdc++/libsupc++ over newlib in the `kickos` package (libstdc++ ships with the pinned vendor
  toolchain -- no install, so this is opt-in plumbing only). Prove with a tiny throw/catch K64F
  image; measure/raise the `_sbrk` arena.
- **C -- Real `spi_transfer` + `AbstractSPI` backend.** Build the enqueue-and-block transport +
  CS-hold + null-buffer + multi-frame; write KickCAT's `AbstractSPI` KickOS impl. Prove on bench:
  raw loopback (SOUT<->SIN jumper) first, then `Lan9252::init()` against a wired LAN9252
  (BYTE_TEST reads 0x87654321).
- **D -- freedom-k64f slave on silicon.** Port the example `main` (drop/replace accel/LED I/O),
  flash, bring a master up. Proves the north star: one slave on KickOS/K64F over a real SPI ESC,
  mirroring the Stage-A sim slave.

Critical path (all resolved): the OS backend was trivial; the two real efforts were
**(1) userspace full-C++ opt-in** (now the `kickos_cxx` leaf) and **(2) the `spi_transfer`
transport + CS-hold `AbstractSPI` backend** (now the exported `kickos_k64dspi` driver).
`k64dspi` reached `OPERATIONAL` against a real LAN9252 on K64F silicon (Stages C-D).
