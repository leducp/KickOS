// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/image.h>
#include <kickos/sys/init.h>

#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

namespace
{
    using kickos::ImageBounds;
    using kickos::ImageCheck;

    bool pow2_region(uintptr_t base, uintptr_t size)
    {
        return (size & (size - 1u)) == 0 and (base & (size - 1u)) == 0;
    }

    constexpr uintptr_t BUILD = 0x5eed;

    // The image's code span [0x8000, 0x10000) in host memory, which every name and declaration is
    // read through: program at 0x8300, stamp at 0x8310, decls at 0x8400, grants at 0x8600,
    // declaration names at 0x8700. An address outside it reads as zeros.
    constexpr uintptr_t CODE = 0x8000;
    constexpr uintptr_t CODE_END = 0x10000;
    constexpr uintptr_t DECLS = 0x8400;
    constexpr uintptr_t GRANTS = 0x8600;
    constexpr uintptr_t NAME = 0x8700;
    alignas(16) unsigned char g_code[0x8000];
    alignas(16) unsigned char g_zero[0x400];

    // The image code_view shows g_code for; every other one reads zeros.
    constexpr size_t VIEWED = 2;

    void const* code_view(size_t image, uintptr_t a)
    {
        if (image == VIEWED and a >= CODE and a < CODE + sizeof(g_code))
        {
            return &g_code[a - CODE];
        }
        return g_zero;
    }

    // A board whose code memory is [0x1000, 0x100000) with the kernel's code at its base, and
    // whose RAM is [0x200000, 0x300000) with the kernel's data at its base and its stacks on top.
    ImageBounds board()
    {
        ImageBounds b{};
        b.build_id = BUILD;
        b.spans = true;
        b.code = {0x1000, 0x100000};
        b.kernel_code = {0x1000, 0x8000};
        b.ram = {0x200000, 0x300000};
        b.kernel_data = {0x200000, 0x204000};
        b.kernel_stacks = {0x2f0000, 0x300000};
        b.arena = {0x210000, 0x2f0000};
        b.tls = {0x9000, 0x9100};
        b.tls_align_max = 8;
        b.window_encodable = pow2_region;
        b.view = code_view;
        b.image = VIEWED;
        return b;
    }

#if KICKOS_IMAGE_CHECK_SPANS
    // A board whose code and windows share one memory: the kernel's code at its base, then
    // the code memory every image's code sits in.
    ImageBounds ram_only()
    {
        ImageBounds b = board();
        b.kernel_code = {0x1000, 0x8000};
        b.code = {0x8000, 0x20000};
        b.ram = {0x1000, 0x300000};
        return b;
    }

    // Bytes [at, CODE_END) of the code span hold no NUL until restored.
    void unterminated_tail(uintptr_t at)
    {
        memset(&g_code[at - CODE], 'x', CODE_END - at);
    }

    void restore_tail(uintptr_t at)
    {
        memset(&g_code[at - CODE], 0, CODE_END - at);
    }
#endif

    kos_image_header image()
    {
        memcpy(&g_code[0x8300 - CODE], "app", 4);
        memcpy(&g_code[0x8310 - CODE], "stamp", 6);
        kos_image_header h{};
        h.magic = KOS_IMAGE_MAGIC;
        h.version = KOS_IMAGE_VERSION;
        h.size = sizeof(kos_image_header);
        h.build_id = BUILD;
        h.program = 0x8300;
        h.code_start = 0x8000;
        h.code_end = 0x10000;
        h.window_start = 0x208000;
        h.window_size = 0x4000;
        h.data_load = 0xf000;
        h.data_start = 0x208000;
        h.data_end = 0x208100;
        h.bss_end = 0x208400;
        h.heap_start = 0x208400;
        h.heap_end = 0x20c000;
        h.tls_template = 0x9000;
        h.tls_offset = 0;
        h.tls_init = 0x20;
        h.tls_total = 0x40;
        h.tls_align = 8;
        h.ctor_start = 0xa000;
        h.ctor_end = 0xa000 + 2 * sizeof(void*);
        h.eh_frame = 0xb000;
        h.start = 0x8101;
        h.reent_template = 0x208010;
        h.reent_width = 0x40;
        h.reent_seat = 0x208080;
        h.stamp = 0x8310;
        h.init_args = 0x208090;
        h.thread_return = 0x8201;
        return h;
    }

