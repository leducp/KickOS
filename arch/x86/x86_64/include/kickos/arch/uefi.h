// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The slice of the UEFI 2.11 boot-time interface this port calls. Field and member ORDER is
// fixed by the firmware ABI.

#ifndef KICKOS_ARCH_UEFI_H
#define KICKOS_ARCH_UEFI_H

#include <stdint.h>

namespace kickos::uefi
{
    // The Microsoft x64 convention. Every declaration firmware reads or writes carries it.
    #define KICKOS_EFIAPI __attribute__((ms_abi))

    using status_t = uint64_t;
    using handle_t = void*;

    // UEFI 2.11 appendix D. The high bit set marks an error.
    constexpr status_t status_success = 0;
    constexpr status_t status_buffer_too_small = 0x8000000000000005ull;

    // UEFI 2.11 section 7.2, EFI_MEMORY_TYPE.
    constexpr uint32_t memory_conventional = 7;

    struct table_header
    {
        uint64_t signature;
        uint32_t revision;
        uint32_t header_size;
        uint32_t crc32;
        uint32_t reserved;
    };

    // GetMemoryMap reports the stride separately, and it may EXCEED this structure: the
    // walk below strides by the reported size and never by sizeof.
    struct memory_descriptor
    {
        uint32_t type;
        uint32_t pad;
        uint64_t physical_start;
        uint64_t virtual_start;
        uint64_t page_count;
        uint64_t attribute;
    };

    using get_memory_map_fn = status_t(KICKOS_EFIAPI*)(uint64_t* map_size,
                                                       memory_descriptor* map,
                                                       uint64_t* map_key,
                                                       uint64_t* descriptor_size,
                                                       uint32_t* descriptor_version);

    using exit_boot_services_fn = status_t(KICKOS_EFIAPI*)(handle_t image_handle,
                                                           uint64_t map_key);

    // UEFI 2.11 section 4.4. Every member is present because the two this port calls are
    // found by their OFFSET in this table.
    struct boot_services
    {
        table_header hdr;

        void* raise_tpl;
        void* restore_tpl;

        void* allocate_pages;
        void* free_pages;
        get_memory_map_fn get_memory_map;
        void* allocate_pool;
        void* free_pool;

        void* create_event;
        void* set_timer;
        void* wait_for_event;
        void* signal_event;
        void* close_event;
        void* check_event;

        void* install_protocol_interface;
        void* reinstall_protocol_interface;
        void* uninstall_protocol_interface;
        void* handle_protocol;
        void* reserved;
        void* register_protocol_notify;
        void* locate_handle;
        void* locate_device_path;
        void* install_configuration_table;

        void* load_image;
        void* start_image;
        void* exit;
        void* unload_image;
        exit_boot_services_fn exit_boot_services;
    };

    // UEFI 2.11 section 4.3, truncated after the member this port reads.
    struct system_table
    {
        table_header hdr;
        char16_t* firmware_vendor;
        uint32_t firmware_revision;
        handle_t console_in_handle;
        void* con_in;
        handle_t console_out_handle;
        void* con_out;
        handle_t standard_error_handle;
        void* std_err;
        void* runtime_services;
        struct boot_services* boot_services;
    };
}

#endif
