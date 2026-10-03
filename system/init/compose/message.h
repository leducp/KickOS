// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The init's one-line text, built without libc: a panic's message or a diagnostic's line.

#ifndef KICKOS_SYSTEM_INIT_COMPOSE_MESSAGE_H
#define KICKOS_SYSTEM_INIT_COMPOSE_MESSAGE_H

#include <stddef.h>

namespace kickos::init
{
    // Text cut at its buffer.
    class Message
    {
    public:
        static constexpr size_t CAPACITY = 160;

        Message& add(char const* text);
        // `value` in decimal.
        Message& add(int value);
        // The text so far, terminated.
        char const* text();
        size_t length() const;
        // Empties it for the next text.
        void clear();

    private:
        char text_[CAPACITY];
        size_t length_ = 0;
    };
}

#endif