    ImageCheck check(kos_image_header const& h, ImageBounds const& b = board())
    {
        return kickos::image_header_check(h, b, nullptr, 0);
    }

    TEST(ImageCheck, a_well_formed_header_is_accepted)
    {
        EXPECT_EQ(check(image()), ImageCheck::OK);
    }

    TEST(ImageCheck, magic)
    {
        kos_image_header h = image();
        h.magic ^= 1u;
        EXPECT_EQ(check(h), ImageCheck::MAGIC);
    }

    TEST(ImageCheck, version)
    {
        kos_image_header h = image();
        h.version = KOS_IMAGE_VERSION + 1u;
        EXPECT_EQ(check(h), ImageCheck::VERSION);
    }

    TEST(ImageCheck, size)
    {
        kos_image_header h = image();
        h.size = sizeof(kos_image_header) - sizeof(uintptr_t);
        EXPECT_EQ(check(h), ImageCheck::SIZE);
    }

    TEST(ImageCheck, build_id_from_another_link)
    {
        kos_image_header h = image();
        h.build_id = BUILD + 1u;
        EXPECT_EQ(check(h), ImageCheck::BUILD_ID);
    }

    TEST(ImageCheck, needs)
    {
        kos_image_header h = image();
        h.needs_count = 1;
        EXPECT_EQ(check(h), ImageCheck::NEEDS);
    }

    TEST(ImageCheck, decls)
    {
        kos_image_header h = image();
        h.decl_count = 1;
        EXPECT_EQ(check(h), ImageCheck::DECLS);
    }

    TEST(ImageCheck, start)
    {
        kos_image_header h = image();
        h.start = 0;
        EXPECT_EQ(check(h), ImageCheck::START);
    }

    TEST(ImageCheck, init_args)
    {
        kos_image_header h = image();
        h.init_args += 1;
        EXPECT_EQ(check(h), ImageCheck::INIT_ARGS);
    }

    TEST(ImageCheck, thread_return)
    {
        kos_image_header h = image();
        h.thread_return = 0;
        EXPECT_EQ(check(h), ImageCheck::THREAD_RETURN);
    }

    TEST(ImageCheck, task_start_where_the_board_enters_no_thread_there)
    {
        kos_image_header h = image();
        h.task_start = 0x8401;
        EXPECT_EQ(check(h), ImageCheck::TASK_START);
    }

    TEST(ImageCheck, task_start_missing_where_the_board_enters_every_thread_there)
    {
        ImageBounds b = board();
        b.task_start = true;
        EXPECT_EQ(check(image(), b), ImageCheck::TASK_START);
    }

    TEST(ImageCheck, cas_store_below_its_start)
    {
        kos_image_header h = image();
        h.cas_start = 0x8501;
        h.cas_store = 0x8500;
        EXPECT_EQ(check(h), ImageCheck::CAS);
    }

    TEST(ImageCheck, cas_window_with_one_end)
    {
        kos_image_header h = image();
        h.cas_store = 0x8510;
        EXPECT_EQ(check(h), ImageCheck::CAS);
    }

#if KICKOS_IMAGE_CHECK_SPANS
    TEST(ImageCheck, cas_window_outside_the_code_span)
    {
        kos_image_header h = image();
        h.cas_start = 0xfff0;
        h.cas_store = 0x10004;
        EXPECT_EQ(check(h), ImageCheck::CAS);
    }

    TEST(ImageCheck, the_sim_states_no_span)
    {
        ImageBounds b = board();
        b.spans = false;
        kos_image_header h = image();
        h.code_end = 0;
        EXPECT_EQ(check(h, b), ImageCheck::OK);
    }

