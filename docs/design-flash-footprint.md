<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# Flash and RAM footprint -- where the bytes actually go

> **Status: ACTIVE** -- a measurement record. Every number below was produced on branch tip
> `ae7996e` (`M4.5.2-unpriv-root-stage2`) with the Arm GNU Toolchain 15.3.1, GCC 15.3.1 for RX
> and Xtensa, by building all fourteen real boards in both `KICKOS_ENABLE_SELFTEST` settings.
> It reports the current footprint and separates what is recoverable from what the kernel
> genuinely needs; it does not schedule any of that work. See `design/README.md` for the marker
> taxonomy.

Terse, measurement-first. Numbers are bytes and each one names the board, the app and the
optimisation level that produced it. Where a figure is inferred rather than measured it says so.

---

## 0. Outline

1. The answer
2. What "measured" means here, and one trap
3. Per-board footprint, unoptimised and at `-Os`
4. Where the flash goes
5. Why `KICKOS_ENABLE_SELFTEST=OFF` produces a *larger* image
6. `syscall_dispatch` and `thread_spawn`
7. Where the RAM goes, and the real `bluepill-c8` / `f302nucleo` arena
8. The `stm32f103.ld` floor: a correction
9. Headroom has eroded this milestone, and by what
10. The two-board `-Os` block: the three options, measured
11. Recoverable versus inherent
12. Two configurations that do not build

**Relation to `TODO.md`.** This is not the first measurement of this. `TODO.md:75-91` already records
the N16 tiering measurement taken 2026-07-27, already identifies the `-Os` block as a holding
measure, and already concludes that "size-aware presets have dissolved the problem and tiering is
unnecessary for now" with `f302nucleo-st` at 49,604 bytes used / 15,932 free and `bluepill-c8-st` at
49,436 / 16,100. It leaves the N16 question as: keep the two-board block, widen it to the fleet, or
replace it with per-preset build types. **This record extends that**, in three ways it did not cover:
it measures the **non-selftest** images, which are the ones still unoptimised (section 3); it does
the RAM and arena attribution, which the optimisation level does not explain (section 7); and it
establishes that the record's own revisit trigger has fired (section 9).

---

## 1. The answer

**The kernel is not large. The as-shipped presets do not optimise it.**

Every MCU configure preset sets `CMAKE_BUILD_TYPE=Debug` (`cmake/presets/arm.json:10`, and the same
line in `rx.json`, `xtensa.json`, `riscv.json`). CMake's GNU `Debug` flags are `-g` with **no `-O`
flag at all**, so the whole tree compiles at `-O0`. Confirmed on the real link line, not inferred:

    arm-none-eabi-g++ -mcpu=cortex-m3 -mfloat-abi=soft -mthumb -mno-unaligned-access
      -ffunction-sections -fdata-sections -g -nostdlib++ -nostartfiles -Wl,--gc-sections ...

On `bluepill-c8`, a minimal production image (`hello`, `KICKOS_ENABLE_SELFTEST=OFF`) costs
**36,620 bytes of flash at `-O0` and 18,904 at `-Os`** -- a 48.4% reduction for a flag. At `-Os`
the KickOS side of that image (kernel + arch + chip + lib + user stubs + init glue + libgcc) is
**17,018 bytes**, or 26% of the part's 64 KiB. That is a normal size for a capability microkernel
with IPC, capability tables, a scheduler, an MPU backend, a console and a UART driver.

Two supporting negatives, both measured rather than assumed:

- **No non-kernel passengers on the default (freestanding) leaf.** `hello`, `hello_c`, `blink` and
  `selftest` contain **zero** newlib `printf`/`vfprintf`/`dtoa` symbols, **zero** `malloc`/`_sbrk`,
  **zero** `__cxa_*`, **zero** unwinder, no vtables and no RTTI. `.ARM.exidx` is **8 bytes**. Only
  `cxxtest`, which links the opt-in `kickos_cxx` leaf, pulls them (`_malloc_r` 1,412,
  `__gnu_unwind_execute` 776, `_Unwind_VRS_Pop` 764, `__gnu_unwind_pr_common` 664, `_free_r` 520).
- **No `.rodata` string mass.** All of `.rodata`, string literals included, is **1,546 bytes** on
  the `bluepill-c8` `-Os` `hello` image: 8.2% of it. Panic, assert and diagnostic literals are not
  a footprint problem.

**No board in the tree overflows at tip.** The two 64 KiB parts fit, but one shipped configuration
fits by under 1.4 KiB (section 3), and that thin margin is entirely an `-O0` artefact.

## 2. What "measured" means here, and one trap

- Fourteen real boards (non-sim, non-QEMU) configured directly rather than through the presets, so
  the build directory could live outside the worktree. Both `KICKOS_ENABLE_SELFTEST=OFF` and `ON`.
  All 28 configured and linked.
- Per-region usage is computed from the ELF section headers against the `MEMORY` block of the
  **generated** linker script in the build directory (`<build>/arch/<chip>.ld`), because the scripts
  are preprocessed through `cpp` before use (`arch/CMakeLists.txt:320-327`) and the source script is
  not what the link consumes. `.data` is counted against **both** regions: it is `AT > FLASH`, so it
  spends flash for its load copy and RAM for its run copy.
- Symbol sizes come from `arm-none-eabi-nm --print-size`; object and section attribution comes from
  `-Wl,-Map` with `--cref`, produced by re-running the exact link command with the map flags added.
- **Trap worth recording.** `nm --print-size` is only trustworthy for symbols that carry a `.size`,
  which compiler-emitted functions do and hand-written assembly labels do not. `nm` reports
  `g_isr_vector` as 20,484 bytes on `xmc4800-relax`; the link map says the `.isr_vector` input
  section is **0x200 = 512 bytes**, which matches 16 core vectors plus `KICKOS_MAX_IRQ = 112`. The
  map is authoritative for sections, `nm` for functions.

## 3. Per-board footprint, unoptimised and at `-Os`

