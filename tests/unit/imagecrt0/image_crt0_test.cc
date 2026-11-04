// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/image.h>
#include <kickos/sys/init.h>

#include <gtest/gtest.h>

#include <stdint.h>

#if not (KICKOS_INIT_IMAGE and not KICKOS_HAVE_ASPACE)
#error "kickos_image_crt0 exists only where each task runs an image and no backend translates"
#endif

// The image's whole .bss is kmain's arguments: the copy and the zeroing run over them.
struct kos_init_args kickos_init_args = {0, nullptr};

extern "C"
{
    void (*__kickos_app_init_array_start[1])() = {nullptr};
    void (*__kickos_app_init_array_end[1])() = {nullptr};
}

namespace
{
    kos_image_header bss_over_the_arguments()
    {
        uintptr_t const at = reinterpret_cast<uintptr_t>(&kickos_init_args);
        kos_image_header h{};
        h.data_load = at;
        h.data_start = at;
        h.data_end = at;
        h.bss_end = at + sizeof(kickos_init_args);
        return h;
    }
}

struct kos_image_header const kickos_image_header = bss_over_the_arguments();

namespace
{
    TEST(ImageCrt0, the_arguments_kmain_published_survive_every_start)
    {
        char arg0[] = "kickos";
        char* argv[] = {arg0, nullptr};
        kickos_init_args.argc = 1;
        kickos_init_args.argv = argv;
        kickos_image_crt0();
        EXPECT_EQ(kickos_init_args.argc, 1);
        EXPECT_EQ(kickos_init_args.argv, argv);
        kickos_image_crt0();
        EXPECT_EQ(kickos_init_args.argc, 1) << "a restart runs the startup again";
        EXPECT_EQ(kickos_init_args.argv, argv);
    }
}