    TEST(ImageCheck, tls_init_above_tls_total)
    {
        kos_image_header h = image();
        h.tls_init = h.tls_total + 1u;
        EXPECT_EQ(check(h), ImageCheck::TLS_SIZES);
    }

    TEST(ImageCheck, tls_align_not_a_power_of_two)
    {
        kos_image_header h = image();
        h.tls_align = 12;
        EXPECT_EQ(check(h), ImageCheck::TLS_ALIGN);
    }

    TEST(ImageCheck, tls_align_past_the_stack_alignment)
    {
        kos_image_header h = image();
        h.tls_align = 16;
        EXPECT_EQ(check(h), ImageCheck::TLS_ALIGN);
    }

    TEST(ImageCheck, tls_offset_past_the_segment)
    {
        kos_image_header h = image();
        h.tls_offset = 0x200;
        EXPECT_EQ(check(h), ImageCheck::TLS_SEGMENT);
    }

    TEST(ImageCheck, tls_total_wraps_past_the_segment)
    {
        kos_image_header h = image();
        h.tls_offset = 0x10;
        h.tls_total = UINTPTR_MAX - 0x8;
        h.tls_init = 0;
        EXPECT_EQ(check(h), ImageCheck::TLS_SEGMENT);
    }

    TEST(ImageCheck, code_span_backwards)
    {
        kos_image_header h = image();
        h.code_end = h.code_start - 1u;
        EXPECT_EQ(check(h), ImageCheck::CODE_SPAN);
    }

    TEST(ImageCheck, code_span_past_code_memory)
    {
        kos_image_header h = image();
        h.code_end = 0x100001;
        EXPECT_EQ(check(h), ImageCheck::CODE_SPAN);
    }

    TEST(ImageCheck, code_span_meets_the_kernel)
    {
        kos_image_header h = image();
        h.code_start = 0x7ff0;
        EXPECT_EQ(check(h), ImageCheck::CODE_KERNEL);
    }

    TEST(ImageCheck, code_span_meets_an_earlier_image)
    {
        kos_image_header const first = image();
        kos_image_header const* const earlier[] = {&first};
        kos_image_header h = image();
        h.window_start = 0x20c000;
        h.data_start = h.data_end = h.bss_end = h.heap_start = h.heap_end = 0x20c000;
        h.reent_width = 0;
        h.reent_seat = 0;
        h.init_args = 0x20c000;
        h.window_size = 0x4000;
        h.heap_end = 0x210000;
        EXPECT_EQ(kickos::image_header_check(h, board(), earlier, 1), ImageCheck::CODE_OVERLAP);
    }

    TEST(ImageCheck, start_outside_the_code_span)
    {
        kos_image_header h = image();
        h.start = 0x2001;
        EXPECT_EQ(check(h), ImageCheck::START);
    }

    TEST(ImageCheck, thread_return_outside_the_code_span)
    {
        kos_image_header h = image();
        h.thread_return = 0x10000;
        EXPECT_EQ(check(h), ImageCheck::THREAD_RETURN);
    }

    TEST(ImageCheck, task_start_outside_the_code_span)
    {
        ImageBounds b = board();
        b.task_start = true;
        kos_image_header h = image();
        h.task_start = 0x7001;
        EXPECT_EQ(check(h, b), ImageCheck::TASK_START);
    }

    TEST(ImageCheck, window_wraps)
    {
        kos_image_header h = image();
        h.window_size = UINTPTR_MAX - 0x1000;
        EXPECT_EQ(check(h), ImageCheck::WINDOW_SPAN);
    }

    TEST(ImageCheck, window_outside_ram)
    {
        kos_image_header h = image();
        h.window_start = 0x100000;
        EXPECT_EQ(check(h), ImageCheck::WINDOW_SPAN);
    }

    TEST(ImageCheck, window_meets_kernel_data)
    {
        kos_image_header h = image();
        h.window_start = 0x203000;
        h.window_size = 0x8000;
        EXPECT_EQ(check(h), ImageCheck::WINDOW_KERNEL);
    }

