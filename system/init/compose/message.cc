// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "message.h"

#include <stdint.h>

namespace kickos::init
{
    Message& Message::add(char const* text)
    {
        for (size_t i = 0; text[i] != '\0' and length_ + 1u < sizeof(text_); i++)
        {
            text_[length_] = text[i];
            length_++;
        }
        return *this;
    }

    Message& Message::add(int value)
    {
        uint32_t magnitude = static_cast<uint32_t>(value);
        if (value < 0)
        {
            add("-");
            magnitude = 0u - magnitude;
        }
        char digits[10];
        size_t count = 0;
        do
        {
            digits[count] = static_cast<char>('0' + (magnitude % 10u));
            count++;
            magnitude = magnitude / 10u;
        }
        while (magnitude != 0u);
        char one[2] = {0, 0};
        while (count > 0u)
        {
            count--;
            one[0] = digits[count];
            add(one);
        }
        return *this;
    }

    size_t Message::length() const
    {
        return length_;
    }

    void Message::clear()
    {
        length_ = 0;
    }

    char const* Message::text()
    {
        text_[length_] = '\0';
        return text_;
    }
}
