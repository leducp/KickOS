// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The frame x86_64's interrupt exit resumes, against the bookings a handler's passes leave.
// trap_x86_64.S ends the lock span only for a frame other than the one it passed, so a booking
// back onto the interrupted context must end it here, or the core keeps the kernel lock.

#include "arch/x86/x86_64/isr_resume.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    struct Frame
    {
        uint64_t word;
    };

    struct Ctx
    {
        uintptr_t sp;
    };

    struct Calls
    {
        unsigned end_span = 0;
        unsigned park = 0;
        unsigned enter = 0;
        Ctx* parked = nullptr;
        Frame* parked_frame = nullptr;
        Ctx* entered = nullptr;
    };

    Frame* resume(Ctx*& booked, Ctx* current, Frame* frame, Calls& calls)
    {
        return kickos::x86_64::isr_resume(
            booked, current, frame, [&] { calls.end_span++; },
            [&](Ctx* c, Frame* f) {
                calls.park++;
                calls.parked = c;
                calls.parked_frame = f;
            },
            [&](Ctx* to) {
                calls.enter++;
                calls.entered = to;
            });
    }

    TEST(IsrResume, nothing_booked_resumes_the_interrupted_frame_and_ends_nothing)
    {
        Frame interrupted = {};
        Ctx current = {};
        Ctx* booked = nullptr;
        Calls calls;
        EXPECT_EQ(resume(booked, &current, &interrupted, calls), &interrupted);
        EXPECT_EQ(calls.end_span, 0u);
        EXPECT_EQ(calls.park, 0u);
        EXPECT_EQ(calls.enter, 0u);
    }

    TEST(IsrResume, a_booking_back_onto_the_interrupted_context_ends_the_span_here)
    {
        Frame interrupted = {};
        Ctx current = {};
        Ctx* booked = &current;
        Calls calls;
        EXPECT_EQ(resume(booked, &current, &interrupted, calls), &interrupted);
        EXPECT_EQ(calls.end_span, 1u) << "the entry sees the same frame and never unlocks";
        EXPECT_EQ(calls.park, 0u);
        EXPECT_EQ(calls.enter, 0u);
        EXPECT_EQ(booked, nullptr) << "the booking outlived the exit";
    }

    TEST(IsrResume, a_booking_onto_another_context_parks_the_frame_and_leaves_the_span_to_the_entry)
    {
        Frame interrupted = {};
        Frame saved = {};
        Ctx current = {};
        Ctx next = {reinterpret_cast<uintptr_t>(&saved)};
        Ctx* booked = &next;
        Calls calls;
        EXPECT_EQ(resume(booked, &current, &interrupted, calls), &saved);
        EXPECT_EQ(calls.end_span, 0u);
        EXPECT_EQ(calls.park, 1u);
        EXPECT_EQ(calls.parked, &current);
        EXPECT_EQ(calls.parked_frame, &interrupted);
        EXPECT_EQ(calls.enter, 1u);
        EXPECT_EQ(calls.entered, &next);
        EXPECT_EQ(booked, nullptr);
    }

    TEST(IsrResume, a_booking_from_no_context_parks_nothing)
    {
        Frame interrupted = {};
        Frame saved = {};
        Ctx next = {reinterpret_cast<uintptr_t>(&saved)};
        Ctx* booked = &next;
        Calls calls;
        EXPECT_EQ(resume(booked, nullptr, &interrupted, calls), &saved);
        EXPECT_EQ(calls.park, 0u);
        EXPECT_EQ(calls.enter, 1u);
        EXPECT_EQ(calls.end_span, 0u);
    }
}