    TEST(ImageCheck, window_meets_the_kernel_stacks)
    {
        kos_image_header h = image();
        h.window_start = 0x2f0000;
        EXPECT_EQ(check(h), ImageCheck::WINDOW_KERNEL);
    }

    TEST(ImageCheck, window_meets_the_arena)
    {
        kos_image_header h = image();
        h.window_size = 0x10000;
        h.window_start = 0x210000;
        EXPECT_EQ(check(h), ImageCheck::WINDOW_KERNEL);
    }

    TEST(ImageCheck, a_window_beside_the_code_in_one_memory_is_accepted)
    {
        EXPECT_EQ(check(image(), ram_only()), ImageCheck::OK);
    }

    TEST(ImageCheck, window_meets_the_kernel_code)
    {
        kos_image_header h = image();
        h.window_start = 0x4000;
        EXPECT_EQ(check(h, ram_only()), ImageCheck::WINDOW_KERNEL);
    }

    TEST(ImageCheck, window_meets_the_image_s_own_code)
    {
        kos_image_header h = image();
        h.window_start = 0xc000;
        EXPECT_EQ(check(h, ram_only()), ImageCheck::WINDOW_CODE);
    }

    TEST(ImageCheck, window_meets_another_image_s_code)
    {
        kos_image_header h = image();
        h.window_start = 0x18000;
        EXPECT_EQ(check(h, ram_only()), ImageCheck::WINDOW_CODE);
    }

    TEST(ImageCheck, window_meets_an_earlier_image)
    {
        kos_image_header const first = image();
        kos_image_header const* const earlier[] = {&first};
        kos_image_header h = image();
        h.code_start = 0x10000;
        h.code_end = 0x20000;
        h.start = 0x10001;
        h.thread_return = 0x10001;
        h.program = 0x10100;
        h.stamp = 0x10100;
        h.data_load = 0x11000;
        h.ctor_start = h.ctor_end = 0x12000;
        h.eh_frame = 0;
        EXPECT_EQ(kickos::image_header_check(h, board(), earlier, 1),
                  ImageCheck::WINDOW_OVERLAP);
    }

    TEST(ImageCheck, window_is_not_one_region)
    {
        kos_image_header h = image();
        h.window_size = 0x3000;
        h.heap_end = 0x20b000;
        EXPECT_EQ(check(h), ImageCheck::WINDOW_ENCODABLE);
    }

    TEST(ImageCheck, init_args_outside_the_window)
    {
        kos_image_header h = image();
        h.init_args = 0x200100;
        EXPECT_EQ(check(h), ImageCheck::INIT_ARGS);
    }

    TEST(ImageCheck, bss_past_the_window)
    {
        kos_image_header h = image();
        h.bss_end = 0x20c008;
        EXPECT_EQ(check(h), ImageCheck::DATA);
    }

    TEST(ImageCheck, data_backwards)
    {
        kos_image_header h = image();
        h.data_end = h.data_start - 8u;
        EXPECT_EQ(check(h), ImageCheck::DATA);
    }

    TEST(ImageCheck, heap_over_bss)
    {
        kos_image_header h = image();
        h.heap_start = 0x208300;
        EXPECT_EQ(check(h), ImageCheck::HEAP);
    }

    TEST(ImageCheck, heap_misaligned)
    {
        kos_image_header h = image();
        h.heap_start = 0x208404;
        EXPECT_EQ(check(h), ImageCheck::HEAP);
    }

    TEST(ImageCheck, reent_seat_in_kernel_data)
    {
        kos_image_header h = image();
        h.reent_seat = 0x200100;
        EXPECT_EQ(check(h), ImageCheck::REENT_SEAT);
    }

    TEST(ImageCheck, reent_seat_straddling_the_window_end)
    {
        kos_image_header h = image();
        h.reent_seat = h.window_start + h.window_size - sizeof(void*) / 2u;
        EXPECT_EQ(check(h), ImageCheck::REENT_SEAT);
    }