A single number per board would hide the entire finding, so every cell is given twice: **D** is the
as-shipped configuration (`CMAKE_BUILD_TYPE=Debug`, i.e. `-g` with no `-O`), **Os** is the same tree
with `-DCMAKE_C_FLAGS_DEBUG="-g -Os" -DCMAKE_CXX_FLAGS_DEBUG="-g -Os"`. `hello` stands for a minimal
production image; `selftest` is the fleet-uniform verification image. Flash bytes.

| Board | `hello` D | `hello` Os | `selftest` OFF D | OFF Os | `selftest` ON D | ON Os |
| --- | --- | --- | --- | --- | --- | --- |
| `bluepill-c8` | 36,620 | 18,904 | **64,208** | 46,932 | 51,540 | 51,548 |
| `f302nucleo` | 36,812 | 19,076 | **64,408** | 47,120 | 51,716 | 51,716 |
| `microbit` | 35,516 | 18,708 | 63,764 | 47,424 | 68,888 | 52,160 |
| `blackpill` | 37,108 | 19,288 | 64,704 | 47,332 | 69,632 | 51,928 |
| `f411disco` | 37,100 | 19,288 | 64,696 | 47,332 | 69,624 | 51,928 |
| `due` | 36,668 | 18,944 | 64,256 | 46,972 | 69,192 | 51,596 |
| `frdmk64f` | 38,488 | 19,976 | 66,076 | 48,004 | 71,012 | 52,608 |
| `picopi` | 38,560 | 20,144 | 66,744 | 48,860 | 72,080 | *fails, sec. 12* |
| `xmc4800-relax` | 39,780 | 20,676 | 67,368 | 48,688 | 72,304 | 53,300 |
| `rx72m` | 39,636 | 21,732 | 67,776 | 50,344 | 72,848 | 55,128 |
| `teensy41` | 38,368 | 20,692 | 65,960 | 48,724 | 70,936 | 53,388 |
| `pizero2350` | 36,536 | 19,084 | 64,124 | 47,112 | 69,280 | *fails, sec. 12* |
| `esp32-wroom` | 40,308 | 22,996 | 69,128 | 52,516 | 74,532 | 57,568 |
| `esp32c6-wroom` | *no flash region -- see below* | | | | | |

`bluepill-c8` and `f302nucleo` are the only boards whose `ON` column is already optimised: the `-Os`
block of section 5 covers exactly those two, which is why their `ON D` and `ON Os` figures agree to
within 8 bytes. **Every other cell in a D column is `-O0`.**

Region budgets, RAM and the headroom class. Flash free is quoted for the **worst** shipped
configuration of that board (the `selftest` image, whichever setting is larger); RAM is quoted for
`hello` with `KICKOS_ENABLE_SELFTEST=OFF`.

| Board | Flash avail | Worst flash free, D | Worst flash free, Os | RAM used / avail | Class |
| --- | --- | --- | --- | --- | --- |
| `bluepill-c8` | 65,536 | **1,328** | 18,604 | 11,860 / 20,480 | **fits thinly** |
| `f302nucleo` | 65,536 | **1,128** | 18,416 | 7,884 / 16,384 | **fits thinly** |
| `microbit` | 262,144 | 193,256 | 209,984 | 2,852 / 16,384 | comfortable |
| `blackpill` | 524,288 | 454,656 | 472,360 | 15,076 / 131,072 | comfortable |
| `f411disco` | 524,288 | 454,664 | 472,360 | 15,076 / 131,072 | comfortable |
| `due` | 524,288 | 455,096 | 472,692 | 22,940 / 98,304 | comfortable |
| `frdmk64f` | 1,048,576 | 977,564 | 995,968 | 26,860 / 262,144 | comfortable |
| `picopi` | 2,097,152 | 2,025,072 | -- | 26,412 / 270,336 | comfortable |
| `xmc4800-relax` | 2,097,152 | 2,024,848 | 2,043,852 | 28,700 / 131,072 | comfortable |
| `rx72m` | 4,194,176 | 4,121,328 | 4,139,048 | 27,824 / 524,288 | comfortable |
| `teensy41` | 8,388,608 | 8,317,672 | 8,335,220 | 27,436 / 524,288 | comfortable |
| `pizero2350` | 16,777,216 | 16,707,936 | -- | 26,572 / 524,288 | comfortable |
| `esp32-wroom` | 131,072 IRAM | 56,540 | 73,504 | 28,388 / 196,608 DRAM | comfortable |
| `esp32c6-wroom` | none | -- | -- | 111,764 / 524,288 | comfortable |

- **Overflows: none.** No board and no configuration in the tree fails to link at tip.
- **Fits thinly:** exactly two cells, and both are `-O0` cells -- `bluepill-c8` `selftest` with the
  flag OFF, **1,328 bytes free**, and `f302nucleo` in the same configuration, **1,128 bytes free**.
  Any small addition tips either over. At `-Os` those same two builds have 18,604 and 18,416 free.
- **Fits comfortably:** everything else, minimum 56,540 bytes free (`esp32-wroom` IRAM).
- `esp32c6-wroom` has a single `RAM` region and no flash region in its linker script: code and data
  both live in the 512 KiB RAM, so it has no flash budget to overflow. Its `hello` image occupies
  72,256 bytes of that RAM at `-O0` and 49,112 at `-Os`, the largest single-board recovery measured.
- The RAM column, not the flash column, is where the two 64 KiB boards are genuinely tight -- and
  that is **not** explained by the optimisation level: `bluepill-c8` `hello` RAM is 11,860 at `-O0`
  and 11,844 at `-Os`, a 16-byte difference. Section 7 accounts for it.

