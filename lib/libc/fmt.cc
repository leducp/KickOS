// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/libc/fmt.h>

#include <stdint.h>

namespace
{

    struct Sink
    {
        char* buf;
        size_t size;  // capacity including the reserved NUL slot
        size_t total; // chars that would be written (excludes NUL)

        void put(char c)
        {
            if (size > 0 && total < size - 1)
            {
                buf[total] = c;
            }
            total++;
        }
    };

    void emit_str(Sink& s, char const* str)
    {
        if (str == nullptr)
        {
            str = "(null)";
        }
        while (*str != '\0')
        {
            s.put(*str++);
        }
    }

    // Unsigned integer in the given base (10 or 16). `upper` unused (lowercase hex).
    void emit_uint(Sink& s, uint64_t v, unsigned base)
    {
        char tmp[24];
        int n = 0;
        if (v == 0)
        {
            tmp[n++] = '0';
        }
        else
        {
            while (v != 0)
            {
                unsigned d = static_cast<unsigned>(v % base);
                char c = '0';
                if (d < 10)
                {
                    c = static_cast<char>('0' + d);
                }
                else
                {
                    c = static_cast<char>('a' + (d - 10));
                }
                tmp[n++] = c;
                v /= base;
            }
        }
        while (n > 0)
        {
            s.put(tmp[--n]);
        }
    }

    void emit_int(Sink& s, int64_t v)
    {
        if (v < 0)
        {
            s.put('-');
            emit_uint(s, static_cast<uint64_t>(-(v + 1)) + 1, 10); // avoid INT64_MIN UB
        }
        else
        {
            emit_uint(s, static_cast<uint64_t>(v), 10);
        }
    }

}

extern "C" int kvsnprintf(char* buf, size_t size, char const* fmt, va_list ap)
{
    Sink s{buf, size, 0};

    for (char const* p = fmt; *p != '\0'; p++)
    {
        if (*p != '%')
        {
            s.put(*p);
            continue;
        }
        p++;
        // length modifiers
        int longness = 0; // 0=int, 1=long, 2=long long, 3=size_t
        while (*p == 'l')
        {
            longness++;
            p++;
        }
        if (*p == 'z')
        {
            longness = 3;
            p++;
        }

        switch (*p)
        {
            case 's':
            {
                emit_str(s, va_arg(ap, char const*));
                break;
            }
            case 'c':
            {
                s.put(static_cast<char>(va_arg(ap, int)));
                break;
            }
            case '%':
            {
                s.put('%');
                break;
            }
            case 'd':
            case 'i':
            {
                int64_t v;
                if (longness >= 2)
                {
                    v = va_arg(ap, long long);
                }
                else if (longness == 1)
                {
                    v = va_arg(ap, long);
                }
                else
                {
                    v = va_arg(ap, int);
                }
                emit_int(s, v);
                break;
            }
            case 'u':
            {
                uint64_t v;
                if (longness == 3)
                {
                    v = va_arg(ap, size_t);
                }
                else if (longness >= 2)
                {
                    v = va_arg(ap, unsigned long long);
                }
                else if (longness == 1)
                {
                    v = va_arg(ap, unsigned long);
                }
                else
                {
                    v = va_arg(ap, unsigned);
                }
                emit_uint(s, v, 10);
                break;
            }
            case 'x':
            {
                uint64_t v;
                if (longness == 3)
                {
                    v = va_arg(ap, size_t);
                }
                else if (longness >= 2)
                {
                    v = va_arg(ap, unsigned long long);
                }
                else if (longness == 1)
                {
                    v = va_arg(ap, unsigned long);
                }
                else
                {
                    v = va_arg(ap, unsigned);
                }
                emit_uint(s, v, 16);
                break;
            }
            case 'p':
            {
                void* ptr = va_arg(ap, void*);
                s.put('0');
                s.put('x');
                emit_uint(s, reinterpret_cast<uintptr_t>(ptr), 16);
                break;
            }
            case '\0':
            {
                p--; // trailing '%' : stop cleanly
                break;
            }
            default:
            {
                s.put('%');
                s.put(*p);
                break;
            }
        }
    }

    if (s.size > 0)
    {
        size_t term = s.total;
        if (term > s.size - 1)
        {
            term = s.size - 1;
        }
        s.buf[term] = '\0';
    }
    return static_cast<int>(s.total);
}

extern "C" int ksnprintf(char* buf, size_t size, char const* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