    TEST(ImageCheck, reent_template_running_out_of_the_window)
    {
        kos_image_header h = image();
        h.reent_width = UINTPTR_MAX;
        EXPECT_EQ(check(h), ImageCheck::REENT_TEMPLATE);
    }

    TEST(ImageCheck, reent_template_past_the_initialised_data)
    {
        kos_image_header h = image();
        h.reent_template = h.data_end - h.reent_width / 2u;
        EXPECT_EQ(check(h), ImageCheck::REENT_TEMPLATE);
    }

    TEST(ImageCheck, data_load_outside_the_code_span)
    {
        kos_image_header h = image();
        h.data_load = 0xff80;
        EXPECT_EQ(check(h), ImageCheck::DATA_LOAD);
    }

    TEST(ImageCheck, data_loaded_in_place_takes_its_template_in_the_code_span_or_none)
    {
        ImageBounds b = board();
        b.data_in_place = true;
        EXPECT_EQ(check(image(), b), ImageCheck::OK);
        kos_image_header h = image();
        h.data_load = h.data_start;
        EXPECT_EQ(check(h, b), ImageCheck::OK);
    }

    TEST(ImageCheck, data_template_in_place_outside_the_code_span)
    {
        ImageBounds b = board();
        b.data_in_place = true;
        kos_image_header h = image();
        h.data_load = 0xff80;
        EXPECT_EQ(check(h, b), ImageCheck::DATA_LOAD);
    }

    TEST(ImageCheck, tls_template_away_from_its_offset)
    {
        kos_image_header h = image();
        h.tls_template = 0x9010;
        EXPECT_EQ(check(h), ImageCheck::TLS_TEMPLATE);
    }

    TEST(ImageCheck, ctor_window_with_a_part_word)
    {
        kos_image_header h = image();
        h.ctor_end -= 1u;
        EXPECT_EQ(check(h), ImageCheck::CTOR);
    }

    TEST(ImageCheck, ctor_window_outside_the_image)
    {
        kos_image_header h = image();
        h.ctor_start = 0x2000;
        EXPECT_EQ(check(h), ImageCheck::CTOR);
    }

    TEST(ImageCheck, eh_frame_outside_the_code_span)
    {
        kos_image_header h = image();
        h.eh_frame = 0x10000;
        EXPECT_EQ(check(h), ImageCheck::EH_FRAME);
    }

    TEST(ImageCheck, program)
    {
        kos_image_header h = image();
        h.program = 0;
        EXPECT_EQ(check(h), ImageCheck::PROGRAM);
    }

    TEST(ImageCheck, stamp)
    {
        kos_image_header h = image();
        h.stamp = 0x2000;
        EXPECT_EQ(check(h), ImageCheck::STAMP);
    }

    TEST(ImageCheck, program_with_no_nul_in_the_code_span)
    {
        kos_image_header h = image();
        h.program = CODE_END - 8u;
        unterminated_tail(h.program);
        EXPECT_EQ(check(h), ImageCheck::PROGRAM);
        restore_tail(h.program);
    }

    TEST(ImageCheck, stamp_with_no_nul_in_the_code_span)
    {
        kos_image_header h = image();
        h.stamp = CODE_END - 8u;
        unterminated_tail(h.stamp);
        EXPECT_EQ(check(h), ImageCheck::STAMP);
        restore_tail(h.stamp);
    }

    TEST(ImageCheck, a_stamp_whose_nul_is_the_last_code_byte_is_accepted)
    {
        kos_image_header h = image();
        h.stamp = CODE_END - 8u;
        unterminated_tail(h.stamp);
        g_code[CODE_END - 1u - CODE] = 0;
        EXPECT_EQ(check(h), ImageCheck::OK);
        restore_tail(h.stamp);
    }
#else
    // What this posture leaves to an enforcing board.
    TEST(ImageCheck, a_span_is_not_checked_where_nothing_is_enforced)
    {
        kos_image_header h = image();
        h.code_end = 0;
        h.reent_seat = 0x200100;
        EXPECT_EQ(check(h), ImageCheck::OK);
    }
#endif