The `~72 KiB selftest` figure recorded at `user/apps/common/selftest/CMakeLists.txt:19` is real but
board-specific: at tip it is `xmc4800-relax` 72,304, `rx72m` 72,848, `picopi` 72,080 -- enforcing
boards with `KICKOS_ENABLE_SELFTEST=ON` at `-O0`. On the two 64 KiB parts the same suite is smaller
because the MPU-gated cases compile out. With **both** `-Os` applications removed (section 5), so
that the app compiles at `-O0` too, the suite measures 74,676 (`blackpill`, OFF) and 81,732
(`blackpill`, ON), and it does overflow both 64 KiB parts: by 9,004 and 16,116 bytes on
`bluepill-c8`, by 9,204 and 16,308 on `f302nucleo`.

## 4. Where the flash goes

`bluepill-c8`, `hello`, `KICKOS_ENABLE_SELFTEST=OFF`, `-Os`. Flash-resident bytes from the link
map; total 18,862 (the 42-byte difference from the ELF's 18,904 is inter-section padding).

| Subsystem | Bytes | Share |
| --- | --- | --- |
| kernel (`libkickos_kernel.a`) | 12,496 | 66.2% |
| app (`main.cc.obj`) | 1,844 | 9.8% |
| arch backend (`libkickos_arch_armv7m.a`) | 1,370 | 7.3% |
| chip driver (`libkickos_chip_stm32f103.a`) | 1,158 | 6.1% |
| libgcc | 852 | 4.5% |
| lib (`libkickos_lib.a`) | 642 | 3.4% |
| service / init glue | 288 | 1.5% |
| user syscall stubs | 212 | 1.1% |

The same breakdown on `xmc4800-relax` (MPU-enforcing, `hello`, `-Os`, total 20,605) is kernel
12,582 (61.1%), chip driver 2,510, app 1,869, arch 1,482, libgcc 860, lib 804, glue 286, stubs 212.
**The kernel core is flat across boards at roughly 12.5 KiB** -- that is the kernel, not a
per-board accident.

Largest objects, `bluepill-c8` `-Os`:

| Bytes | Object |
| --- | --- |
| 1,638 | `libkickos_kernel.a(cap.cc.obj)` |
| 1,508 | `libkickos_kernel.a(syscall_ipc.cc.obj)` |
| 1,264 | `libkickos_kernel.a(syscall.cc.obj)` |
| 1,024 | `libkickos_kernel.a(syscall_thread.cc.obj)` |
| 920 | `libkickos_chip_stm32f103.a(chip_stm32f103.cc.obj)` |
| 908 | `libkickos_kernel.a(sync.cc.obj)` |
| 800 | `libgcc.a(_udivmoddi4.o)` |
| 762 | `libkickos_kernel.a(sched.cc.obj)` |
| 732 | `libkickos_kernel.a(kmain.cc.obj)` |
| 676 | `libkickos_kernel.a(domain.cc.obj)` |
| 624 | `libkickos_kernel.a(console_tx.cc.obj)` |
| 612 | `libkickos_lib.a(fmt.cc.obj)` |
| 588 | `libkickos_kernel.a(irq.cc.obj)` |
| 556 | `libkickos_kernel.a(console.cc.obj)` |
| 516 | `libkickos_arch_armv7m.a(arch_armv7m.cc.obj)` |

Largest individual symbols, same image:

| Bytes | Symbol |
| --- | --- |
| 1,184 | `syscall_dispatch` |
| 984 | `kickos::thread_spawn(kos_thread_params const*)` |
| 792 | `__udivmoddi4` |
| 460 | `kvsnprintf` |
| 456 | `kickos::endpoint_call(int, unsigned, unsigned, unsigned)` |
| 444 | `kickos::kmain(int, char**)` |
| 432 | `kickos::thread_create(...)` |
| 412 | `kickos::endpoint_recv(int, unsigned, unsigned, unsigned)` |
| 376 | `kickos::domain_for(bool, void*, unsigned, void*, unsigned, bool, int*)` |
| 332 | `arch_init` |
| 312 | `kickos::(anonymous namespace)::obj_ref_drop(kickos::CapEntry const&, bool)` |
| 240 | `g_isr_vector` (16 core vectors + `KICKOS_MAX_IRQ = 43`) |
| 224 | `arch_timer_arm` |
| 220 | `kickos::endpoint_send(int, unsigned, unsigned)` |

`libgcc.a(_udivmoddi4.o)` at 800 bytes -- 4.2% of the whole image -- is the only toolchain
passenger. Its callers, found by disassembly, are three 64-bit divisions on a 32-bit Cortex-M3:
`arch_init`, `arch_timer_arm` (twice), and the `emit_uint` path in `libkickos_lib.a(fmt.cc.obj)`.

In the `selftest` image the attribution inverts: `main.cc.obj` alone is **28,899 bytes, 62.9%** of
the 46,877-byte `-Os` image, and the kernel-side figures are byte-identical to the `hello` build
above (kernel 12,496, arch 1,370, chip 1,158, libgcc 852, glue 288). The selftest image is a test
app carried on the same kernel, not a bigger kernel.

## 5. Why `KICKOS_ENABLE_SELFTEST=OFF` produces a *larger* image

Fully explained. The flag is not adding tests to a fixed image; on two boards it selects the
optimisation level for the **entire tree**.

There are **two** independent `-Os` applications in the build, and one of them is conditional on
the flag:

1. `user/apps/common/selftest/CMakeLists.txt:26` -- `target_compile_options(selftest PRIVATE -Os)`.
   Unconditional on every MCU board, and it covers only the selftest app's own TUs.
2. `CMakeLists.txt:137-145`, the N16 holding measure -- `add_compile_options(-Os)` when
   `KICKOS_ENABLE_SELFTEST` is on **and** the board is `f302nucleo` or `bluepill-c8`. Directory-wide:
   it covers the kernel, the arch backend, the chip driver, the libs and the app.

So on exactly those two boards, `KICKOS_ENABLE_SELFTEST=ON` means `-Os` everywhere and `OFF` means
`-O0` everywhere. The measured swing on `bluepill-c8` `hello` -- an app containing no test code
whatsoever -- is **36,620 with the flag OFF against 19,052 with it ON, 17,568 bytes**.

Four independent confirmations:

- **Flag asymmetry on the command line.** The OFF build's compile and link lines carry `-g` with no
  `-O`; the ON build's carry `-g -Os`.
- **Reproduction from the OFF side.** Adding `-DCMAKE_C_FLAGS_DEBUG="-g -Os"
  -DCMAKE_CXX_FLAGS_DEBUG="-g -Os"` to the OFF configure gives `hello` = **18,904**, against the ON
  build's 19,052. The 148-byte residual is what the flag genuinely adds.
- **Removing the inversion.** With the N16 block disabled (throwaway edit, reverted), `bluepill-c8`
  `hello` measures **36,996 with the flag ON against 36,620 with it OFF**. The ON image is now the
  larger one, by 376 bytes of selftest code, which is the expected direction.
- **Symbol-set difference, which rules out the obvious suspects.** `hello` has 327 sized symbols in
  the OFF build and 232 in the ON build.
  - Symbols present **only in OFF** total **6,844 bytes**, and every one is an ordinary KickOS
    internal that `-Os` inlined into its caller: `clock_init` 332, `kickos::arch_clk_mul_q32` 320,
    `kbanner` 256, `timer_clock_init` 256, `mutex_ref_drop` 212, `ThreadPool::alloc` 198,
    `dev_window_free` 190, `sem_ref_drop` 188, `switch_to` 136, `usart1_init` 132,
    `ThreadPool::index_of` 108, `ThreadPool::release` 104, `sleepq_insert` 90, `List::unlink` 88,
    `sleepq_remove` 88, `rq_remove` 88.
  - Symbols present **only in ON** total **94 bytes**, and they are exactly the selftest-gated
    additions: `arch_irq_inject` 28, `irq_spurious_count` 12, `arch_reboot` 6,
    `arch_mpu_probe_addr` 4, plus a 44-byte `u32_dec` constprop clone.
  - Across the **241 symbols present in both**, 213 shrank, 13 grew, 15 were unchanged, and the net
    is **11,344 bytes** smaller in the ON build.
  - So the swing is `6,844 + 11,344` gross against `94` plus the growers: **diffuse shrinkage across
    essentially every function**, not a handful of named symbols.
  - It is **not** a libc, formatting, iostream, unwinder or C++-runtime path, and **not** a different
    default init or app path: both builds contain zero `printf`, `malloc`, `_sbrk`, `__cxa_*` and
    unwinder symbols, and the OFF-only set contains no toolchain symbol at all.

Nothing remains unknown about this inversion.

## 6. `syscall_dispatch` and `thread_spawn`

Same cause. The two configurations are `bluepill-c8` at the as-shipped `Debug` default (`-g`, no
`-O`, i.e. `-O0`) and the same tree at `-Os`. The figures are **identical in `hello` and in
`selftest`**, which is what proves they have nothing to do with test content.

| Symbol | `-O0` | `-Os`, selftest OFF | `-Os`, selftest ON |
| --- | --- | --- | --- |
| `syscall_dispatch` | 2,422 | 1,184 | 1,278 |
| `kickos::thread_spawn` | 2,148 | 984 | 984 |

Together they are 4,570 bytes at `-O0`, which is 7.0% of a 64 KiB flash -- the "about a tenth of
the budget" observation. At `-Os` they are 2,168 bytes, 3.3%.

`syscall_dispatch` lowering, from disassembly of both builds:

- **`-Os`:** a real jump table. `e8df f010  tbh [pc, r0, lsl #1]` at `0x08001cf2`, followed
  immediately by a 38-entry halfword table. The table is **inline in `.text`** (76 bytes), not in
  `.rodata`, so it is counted where it is spent. 460 disassembly lines. `IrqLock` is fully inlined
  to bare `arch_irq_save`/`arch_irq_restore` calls (9 and 4 sites).
- **`-O0`:** **no jump table at all.** A linear chain of `cmp rN, #imm` plus `beq.w` per case, with
  compares duplicated (`cmp r3, #37` is emitted twice), 918 disassembly lines, and a **184-byte
  stack frame** (`sub sp, #184`). `IrqLock`'s constructor and destructor are called out of line 9
  times each.

The switch has 38 `case KOS_SYS_*` labels (counted in `kernel/syscall/syscall.cc`). At `-Os` that is
1,184 bytes for 38 cases plus a 76-byte table, about 29 bytes per case, and no callee is being
inlined into it wholesale. Nothing there looks trimmable without removing syscalls.

`thread_spawn`: 735 disassembly lines at `-O0` against 358 at `-Os`. The `-O0` build performs the
`kos_thread_params` copy byte-wise (40 `ldrb`/`strb` against 30 at `-Os`) and reaches its callees
through un-inlined accessors (`ThreadPool::handle_for`, `IrqLock` constructor and destructor) that
vanish at `-Os`. No template is instantiated per call site; the `SlotPool<T, N>` instantiations
are three, shared across the image, and total 252 bytes.

## 7. Where the RAM goes, and the real `bluepill-c8` / `f302nucleo` arena

A measurement note first, because it changes the reading of every `size` output: `arm-none-eabi-size`
counts `.userheap` in the `bss` column, because that section is `ALLOC`/`NOBITS`. The 11,800-byte
`bss` of `bluepill-c8` `hello` is **3,604 bytes of real static `.bss` plus an 8,196-byte
`.userheap` carve**. The static footprint is a quarter of what the column suggests.

Static RAM (`.bss` + `.data`, from the link map), `-Os`:

| Subsystem | `bluepill-c8` hello | `bluepill-c8` selftest | `f302nucleo` hello | `f302nucleo` selftest |
| --- | --- | --- | --- | --- |
| kernel | 3,019 | 3,019 | 3,139 | 3,139 |
| app + TAP harness | 28 | 3,905 | 28 | 3,905 |
| chip driver | 548 | 548 | 552 | 552 |
| arch backend | 36 | 36 | 36 | 36 |
| user stubs | 8 | 8 | 8 | 8 |
| **total** | **3,639** | **7,516** | **3,763** | **7,640** |

Largest static objects, `bluepill-c8` `-Os`:

| Bytes | Object |
| --- | --- |
| 8,192 | `.userheap` (`_kickos_heap_start`), reserved by the linker script |
| 2,328 | `kickos::detail::g_instance` -- the kernel singleton (handle and object tables) |
| 2,048 | `g_cstk_static` -- selftest's own caller-stack buffer (selftest image only) |
| 1,024 | `tap::g_tests` (selftest image only) |
| 512 | `console_tx_buf` -- the chip console ring |
| 320 + 320 | `g_root_tcb` + `g_idle_tcb` in `kmain.cc` |
| 192 | `tap::g_msg` (selftest image only) |
| 128 | `g_log` (selftest image only) |

**The kernel's static tables are already sized per board, and the small boards already take the
discount.** `boards/bluepill-c8/include/kickos/board_config.h` sets `KICKOS_MAX_THREADS 2`,
`KICKOS_MAX_HANDLES 9`, `KICKOS_MAX_MUTEXES 4`, `KICKOS_USER_STACK_SIZE 2048`. `f302nucleo` has no
board include directory, so the resolver falls back to the chip's
(`CMakeLists.txt:55-57`), and `arch/arm/chip/stm32f302/include/kickos/board_config.h` provides the
same discipline: `MAX_THREADS 2`, `MAX_HANDLES 9`, `MAX_SEMAPHORES 4`, `USER_STACK_SIZE 2048`.

Measured sensitivity, `bluepill-c8` `hello` `-Os`, rebuilt with the fleet defaults
(`-DKICKOS_MAX_THREADS=16 -DKICKOS_MAX_HANDLES=12 -DKICKOS_MAX_SEMAPHORES=16`):

| Provisioning | `g_instance` | `.bss` (incl. heap carve) |
| --- | --- | --- |
| board values (as shipped) | 2,328 | 11,784 |
| fleet defaults | 8,672 | 18,176 |

So per-board provisioning **already recovers 6,344 bytes of SRAM** on this part -- enough that at
the fleet defaults the selftest arena would be negative and the link would fail its
`__kickos_ram_start <= __kickos_ram_end` assert. There is no fleet-uniform constant that a 20 KiB
part is currently over-paying for. This is a solved problem, recorded here so it is not re-solved.

### `g_instance` broken down by the maxima that size it

One knob varied at a time from the board values, `bluepill-c8`, `MinSizeRel`:

| Knob | Value | `g_instance` | Delta | Marginal cost |
| --- | --- | --- | --- | --- |
| (board values) | `MAX_THREADS 2`, `MAX_HANDLES 9` | 2,328 | -- | -- |
| `KICKOS_MAX_THREADS` | 4 | 3,176 | +848 | **424 bytes per thread** |
| `KICKOS_MAX_THREADS` | 16 | 8,288 | +5,960 | 426 bytes per thread |
| `KICKOS_MAX_HANDLES` | 12 | 2,376 | +48 | **16 bytes per handle** |
| `KICKOS_MAX_HANDLES` | 16 | 2,440 | +112 | 16 bytes per handle |
| `KICKOS_MAX_SEMAPHORES` | 16 | 2,328 | 0 | (already the effective value) |

`KICKOS_MAX_THREADS` dominates by an order of magnitude. Its marginal cost bundles two things,
because `KICKOS_MAX_DOMAINS` defaults to `KICKOS_MAX_THREADS + 2`
(`kernel/include/kickos/config/system.h:60-62`): each additional thread buys a thread-pool slot and a
domain slot together. The handle-table cost matches the arithmetic the header itself documents at
`system.h:45` -- `MAX_THREADS x MAX_HANDLES x 8 bytes`, so 16 bytes per handle at `MAX_THREADS 2`.

Both 64 KiB boards are already at the floor the full selftest allows: `MAX_HANDLES 9` is the
documented minimum for the suite (`system.h:49-53`), and `MAX_THREADS 2` cannot go lower because root
and idle both need a slot. **A per-board table sizing would recover zero further bytes on either
board.** What is left is the heap carve (R4 in section 11), which is the larger consumer anyway:
8,192 bytes against `g_instance`'s 2,328.

### Arena arithmetic

`arena = SRAM - _kernel_stack_size - .userheap - .data - .bss(static) - alignment`, and the arena is
the span `[__kickos_ram_start, __kickos_ram_end)`. Both symbols read out of the ELF.

**`bluepill-c8`** -- 20,480 SRAM (`_estack = 0x2000_5000`, exactly 20 KiB), `_kernel_stack_size = 2K`,
`.userheap` default 8K (`boards/bluepill-c8/stm32f103.ld:27`), `__kickos_ram_end = 0x2000_4800`:

| Image | Opt | Heap | `__kickos_ram_start` | Arena |
| --- | --- | --- | --- | --- |
| `hello` | `-O0` | 8K | `0x2000_2e60` | **6,560** |
| `hello` | `-Os` | 8K | `0x2000_2e60` | **6,560** |
| `selftest` OFF | `-O0` kernel | 8K | `0x2000_3d80` | **2,688** |
| `selftest` ON | `-Os` | 8K | `0x2000_3de0` | **2,592** |
| `hello` | `-Os` | 0 | `0x2000_0e60` | **14,752** |
| `selftest` ON | `-Os` | 0 | `0x2000_1de0` | **10,784** |

**`f302nucleo`** -- 16,384 SRAM (`_estack = 0x2000_4000`), `_kernel_stack_size = 2K`, `.userheap`
default 4K (`arch/arm/chip/stm32f302/stm32f302.ld:25`), `__kickos_ram_end = 0x2000_3800`:

| Image | Opt | Heap | `__kickos_ram_start` | Arena |
| --- | --- | --- | --- | --- |
| `hello` | `-Os` | 4K | `0x2000_1ec0` | **6,464** |
| `selftest` ON | `-Os` | 4K | `0x2000_2e00` | **2,560** |

The two boards are **not** the same case -- 20 KiB against 16 KiB of SRAM -- but they land within
128 bytes of each other because `f302nucleo`'s smaller SRAM is matched by a smaller heap carve
(4K against 8K).

**The "barely 3 KiB of arena" figure is accurate, and it is a selftest-image figure.** For the
selftest image it is 2,592 bytes on `bluepill-c8` and 2,560 on `f302nucleo`. For a production image
it is 6,560 and 6,464. Its dominant consumer is **not** the kernel: on `bluepill-c8` `selftest`, of
the 17,888 bytes the arena does not get, `.userheap` takes 8,192 (46%), the selftest app and TAP
harness take 3,905 (22%), the kernel stack takes 2,048 (11%), and the kernel's own static state
takes 3,019 (17%).

## 8. The `stm32f103.ld` floor: a correction

The floor is real: `arch/arm/chip/stm32f103/stm32f103.ld:22-23` declares `FLASH 32K` / `RAM 10K`,
sized down for the low-density Blue Pill clones.

**It penalises nothing, because no board reaches it.** `arch/CMakeLists.txt:291-293` prefers
`boards/<board>/<chip>.ld` when that file exists, and `boards/bluepill-c8/stm32f103.ld:17-18`
declares the honest `FLASH 64K` / `RAM 20K`. `bluepill-c8` is the only board in the tree with
`KICKOS_CHIP "stm32f103"` (checked across all of `boards/*/board.cmake`), and it has that override.
Confirmed in the linked artefact rather than from the sources: the generated script the link
actually consumes (`<build>/arch/stm32f103.ld`) carries `LENGTH = 64K` / `LENGTH = 20K`, and the ELF
places `_estack` at `0x2000_5000`, which is `0x2000_0000 + 20480`.

So the chip default is currently reachable by no board: it is unused defensive code left behind by
the clone's retirement (`e562eef`, "board cleanups"). **Removing the floor recovers zero bytes on
any board that exists.** No test, CI job or script consumes it either -- the only in-tree reference
to it is the explanatory comment at `boards/bluepill-c8/board.cmake:9`.

The corollary matters more than the floor: the small `bluepill-c8` arena is **not** a floor artefact.
The linker sees the full 20 KiB. The arena is small because of the 8 KiB heap carve and the static
footprint (section 7).

**Does the C8 have a footprint problem at all?** No, once the image is compiled at `-Os`:

| `bluepill-c8` at `-Os` | Flash used / 65,536 | Flash free | Arena |
| --- | --- | --- | --- |
| production image (`hello`) | 18,904 | 46,632 (71%) | 6,560 |
| full selftest, flag OFF | 46,932 | 18,604 | 2,688 |
| full selftest, flag ON | 51,540 | 13,996 | 2,592 |
| production image, heap 0 | 18,904 | 46,632 | 14,752 |

The part is comfortable. The only tight configuration in the tree is the one that compiles the
kernel at `-O0`.

## 9. Headroom has eroded this milestone, and by what

`TODO.md:88-90` set the revisit trigger explicitly: "Revisit only if headroom actually erodes." It
has. Measured by building the M4.5.2 baseline `181540e` (the M4.5.1 merge, and this branch's
merge-base) from a read-only `git archive` extraction, with the same toolchain, the same flags and
the same method as tip, so the two figures are directly comparable:

| Build | `bluepill-c8-st` flash | free | `f302nucleo-st` flash | free |
| --- | --- | --- | --- | --- |
| `181540e` (M4.5.1 merge) | 49,904 | 15,632 | 50,072 | 15,464 |
| `ae7996e` (tip) | 51,540 | 13,996 | 51,716 | 13,820 |
| **consumed** | **+1,636** | -1,636 | **+1,644** | -1,644 |

Headroom on `bluepill-c8-st` is now **21.4%** of the part, against the 24.6% `TODO.md` records.
(`TODO.md`'s absolute figure, 49,436, is 468 bytes below the `181540e` build above; that measurement
was taken 2026-07-27, before the M4.5.1 audit-fix and comment-cleanup commits landed, so it reflects
a slightly earlier tree. The delta is what matters and it is reproducible.)

Attribution of the 1,636 bytes on `bluepill-c8`, from a symbol-level diff of the two `selftest` ELFs:

| Bytes | Cause |
| --- | --- |
| 672 | `t_bus_device_slots` -- the new selftest case (`6c0e56a`) |
| 280 | its helpers: `slot_client` 120, `slot_xfer` 80, `slot_config` 80 |
| 152 | `t_reboot_denied` 128 + `reboot_denied_worker` 24 (`8be5e2c`) |
| 88 | `kickos::domain_for` growth -- the one-holder-per-MMIO-window check (`68fdda3`) |
| 52 | `kos_reboot` 20, `mem_copy` 18, `kickos_terminate` 14 (`040bf01`) |
| 32 | `kickos_app_main` growth -- the new `tap::add` registrations |
| 22 | `syscall_dispatch` growth -- the `KOS_SYS_REBOOT` case |
| 1,298 | **subtotal, named symbols** |
| ~338 | residual: `.rodata` string literals for the new test names and messages (which carry no sized symbol), `.data` +24, and alignment |

**No symbol was removed** between the two trees, and net growth across the 1,000-plus symbols present
in both is only **+138 bytes**. So the erosion is not drift: it is **1,104 bytes of new selftest
content** plus **110 bytes of genuine kernel growth** (`domain_for` +88, `syscall_dispatch` +22).

RAM eroded too, and here the cause is a single knob. `.bss` on `bluepill-c8-st` `selftest` went from
14,920 to 15,448, **+528 bytes**, of which **512 is `09e7133` raising `MAX_TESTS` from 64 to 128** in
`tests/tap/tap.cc:19` (`Entry g_tests[MAX_TESTS]` at 8 bytes per entry, so 512 -> 1,024). The
remaining 16 bytes is the new test's own state. That commit's message notes the registry "costs no
production RAM" because `tap` links only into test images, which is correct -- the cost lands
entirely on the arena of the selftest image, which is the tightest arena in the tree (section 7).

## 10. The two-board `-Os` block: the three options, measured

`TODO.md:91` frames the remaining N16 question as three options. Each one's consequence is measurable
and was measured.

First, a fact that inverts the block's own stated cost. `CMakeLists.txt:139-140` gives the cost of the
block as "the `-st` kernel is no longer codegen-identical to the same board's non-st build". Measured
kernel-side flash on `bluepill-c8` `hello`:

| Configuration | kernel `.text` + `.rodata` |
| --- | --- |
| as shipped: non-st (`-O0`) versus `-st` (`-Os`) | 26,115 versus 12,610 -- **differ by 13,505** |
| both at `-Os`: non-st versus `-st` | 12,496 versus 12,610 -- **differ by 114** |

The 114 bytes are the flag's genuine content (`arch_reboot`, `arch_irq_inject`,
`arch_mpu_probe_addr`, `irq_spurious_count`). So the block does not *create* the codegen divergence
it warns about -- **the unoptimised default does**, and widening `-Os` is what would nearly close it.
This matters because silicon witnesses are taken on `-st` images: today a fault address or a
disassembly offset from an `-st` witness does not transfer to the shipped non-st image at all.

