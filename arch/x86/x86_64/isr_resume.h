// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Which frame an interrupt the kernel handled resumes, from the switch its passes booked.
// trap_x86_64.S ends the kernel lock span only when the frame returned is not the one it passed,
// so a booking back onto the interrupted context ends the span here.

#ifndef KICKOS_ARCH_X86_X86_64_ISR_RESUME_H
#define KICKOS_ARCH_X86_X86_64_ISR_RESUME_H

namespace kickos::x86_64
{
    // `booked` is consumed. `end_span()` ends the lock span, `park(current, frame)` saves the
    // interrupted context into `frame` and `enter(to)` makes `to` current; the frame returned for
    // a new context is its own saved one.
    template <typename Ctx, typename Frame, typename EndSpan, typename Park, typename Enter>
    inline Frame* isr_resume(Ctx*& booked, Ctx* current, Frame* frame, EndSpan end_span, Park park,
                             Enter enter)
    {
        if (booked == nullptr)
        {
            return frame;
        }
        Ctx* const to = booked;
        booked = nullptr;
        if (to == current)
        {
            end_span();
            return frame;
        }
        if (current != nullptr)
        {
            park(current, frame);
        }
        enter(to);
        return reinterpret_cast<Frame*>(to->sp);
    }
}

#endif