    bool device_ok(uintptr_t base, uintptr_t size)
    {
        return base >= 0x40000000u and size != 0 and size <= 0x1000u;
    }

    bool ports_ok(uintptr_t base, uintptr_t count)
    {
        return count != 0 and base + count <= 0x100u;
    }

    bool line_ok(uint16_t line)
    {
        return line < 64u and line != 7u; // 7 is the kernel's
    }

    ImageBounds declaring()
    {
        ImageBounds b = board();
        b.decls_max = 4;
        b.prio_min = 1;
        b.prio_max = 31;
        b.cores = 0x3;
        b.authority = 0xff;
        b.stack_align = 8;
        b.windows_max = 2;
        b.lines_max = 2;
        b.device_ok = device_ok;
        b.ports_ok = ports_ok;
        b.line_ok = line_ok;
        b.view = code_view;
        b.image = VIEWED;
        return b;
    }

    kos_task_decl* decl(uintptr_t i)
    {
        return reinterpret_cast<kos_task_decl*>(&g_code[DECLS - CODE]) + i;
    }

    kos_decl_grant* grant(uintptr_t k)
    {
        return reinterpret_cast<kos_decl_grant*>(&g_code[GRANTS - CODE]) + k;
    }

    // Two declarations: a task holding a device window, a port window and a line, and a driver
    // holding nothing, with no stack of its own.
    kos_image_header declared()
    {
        memset(g_code, 0, sizeof(g_code));
        memcpy(&g_code[NAME - CODE], "sensor", 7);
        *grant(0) = kos_decl_grant{};
        grant(0)->kind = KOS_DECL_DEVICE;
        grant(0)->base = 0x40001000u;
        grant(0)->size = 0x400u;
        grant(1)->kind = KOS_DECL_PORTS;
        grant(1)->base = 0x60u;
        grant(1)->size = 0x10u;
        grant(2)->kind = KOS_DECL_LINE;
        grant(2)->line = 12;
        for (uintptr_t i = 0; i < 2; i++)
        {
            decl(i)->name = NAME - DECLS;
            decl(i)->priority = 8;
            decl(i)->ceiling = 9;
            decl(i)->core_mask = 0x1;
            decl(i)->authority = 0x3;
            decl(i)->stack = 0x800;
        }
        decl(0)->grants = GRANTS - DECLS;
        decl(0)->grant_count = 3;
        decl(1)->stack = 0;
        decl(1)->flags = KOS_DECL_DRIVER | KOS_DECL_CONSOLE;
        kos_image_header h = image();
        h.decls = DECLS;
        h.decl_count = 2;
        h.decl_start = 0x8111;
        return h;
    }

    ImageCheck check_declared(kos_image_header const& h)
    {
        return check(h, declaring());
    }

    TEST(ImageCheck, well_formed_declarations_are_accepted)
    {
        EXPECT_EQ(check_declared(declared()), ImageCheck::OK);
    }

#if KICKOS_IMAGE_CHECK_SPANS
    TEST(ImageCheck, declarations_are_read_through_their_own_image)
    {
        ImageBounds b = declaring();
        b.image = VIEWED + 1;
        EXPECT_NE(check(declared(), b), ImageCheck::OK);
    }
#endif

    TEST(ImageCheck, decls_without_a_count)
    {
        kos_image_header h = declared();
        h.decl_count = 0;
        EXPECT_EQ(check_declared(h), ImageCheck::DECLS);
    }

    TEST(ImageCheck, decls_past_their_bound)
    {
        kos_image_header h = declared();
        h.decl_count = 5;
        EXPECT_EQ(check_declared(h), ImageCheck::DECLS);
    }