**Option A -- keep the two-board block.** Costs nothing to leave alone. Leaves twelve of fourteen
boards at `-O0` in every configuration, and both 64 KiB boards at `-O0` in their non-selftest
configuration, which is the shipped one. Keeps the two thin cells of section 3 (1,328 and 1,128 bytes
free) and keeps the 13,619-byte codegen divergence above. Section 9 shows the margin those cells sit
on is now shrinking at roughly 1.6 KiB per milestone, so this option has a measurable expiry.

**Option B -- widen `-Os` to the fleet.** Recovers 16,808 to 23,144 bytes per board on a production
image (the table in section 11), reduces the codegen divergence to 114 bytes, and lifts the worst
shipped free-flash figure on the two tight boards from 1,328 to 18,604. **Blocked today:** it does
not compile on `picopi` or `pizero2350` with the selftest flag on (section 12), so that bug is a
prerequisite, not an afterthought. `TODO.md:293` additionally argues `-O0` inflates "every switch's
I-cache footprint"; that is plausible and this record did not measure it -- no instruction-cache or
cycle measurement was taken, so it is **unverified here** and should not be cited as measured.

**Option C -- per-preset build types.** The obvious objection to a real build type is that
`MinSizeRel`, `Release` and `RelWithDebInfo` all add `-DNDEBUG`. **That objection does not apply to
this tree:** there is no `NDEBUG` reference anywhere in it and no `assert()` in `kernel/`, `arch/`,
`lib/` or `user/` -- the internal-consistency guards are gated on the project's own `KICKOS_DEBUG`
knob (`kernel/include/kickos/debug.h:14-15`), which is independent. Verified by building
`CMAKE_BUILD_TYPE=MinSizeRel`: it links on both settings and is byte-identical in `.text` to the
explicit `-Os` variant (`hello` 18,836 OFF / 18,984 ON; `selftest` 46,600 OFF / 51,152 ON). Its only
real cost is that `MinSizeRel` drops `-g`, recoverable with
`-DCMAKE_C_FLAGS_MINSIZEREL="-Os -DNDEBUG -g"`.

