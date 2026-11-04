// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The __atomic_* and __sync_* calls GCC emits for a 1-, 2- or 4-byte read-modify-write where user
// mode has no compare-and-swap instruction, each a loop over kickos_user_cas: atomic against the
// threads of its own kernel instance, and no further. A 1- or 2-byte object is updated through the
// aligned 32-bit word that holds it. No 8-byte form is defined.
//
// No read-modify-write builtin may appear in this file: each would compile to a call of itself.

#include <kickos/sys/user_cas.h>

#include <stdint.h>

#define KICKOS_UA_STR2(x) #x
#define KICKOS_UA_STR(x) KICKOS_UA_STR2(x)
#define KICKOS_UA_NAME(name) KICKOS_UA_STR(__USER_LABEL_PREFIX__) #name

namespace
{
    using Word = uint32_t __attribute__((__may_alias__));

    void fence_before(int order)
    {
        switch (order)
        {
            case __ATOMIC_RELAXED:
            case __ATOMIC_CONSUME:
            case __ATOMIC_ACQUIRE:
            {
                break;
            }
            case __ATOMIC_RELEASE:
            case __ATOMIC_ACQ_REL:
            {
                __atomic_thread_fence(__ATOMIC_RELEASE);
                break;
            }
            default:
            {
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                break;
            }
        }
    }

    void fence_after(int order)
    {
        switch (order)
        {
            case __ATOMIC_RELAXED:
            case __ATOMIC_RELEASE:
            {
                break;
            }
            case __ATOMIC_CONSUME:
            case __ATOMIC_ACQUIRE:
            case __ATOMIC_ACQ_REL:
            {
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                break;
            }
            default:
            {
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                break;
            }
        }
    }

    // The object at p as a field of its aligned word.
    struct Lane
    {
        Word* word;
        uint32_t shift;
        uint32_t mask;
    };

    template <typename T>
    Lane lane_of(void volatile* p)
    {
        uintptr_t const at = reinterpret_cast<uintptr_t>(p);
        uintptr_t const off = at & 3u;
        Lane lane;
        lane.word = reinterpret_cast<Word*>(at - off);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        lane.shift = static_cast<uint32_t>((4u - sizeof(T) - off) * 8u);
#else
        lane.shift = static_cast<uint32_t>(off * 8u);
#endif
        lane.mask = static_cast<uint32_t>(static_cast<T>(~static_cast<T>(0))) << lane.shift;
        return lane;
    }

    template <typename T>
    T field(Lane const& lane, uint32_t w)
    {
        return static_cast<T>((w & lane.mask) >> lane.shift);
    }

    uint32_t cas(Lane const& lane, uint32_t expected, uint32_t desired)
    {
        return kickos_user_cas(reinterpret_cast<uint32_t*>(lane.word), expected, desired);
    }

    // Stores op(old) over the object at p and answers old; the value stored is left in now.
    template <typename T, typename Op>
    T modify(void volatile* p, int order, Op op, T& now)
    {
        Lane const lane = lane_of<T>(p);
        fence_before(order);
        uint32_t w = __atomic_load_n(lane.word, __ATOMIC_RELAXED);
        T old = 0;
        while (true)
        {
            old = field<T>(lane, w);
            now = op(old);
            uint32_t const next
                = (w & ~lane.mask) | ((static_cast<uint32_t>(now) << lane.shift) & lane.mask);
            uint32_t const seen = cas(lane, w, next);
            if (seen == w)
            {
                break;
            }
            w = seen;
        }
        fence_after(order);
        return old;
    }

    template <typename T, typename Op>
    T fetch(void volatile* p, int order, Op op)
    {
        T now = 0;
        return modify<T>(p, order, op, now);
    }

    template <typename T, typename Op>
    T then(void volatile* p, int order, Op op)
    {
        T now = 0;
        (void)modify<T>(p, order, op, now);
        return now;
    }

    // A neighbour's store in the same word fails the word's compare without the object changing,
    // and is retried: the compare answers false only on the object's own value.
    template <typename T>
    bool compare_exchange(void volatile* p, T* expected, T desired, int success, int failure)
    {
        Lane const lane = lane_of<T>(p);
        T const want = *expected;
        fence_before(success);
        uint32_t w = __atomic_load_n(lane.word, __ATOMIC_RELAXED);
        while (true)
        {
            T const held = field<T>(lane, w);
            if (held != want)
            {
                *expected = held;
                fence_after(failure);
                return false;
            }
            uint32_t const next
                = (w & ~lane.mask) | ((static_cast<uint32_t>(desired) << lane.shift) & lane.mask);
            uint32_t const seen = cas(lane, w, next);
            if (seen == w)
            {
                fence_after(success);
                return true;
            }
            w = seen;
        }
    }

    template <typename T>
    struct Add
    {
        T v;
        T operator()(T old) const { return static_cast<T>(old + v); }
    };
    template <typename T>
    struct Sub
    {
        T v;
        T operator()(T old) const { return static_cast<T>(old - v); }
    };
    template <typename T>
    struct And
    {
        T v;
        T operator()(T old) const { return static_cast<T>(old & v); }
    };
    template <typename T>
    struct Or
    {
        T v;
        T operator()(T old) const { return static_cast<T>(old | v); }
    };
    template <typename T>
    struct Xor
    {
        T v;
        T operator()(T old) const { return static_cast<T>(old ^ v); }
    };
    template <typename T>
    struct Nand
    {
        T v;
        T operator()(T old) const { return static_cast<T>(~(old & v)); }
    };
    template <typename T>
    struct Set
    {
        T v;
        T operator()(T) const { return v; }
    };
}