    TEST(ImageCheck, declarations_with_no_entry)
    {
        kos_image_header h = declared();
        h.decl_start = 0;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_START);
    }

    TEST(ImageCheck, an_entry_with_no_declaration)
    {
        kos_image_header h = image();
        h.decl_start = 0x8111;
        EXPECT_EQ(check(h), ImageCheck::DECL_START);
    }

    TEST(ImageCheck, decls_misaligned)
    {
        kos_image_header h = declared();
        h.decls += 1;
        EXPECT_EQ(check_declared(h), ImageCheck::DECLS);
    }

#if KICKOS_IMAGE_CHECK_SPANS
    TEST(ImageCheck, declaration_with_no_name)
    {
        kos_image_header h = declared();
        decl(1)->name = 0;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_NAME);
    }

    TEST(ImageCheck, declaration_priority_below_the_range)
    {
        kos_image_header h = declared();
        decl(1)->priority = 0;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_PRIORITY);
    }

    TEST(ImageCheck, declaration_priority_above_the_range)
    {
        kos_image_header h = declared();
        decl(1)->priority = 32;
        decl(1)->ceiling = 32;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_PRIORITY);
    }

    TEST(ImageCheck, declaration_ceiling_below_its_priority)
    {
        kos_image_header h = declared();
        decl(1)->ceiling = 7;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_CEILING);
    }

    TEST(ImageCheck, declaration_ceiling_above_the_range)
    {
        kos_image_header h = declared();
        decl(1)->ceiling = 32;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_CEILING);
    }

    TEST(ImageCheck, declaration_naming_no_core_of_the_kernel)
    {
        kos_image_header h = declared();
        decl(1)->core_mask = 0x4;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_CORES);
    }

    TEST(ImageCheck, declaration_flag_undefined)
    {
        kos_image_header h = declared();
        decl(1)->flags = 1u << 7;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_FLAGS);
    }

    TEST(ImageCheck, a_declaration_started_on_request_is_accepted)
    {
        kos_image_header h = declared();
        decl(0)->flags = KOS_DECL_ON_REQUEST;
        EXPECT_EQ(check_declared(h), ImageCheck::OK);
    }

    TEST(ImageCheck, a_declaration_restarted_is_accepted)
    {
        kos_image_header h = declared();
        decl(0)->flags = KOS_DECL_RESTARTS;
        EXPECT_EQ(check_declared(h), ImageCheck::OK);
    }

    TEST(ImageCheck, a_console_driver_started_on_request_is_accepted)
    {
        kos_image_header h = declared();
        decl(1)->flags = KOS_DECL_DRIVER | KOS_DECL_ON_REQUEST | KOS_DECL_CONSOLE;
        decl(1)->grant_count = 0;
        EXPECT_EQ(check_declared(h), ImageCheck::OK);
    }

    TEST(ImageCheck, a_driver_started_on_request_without_the_console)
    {
        kos_image_header h = declared();
        decl(1)->flags = KOS_DECL_DRIVER | KOS_DECL_ON_REQUEST;
        decl(1)->grant_count = 0;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_FLAGS);
    }

    TEST(ImageCheck, a_driver_started_on_request_holding_a_grant)
    {
        kos_image_header h = declared();
        decl(1)->flags = KOS_DECL_DRIVER | KOS_DECL_ON_REQUEST | KOS_DECL_CONSOLE;
        decl(1)->grant_count = 1;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_FLAGS);
    }

    TEST(ImageCheck, the_declarations_entry_outside_the_code_span)
    {
        kos_image_header h = declared();
        h.decl_start = 0x7001;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_START);
    }

    TEST(ImageCheck, declaration_stack_misaligned)
    {
        kos_image_header h = declared();
        decl(0)->stack = 0x804;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_STACK);
    }

    TEST(ImageCheck, declaration_with_no_stack_and_no_driver)
    {
        kos_image_header h = declared();
        decl(0)->stack = 0;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_STACK);
    }

    TEST(ImageCheck, declaration_authority_undefined)
    {
        kos_image_header h = declared();
        decl(1)->authority = 0x100;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_AUTHORITY);
    }

    TEST(ImageCheck, declaration_grants_past_their_bound)
    {
        kos_image_header h = declared();
        decl(0)->grant_count = 5;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_GRANTS);
    }

    TEST(ImageCheck, declaration_grants_without_a_count)
    {
        kos_image_header h = declared();
        decl(1)->grants = GRANTS - DECLS;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_GRANTS);
    }

    TEST(ImageCheck, declaration_grants_misaligned)
    {
        kos_image_header h = declared();
        decl(0)->grants += 2;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_GRANTS);
    }

    TEST(ImageCheck, declaration_grant_of_no_kind)
    {
        kos_image_header h = declared();
        grant(1)->kind = 0;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_KIND);
    }

    TEST(ImageCheck, declaration_windows_past_their_bound)
    {
        kos_image_header h = declared();
        grant(2)->kind = KOS_DECL_DEVICE;
        grant(2)->base = 0x40002000u;
        grant(2)->size = 0x400u;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_WINDOWS);
    }

    TEST(ImageCheck, declaration_device_window_refused)
    {
        kos_image_header h = declared();
        grant(0)->base = 0x30000000u;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_DEVICE);
    }

    TEST(ImageCheck, declaration_port_window_refused)
    {
        kos_image_header h = declared();
        grant(1)->base = 0xf8u;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_PORTS);
    }

    TEST(ImageCheck, declaration_lines_past_their_bound)
    {
        kos_image_header h = declared();
        grant(0)->kind = KOS_DECL_LINE;
        grant(0)->line = 13;
        grant(1)->kind = KOS_DECL_LINE;
        grant(1)->line = 14;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_LINES);
    }

    TEST(ImageCheck, declaration_line_the_kernel_owns)
    {
        kos_image_header h = declared();
        grant(2)->line = 7;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_LINE);
    }

    TEST(ImageCheck, decls_outside_the_code_span)
    {
        kos_image_header h = declared();
        h.decls = 0x10000 - sizeof(kos_task_decl);
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_SPAN);
    }

    TEST(ImageCheck, declaration_name_outside_the_code_span)
    {
        kos_image_header h = declared();
        decl(1)->name = 0x10000 - DECLS;
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_NAME);
    }

    TEST(ImageCheck, declaration_name_with_no_nul_in_the_code_span)
    {
        kos_image_header h = declared();
        decl(1)->name = CODE_END - 8u - DECLS;
        unterminated_tail(CODE_END - 8u);
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_NAME);
        restore_tail(CODE_END - 8u);
    }

    TEST(ImageCheck, declaration_grants_outside_the_code_span)
    {
        kos_image_header h = declared();
        decl(0)->grants = 0x10000 - DECLS - sizeof(kos_decl_grant);
        EXPECT_EQ(check_declared(h), ImageCheck::DECL_GRANTS);
    }

    TEST(ImageCheck, a_declaration_s_span_is_not_checked_where_the_board_states_none)
    {
        ImageBounds b = declaring();
        b.spans = false;
        kos_image_header h = declared();
        decl(1)->name = 0x10000 - DECLS;
        EXPECT_EQ(check(h, b), ImageCheck::OK);
        decl(1)->priority = 0;
        EXPECT_EQ(check(h, b), ImageCheck::DECL_PRIORITY) << "what a declaration states still is";
    }
#else
    TEST(ImageCheck, a_declaration_is_not_checked_where_nothing_is_enforced)
    {
        kos_image_header h = declared();
        decl(1)->name = 0x10000 - DECLS;
        decl(1)->priority = 0;
        grant(2)->line = 7;
        EXPECT_EQ(check_declared(h), ImageCheck::OK);
    }
#endif

#if KICKOS_IMAGE_CHECK_SPANS
    TEST(ImageCheck, every_refusal_names_its_field)
    {
        for (int c = static_cast<int>(ImageCheck::MAGIC); c < static_cast<int>(ImageCheck::COUNT);
             c++)
        {
            char const* const m = kickos::image_check_message(static_cast<ImageCheck>(c));
            EXPECT_STRNE(m, "refused") << "check " << c;
        }
    }
#endif
}