**Recommendation: option C, gated on fixing the RP2040/RP2350 compile first.** It expresses the
intent where CMake already expresses it, applies uniformly so no board is a special case, removes
the two-board conditional that produced the inversion of section 5 in the first place, and needs no
new mechanism. Option B reaches the same footprint but leaves the size policy encoded as a
board-name test in the top-level `CMakeLists.txt`, which is what made a test-only flag change the
optimisation level of the whole tree. Either way the prerequisite is the same, and until it is fixed
neither option can be applied fleet-wide.

## 11. Recoverable versus inherent

### Recoverable, each measured

**R1 -- `-O0` to `-Os`, whole tree.** The single largest item by an order of magnitude. Measured on
`hello` (flash bytes saved), ranked:

| Board | `-O0` | `-Os` | Recovered |
| --- | --- | --- | --- |
| `esp32c6-wroom` | 72,256 (RAM) | 49,112 | **23,144** |
| `xmc4800-relax` | 39,780 | 20,676 | **19,104** |
| `frdmk64f` | 38,488 | 19,976 | **18,512** |
| `picopi` | 38,560 | 20,144 | **18,416** |
| `rx72m` | 39,636 | 21,732 | **17,904** |
| `blackpill` | 37,108 | 19,288 | **17,820** |
| `f411disco` | 37,100 | 19,288 | **17,812** |
| `f302nucleo` | 36,812 | 19,076 | **17,736** |
| `due` | 36,668 | 18,944 | **17,724** |
| `bluepill-c8` | 36,620 | 18,904 | **17,716** |
| `teensy41` | 38,368 | 20,692 | **17,676** |
| `pizero2350` | 36,536 | 19,084 | **17,452** |
| `esp32-wroom` | 40,308 | 22,996 | **17,312** |
| `microbit` | 35,516 | 18,708 | **16,808** |

