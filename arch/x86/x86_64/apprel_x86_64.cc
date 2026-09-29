// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The app window's second relocation (docs/design-m10-kernel-share.md section 1.3).
//
// Firmware applied the image's base relocations for the address it loaded the image at, which
// is the kernel's view. A task reaches the app window at that address plus the user offset, so
// every absolute word stored IN the app window and aimed INTO it gains that offset once, here,
// before the first space maps any of it. Words in the kernel's half stay where firmware put
// them. A word in the app window aimed at the kernel is a link defect the build refuses; meeting
// one here refuses the boot rather than guessing.
//
// The records are the retained copy the second link places in .krel, because .reloc itself is a
// discardable section a loader is free not to keep.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/arch/regs.h>
#include <kickos/chip_com1.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
    // The linker's own symbol for the image's first byte, taken PC-relative, so it is where
    // firmware LOADED the image.
    extern __attribute__((visibility("hidden"))) char __ImageBase[];
    extern unsigned char __kickos_krel_start[];
    extern unsigned char __kickos_krel_end[];
    extern unsigned char __kickos_app_rom_start[];
    extern unsigned char __kickos_app_sram_end[];
}

namespace
{
    // PE32+ (Microsoft PE/COFF specification): the optional header's magic, SizeOfImage, and
    // the base-relocation entry of the data directory; a relocation entry's type in its top
    // four bits.
    constexpr uint16_t pe32plus_magic = 0x20b;
    constexpr size_t opt_size_of_image = 56;
    constexpr size_t opt_basereloc_dir = 112 + 5 * 8;
    constexpr unsigned rel_absolute = 0;
    constexpr unsigned rel_dir64 = 10;
    constexpr uint64_t cr0_wp = 1ull << 16;

    uintptr_t g_applied = 0;

    [[noreturn]] void refuse(char const* what)
    {
        kickos::q35::com1_puts("\nx86_64 app relocation: ");
        kickos::q35::com1_puts(what);
        kickos::q35::com1_puts("\n");
        kfault_terminate();
    }

    uint32_t read32(uint8_t const* p)
    {
        uint32_t v = 0;
        __builtin_memcpy(&v, p, sizeof(v));
        return v;
    }

    // The script's absolute __kickos_link_image_base, as an immediate: an absolute symbol
    // carries no base-relocation record, so firmware's pass leaves the value the link chose.
    uintptr_t link_image_base(void)
    {
        uintptr_t v = 0;
        __asm__("movabsq $__kickos_link_image_base, %0" : "=r"(v));
        return v;
    }

    uint16_t read16(uint8_t const* p)
    {
        uint16_t v = 0;
        __builtin_memcpy(&v, p, sizeof(v));
        return v;
    }
}

namespace kickos::x86_64
{
    void app_relocate(void)
    {
        uint8_t const* const image = reinterpret_cast<uint8_t const*>(__ImageBase);
        uint8_t const* const pe = image + read32(image + 0x3c);
        if (pe[0] != 'P' or pe[1] != 'E' or pe[2] != 0 or pe[3] != 0)
        {
            refuse("this image carries no PE signature");
        }
        uint8_t const* const opt = pe + 24;
        if (read16(opt) != pe32plus_magic)
        {
            refuse("this image is not PE32+");
        }
        uintptr_t const base = reinterpret_cast<uintptr_t>(image);
        uintptr_t const end = base + read32(opt + opt_size_of_image);
        uint32_t const dir_size = read32(opt + opt_basereloc_dir + 4);

        uint8_t const* p = __kickos_krel_start;
        uint8_t const* const krel_end = __kickos_krel_end;
        if (static_cast<size_t>(krel_end - p) != dir_size)
        {
            refuse("the retained relocation copy is not the size of the directory firmware used");
        }

        uintptr_t const app_lo = reinterpret_cast<uintptr_t>(__kickos_app_rom_start);
        uintptr_t const app_hi = reinterpret_cast<uintptr_t>(__kickos_app_sram_end);
        uintptr_t const u = arch_aspace_user_offset();
        if (u == 0)
        {
            refuse("the user offset is zero; aspace_init has not run");
        }

        uint64_t moved = 0;
        uint64_t const cr0 = read_cr0();
        write_cr0(cr0 & ~cr0_wp);
        while (krel_end - p >= 8)
        {
            uint32_t const page = read32(p);
            uint32_t const block = read32(p + 4);
            if (block < 8 or (block & 1u) != 0 or block > static_cast<size_t>(krel_end - p))
            {
                refuse("a relocation block is malformed");
            }
            for (uint32_t off = 8; off < block; off += 2)
            {
                uint16_t const entry = read16(p + off);
                unsigned const type = entry >> 12;
                if (type == rel_absolute)
                {
                    continue; // padding to the block's alignment
                }
                if (type != rel_dir64)
                {
                    refuse("a relocation record is not DIR64");
                }
                uintptr_t const loc = base + page + (entry & 0xfffu);
                if (loc < base or loc > end - 8)
                {
                    refuse("a relocation record points outside the image");
                }
                if (loc < app_lo or loc >= app_hi)
                {
                    continue; // the kernel's half keeps the loader address
                }
                uint64_t word = 0;
                __builtin_memcpy(&word, reinterpret_cast<void const*>(loc), sizeof(word));
                if (word < app_lo or word > app_hi)
                {
                    refuse("an absolute word in the app window is aimed at the kernel");
                }
                word += u;
                __builtin_memcpy(reinterpret_cast<void*>(loc), &word, sizeof(word));
                ++moved;
            }
            p += block;
        }
        write_cr0(cr0);
        if (p != krel_end)
        {
            refuse("the retained relocation copy ends inside a block");
        }
        g_applied = u;

        // The load delta says which case firmware's own pass ran: zero is the image at its
        // preferred base, where firmware relocated nothing.
        kickos::q35::com1_puts("x86_64 app relocation: base=");
        kickos::q35::com1_hex64(base);
        kickos::q35::com1_puts(" load_delta=");
        kickos::q35::com1_hex64(base - link_image_base());
        kickos::q35::com1_puts(" app_words=");
        kickos::q35::com1_dec(moved);
        kickos::q35::com1_puts("\n");
    }

    uintptr_t user_alias_offset(void)
    {
        return g_applied;
    }
}