// GCC declares each of these names as a builtin, so each is defined under another name and
// given the builtin's as its assembler name.
#define KICKOS_UA_OP(op, Op, n, T)                                                                \
    extern "C" T kickos_ua_fetch_##op##_##n(void volatile* p, T v, int order)                     \
        __asm__(KICKOS_UA_NAME(__atomic_fetch_##op##_##n));                                       \
    extern "C" T kickos_ua_fetch_##op##_##n(void volatile* p, T v, int order)                     \
    {                                                                                             \
        return fetch<T>(p, order, Op<T>{v});                                                      \
    }                                                                                             \
    extern "C" T kickos_ua_##op##_fetch_##n(void volatile* p, T v, int order)                     \
        __asm__(KICKOS_UA_NAME(__atomic_##op##_fetch_##n));                                       \
    extern "C" T kickos_ua_##op##_fetch_##n(void volatile* p, T v, int order)                     \
    {                                                                                             \
        return then<T>(p, order, Op<T>{v});                                                       \
    }                                                                                             \
    extern "C" T kickos_ua_sync_fetch_##op##_##n(void volatile* p, T v)                           \
        __asm__(KICKOS_UA_NAME(__sync_fetch_and_##op##_##n));                                     \
    extern "C" T kickos_ua_sync_fetch_##op##_##n(void volatile* p, T v)                           \
    {                                                                                             \
        return fetch<T>(p, __ATOMIC_SEQ_CST, Op<T>{v});                                           \
    }                                                                                             \
    extern "C" T kickos_ua_sync_##op##_fetch_##n(void volatile* p, T v)                           \
        __asm__(KICKOS_UA_NAME(__sync_##op##_and_fetch_##n));                                     \
    extern "C" T kickos_ua_sync_##op##_fetch_##n(void volatile* p, T v)                           \
    {                                                                                             \
        return then<T>(p, __ATOMIC_SEQ_CST, Op<T>{v});                                            \
    }

#define KICKOS_UA_SIZE(n, T)                                                                      \
    KICKOS_UA_OP(add, Add, n, T)                                                                  \
    KICKOS_UA_OP(sub, Sub, n, T)                                                                  \
    KICKOS_UA_OP(and, And, n, T)                                                                  \
    KICKOS_UA_OP(or, Or, n, T)                                                                    \
    KICKOS_UA_OP(xor, Xor, n, T)                                                                  \
    KICKOS_UA_OP(nand, Nand, n, T)                                                                \
    extern "C" bool kickos_ua_compare_exchange_##n(void volatile* p, void* expected, T desired,   \
                                                   int success, int failure)                      \
        __asm__(KICKOS_UA_NAME(__atomic_compare_exchange_##n));                                   \
    extern "C" bool kickos_ua_compare_exchange_##n(void volatile* p, void* expected, T desired,   \
                                                   int success, int failure)                      \
    {                                                                                             \
        return compare_exchange<T>(p, static_cast<T*>(expected), desired, success, failure);      \
    }                                                                                             \
    extern "C" T kickos_ua_sync_val_cas_##n(void volatile* p, T expected, T desired)              \
        __asm__(KICKOS_UA_NAME(__sync_val_compare_and_swap_##n));                                 \
    extern "C" T kickos_ua_sync_val_cas_##n(void volatile* p, T expected, T desired)              \
    {                                                                                             \
        T seen = expected;                                                                        \
        (void)compare_exchange<T>(p, &seen, desired, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);         \
        return seen;                                                                              \
    }                                                                                             \
    extern "C" bool kickos_ua_sync_bool_cas_##n(void volatile* p, T expected, T desired)          \
        __asm__(KICKOS_UA_NAME(__sync_bool_compare_and_swap_##n));                                \
    extern "C" bool kickos_ua_sync_bool_cas_##n(void volatile* p, T expected, T desired)          \
    {                                                                                             \
        T seen = expected;                                                                        \
        return compare_exchange<T>(p, &seen, desired, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);        \
    }                                                                                             \
    extern "C" T kickos_ua_sync_lock_test_and_set_##n(void volatile* p, T v)                      \
        __asm__(KICKOS_UA_NAME(__sync_lock_test_and_set_##n));                                    \
    extern "C" T kickos_ua_sync_lock_test_and_set_##n(void volatile* p, T v)                      \
    {                                                                                             \
        return fetch<T>(p, __ATOMIC_ACQUIRE, Set<T>{v});                                          \
    }

KICKOS_UA_SIZE(1, uint8_t)
KICKOS_UA_SIZE(2, uint16_t)
KICKOS_UA_SIZE(4, uint32_t)

// RX exchanges with its xchg instruction, which GCC emits inline.
#if not defined(__RX__)
#define KICKOS_UA_EXCHANGE(n, T)                                                                  \
    extern "C" T kickos_ua_exchange_##n(void volatile* p, T v, int order)                         \
        __asm__(KICKOS_UA_NAME(__atomic_exchange_##n));                                           \
    extern "C" T kickos_ua_exchange_##n(void volatile* p, T v, int order)                         \
    {                                                                                             \
        return fetch<T>(p, order, Set<T>{v});                                                     \
    }

KICKOS_UA_EXCHANGE(1, uint8_t)
KICKOS_UA_EXCHANGE(2, uint16_t)
KICKOS_UA_EXCHANGE(4, uint32_t)

extern "C" bool kickos_ua_test_and_set(void volatile* p, int order)
    __asm__(KICKOS_UA_NAME(__atomic_test_and_set));
extern "C" bool kickos_ua_test_and_set(void volatile* p, int order)
{
    return fetch<uint8_t>(p, order, Set<uint8_t>{__GCC_ATOMIC_TEST_AND_SET_TRUEVAL}) != 0u;
}
#endif