On the `selftest` image with the flag OFF the recovery is 17,276 (`bluepill-c8`), 17,288
(`f302nucleo`), 17,372 (`blackpill`), 16,340 (`microbit`). On `bluepill-c8` and `f302nucleo` with
the flag ON it is already taken by the N16 block (measured delta 0 and -8 bytes: the N16 `-Os` and an
explicit `-Os` produce the same image). **Every other board, and every non-selftest image on all
fourteen, is still at `-O0`.**

**R2 -- `-Os`, not `-O2`.** `bluepill-c8` `hello`: 18,904 at `-Os` against 21,620 at `-O2`, so `-O2`
gives back **2,716 bytes** of the recovery; on `selftest` OFF it is 46,932 against 49,768, i.e.
**4,324 bytes**. The recovery has to be `-Os` on the tight boards.

**R3 -- the 64-bit division helper, 800 bytes** on `bluepill-c8` (`libgcc.a(_udivmoddi4.o)`, 4.2% of
the `-Os` `hello` image; 860 on `xmc4800-relax`). Three call sites, all reached by disassembly:
`arch_init`, `arch_timer_arm` (twice) and `emit_uint` in `libkickos_lib.a(fmt.cc.obj)`. Recoverable
only by changing those three to 32-bit arithmetic or shifts; not measured as a delta, because that
needs a code change rather than a flag.

