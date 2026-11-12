// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A spawn's window admission of a memory type, over the REAL system-call and address-space
// layers at the translating posture: where a page entry carries no type, the frames' own address
// decides it, and a type the backend cannot give is refused as unsupported, apart from a window
// the kernel holds or one mapped with another type.

#include "../common/seam_pools.h"

#include <stdint.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include "host_aspace.h"
#include "stub_seam.h"
#include "sys_fixture.h"
#include "syscall_internal.h"

extern "C"
{
    alignas(4096) unsigned char g_sys_image[SYS_IMAGE_BYTES];
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class MemtypeAdmit : public SysFixture
            {
              protected:
                void TearDown() override
                {
                    g_device_reserved = false;
                    SysFixture::TearDown();
                }

                int32_t spawn_device(kos_thread_t* out = nullptr)
                {
                    uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0);
                    *host<kos_window>(uva(MAIN_PARAMS, WINDOWS_AT)) = {
                        STUB_DEVICE_BASE, static_cast<uint32_t>(STUB_DEVICE_SIZE),
                        KOS_WINDOW_DEVICE, 0u};
                    params(p)->windows =
                        reinterpret_cast<kos_window const*>(uva(MAIN_PARAMS, WINDOWS_AT));
                    params(p)->window_count = 1;
                    kos_thread_t child = KOS_THREAD_NONE;
                    int32_t const rc = spawn(main_, p, MAIN_OUT, &child);
                    if (out != nullptr)
                    {
                        *out = child;
                    }
                    return rc;
                }
            };

            TEST_F(MemtypeAdmit, uncached_ram_an_untyped_entry_cannot_take_is_unsupported)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                untyped_entries(true);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(main_, window_spawn(blk, G), MAIN_OUT, &child), -KOS_ENOTSUP);
                EXPECT_EQ(child, KOS_THREAD_NONE);
            }

            TEST_F(MemtypeAdmit, uncached_window_over_cached_ram_is_busy_where_entries_type_it)
            {
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(main_, window_spawn(blk, G), MAIN_OUT, &child), -KOS_EBUSY);
            }

            TEST_F(MemtypeAdmit, a_device_window_is_admitted_where_the_backend_maps_device_memory)
            {
                EXPECT_EQ(spawn_device(), 0);
            }

            TEST_F(MemtypeAdmit, a_device_window_the_backend_cannot_type_is_unsupported)
            {
                refuse_device_type(true);
                EXPECT_EQ(spawn_device(), -KOS_ENOTSUP);
            }

            TEST_F(MemtypeAdmit, a_device_window_over_the_kernels_own_device_is_not_permitted)
            {
                g_device_reserved = true;
                EXPECT_EQ(spawn_device(), -KOS_EPERM);
            }

            TEST_F(MemtypeAdmit, a_device_window_answers_no_kernel_access)
            {
                kos_thread_t h = KOS_THREAD_NONE;
                ASSERT_EQ(spawn_device(&h), 0);
                Thread* const child = kernel().threads.resolve(h);
                ASSERT_NE(child, nullptr);
                ASSERT_EQ(err(call_as(child, KOS_SYS_WINDOW_GET, 0, uva(EDIT_OUT))), 0);
                uintptr_t const win = host<kos_window>(uva(EDIT_OUT))->base;
                ASSERT_NE(win, 0u);
                EXPECT_EQ(err(call_as(child, KOS_SYS_KCONSOLE_WRITE, win, 4)), -KOS_EFAULT)
                    << "a console write read the device";
                EXPECT_EQ(err(call_as(child, KOS_SYS_NOTIFY_CREATE, win)), -KOS_EFAULT)
                    << "an out word was written to the device";
                // The acquire's own refusal answers the calls above alike, so ask admission direct.
                running() = child;
                bool const readable = user_readable_ok(win, 4);
                bool const writable = user_writable_ok(win, 4);
                // The receive buffer's check; IPC itself is a stub here.
                bool const both = user_readable_and_writable_ok(win, 4, 4);
                running() = main_;
                EXPECT_FALSE(readable) << "a read was admitted on the device";
                EXPECT_FALSE(writable) << "a write was admitted on the device";
                EXPECT_FALSE(both) << "a receive buffer was admitted on the device";
            }
        }
    }
}