**R4 -- the `.userheap` carve, 8,192 bytes of SRAM on `bluepill-c8`**, 40% of the part's RAM.
Measured with `-DKICKOS_USER_HEAP_SIZE=0`: RAM used falls from 11,844 to 3,652 on `hello`, and the
arena rises from 6,560 to 14,752 (from 2,592 to 10,784 on the selftest image). No app on the
freestanding leaf references it -- `hello`, `hello_c`, `blink` and `selftest` contain zero
`malloc`/`_sbrk` symbols, and only `cxxtest` on the `kickos_cxx` leaf does. This is a **policy cost,
not waste**: user-facing apps are expected to be able to use `printf`/`std::cout`, which needs a
heap. It is recoverable per board through the existing `KICKOS_USER_HEAP_SIZE` knob, at the price of
that guarantee.

### Inherent

**I1 -- the kernel core, 12,496 bytes at `-Os`** on `bluepill-c8` and 12,582 on `xmc4800-relax`.
Flat across boards and identical between the `hello` and `selftest` images. This is the microkernel.

**I2 -- `.rodata` including every string literal, 1,546 bytes**, 8.2% of the `-Os` `hello` image.
Not a mass; nothing to reclaim.

**I3 -- the C++ runtime, zero.** `.ARM.exidx` is 8 bytes; no `__cxa_*`, no vtable, no RTTI, no
unwinder on the freestanding leaf. `-ffreestanding -fno-common -fno-exceptions -fno-rtti
-fno-threadsafe-statics -fno-use-cxa-atexit` are confirmed on the actual compile line of every
kernel, arch, chip and lib TU (`cmake/kickos.cmake:112-118`), and `-nostdlib++` on the link.

**I4 -- newlib, zero on the default leaf.** The cost only appears on the opt-in `kickos_cxx` leaf,
where `cxxtest` pulls `_malloc_r` 1,412, `__gnu_unwind_execute` 776, `_Unwind_VRS_Pop` 764,
`__gnu_unwind_pr_common` 664, `_free_r` 520, `__cxa_call_unexpected` 220 and the rest of the ARM EH
machinery.

**I5 -- section GC is already uniform.** `-ffunction-sections -fdata-sections` come from
`cmake/toolchain-arm-none-eabi.cmake` for every ARM board and `-Wl,--gc-sections` from
`CMakeLists.txt:617` for every non-sim board. Verified on the real compile and link lines; they do
not differ between boards or between the two selftest settings. No recovery here.

**I6 -- kernel static state, 3,019 bytes** on `bluepill-c8` (3,139 on `f302nucleo`), already sized
by that board's own provisioning; see the 6,344-byte measurement in section 7.

**I7 -- the vector table**, 240 bytes on `bluepill-c8` (16 core + `KICKOS_MAX_IRQ = 43`), 512 on
`xmc4800-relax` (16 + 112). Chip facts.

### What is left once the build is optimised

**Very little, and it should be said plainly.** Beyond the optimisation level, the whole recoverable
set on the tightest board is **800 bytes of flash** (R3, the 64-bit division helper, and it needs a
code change in three call sites) and **8,192 bytes of SRAM** (R4, the heap carve, and giving it up
costs the `printf`/`std::cout` guarantee that user-facing apps are written against). Everything else
enumerated above is either already taken -- per-board table sizing, section GC, the freestanding
clamp -- or is the kernel itself.

The `-Os` production image on `bluepill-c8` is **18,904 bytes against 65,536, with 46,632 free**.
There is no kernel footprint problem on this board class to go hunting for. The two facts worth
carrying forward are that the shipped presets do not optimise (section 5) and that the tightest
arena in the tree is a RAM-policy question, not a code-size one (section 7).

## 12. Two configurations that do not build

Both found by trying to take the R1 recovery, both real and neither previously visible, because
nothing in the tree currently compiles optimised.

**LTO does not link.** `-flto` on `bluepill-c8` fails every app with

    (.isr_vector+0x4): undefined reference to `Reset_Handler'

`Reset_Handler` is defined in a C++ TU (`libkickos_chip_stm32f103.a(chip_stm32f103.cc.obj)`) and is
referenced **only** from the vector table in an assembly object (`startup.S.obj`), so the LTO plugin
does not see a reason to keep the definition. The cause is identified; no fix was attempted. LTO is
therefore not an available recovery today.

**`-Os` and `-O2` do not compile on RP2040 and RP2350 with the selftest flag on.**
`arch/arm/chip/rp2040/chip_rp2040.cc:80` and `arch/arm/chip/rp2350/chip_rp2350.cc:96` fail with

    error: array subscript 0 is outside array bounds of 'volatile uint8_t [0]' [-Werror=array-bounds=]

Bisected on the exact compile line: clean at `-O0` and `-O1`, six errors at `-O2` and at `-Os`. The
offending accessors are the bootrom-header readers `r8`/`r16`, which exist only under
`KICKOS_ENABLE_SELFTEST` (they serve `arch_reboot`), which is why the same board's `-Os` build with
the flag off succeeds. Consequence, measured: `picopi` and `pizero2350` cannot build an optimised
selftest image at all -- those are the only two of the 28 fleet builds at `-Os` that failed.
