// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The atomics library calls of user/src/user_atomic.cc, by their assembler names, over the sim's
// compare-and-swap: each operation's answer and stored value at every width and offset, a peer's
// store landing between a loop's load and its compare, and threads racing on one word.

#include <gtest/gtest.h>

#include <stdint.h>

#include <thread>
#include <vector>

#include <kickos/sys/user_cas.h>

#define UA_DECL(ret, name, ...) extern "C" ret ua_##name(__VA_ARGS__) __asm__(#name);

#define UA_SIZE(n, T)                                                                             \
    UA_DECL(T, __atomic_fetch_add_##n, void volatile*, T, int)                                    \
    UA_DECL(T, __atomic_fetch_sub_##n, void volatile*, T, int)                                    \
    UA_DECL(T, __atomic_fetch_and_##n, void volatile*, T, int)                                    \
    UA_DECL(T, __atomic_fetch_or_##n, void volatile*, T, int)                                     \
    UA_DECL(T, __atomic_fetch_xor_##n, void volatile*, T, int)                                    \
    UA_DECL(T, __atomic_fetch_nand_##n, void volatile*, T, int)                                   \
    UA_DECL(T, __atomic_add_fetch_##n, void volatile*, T, int)                                    \
    UA_DECL(T, __atomic_sub_fetch_##n, void volatile*, T, int)                                    \
    UA_DECL(T, __atomic_and_fetch_##n, void volatile*, T, int)                                    \
    UA_DECL(T, __atomic_or_fetch_##n, void volatile*, T, int)                                     \
    UA_DECL(T, __atomic_xor_fetch_##n, void volatile*, T, int)                                    \
    UA_DECL(T, __atomic_nand_fetch_##n, void volatile*, T, int)                                   \
    UA_DECL(T, __atomic_exchange_##n, void volatile*, T, int)                                     \
    UA_DECL(bool, __atomic_compare_exchange_##n, void volatile*, void*, T, int, int)              \
    UA_DECL(T, __sync_fetch_and_add_##n, void volatile*, T)                                       \
    UA_DECL(T, __sync_fetch_and_nand_##n, void volatile*, T)                                      \
    UA_DECL(T, __sync_sub_and_fetch_##n, void volatile*, T)                                       \
    UA_DECL(T, __sync_xor_and_fetch_##n, void volatile*, T)                                       \
    UA_DECL(T, __sync_val_compare_and_swap_##n, void volatile*, T, T)                             \
    UA_DECL(bool, __sync_bool_compare_and_swap_##n, void volatile*, T, T)                         \
    UA_DECL(T, __sync_lock_test_and_set_##n, void volatile*, T)

UA_SIZE(1, uint8_t)
UA_SIZE(2, uint16_t)
UA_SIZE(4, uint32_t)
UA_DECL(bool, __atomic_test_and_set, void volatile*, int)

extern "C" uint32_t __real_kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired);

namespace
{
    // One-shot: stored over the word the next compare names, before that compare reads it.
    uint32_t* g_peer_word = nullptr;
    uint32_t g_peer_value = 0;
    uint32_t g_cas_calls = 0;
}

extern "C" uint32_t __wrap_kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired)
{
    g_cas_calls = g_cas_calls + 1u;
    if (g_peer_word != nullptr and word == g_peer_word)
    {
        g_peer_word = nullptr;
        *word = g_peer_value;
    }
    return __real_kickos_user_cas(word, expected, desired);
}

namespace
{
    // A word whose bytes are distinct, so a write to the wrong lane changes a byte the test reads.
    constexpr uint32_t BACKDROP = 0xa1b2c3d4u;

    union Cell
    {
        uint32_t word;
        uint8_t bytes[4];
        uint16_t halves[2];
    };

    void peer_stores(Cell& c, uint32_t value)
    {
        g_peer_word = &c.word;
        g_peer_value = value;
    }

    // The word c holds with the object of `size` bytes at byte `at` replaced by v.
    uint32_t with(Cell const& c, unsigned at, unsigned size, uint32_t v)
    {
        Cell e = c;
        if (size == 1u)
        {
            e.bytes[at] = static_cast<uint8_t>(v);
        }
        else
        {
            e.halves[at / 2u] = static_cast<uint16_t>(v);
        }
        return e.word;
    }

    class UserAtomic : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            g_peer_word = nullptr;
            g_cas_calls = 0;
        }
    };
}

TEST_F(UserAtomic, word_operations_answer_and_store_their_values)
{
    Cell c{};
    c.word = 10u;
    EXPECT_EQ(ua___atomic_fetch_add_4(&c.word, 5u, __ATOMIC_SEQ_CST), 10u);
    EXPECT_EQ(c.word, 15u);
    EXPECT_EQ(ua___atomic_add_fetch_4(&c.word, 5u, __ATOMIC_RELAXED), 20u);
    EXPECT_EQ(ua___atomic_fetch_sub_4(&c.word, 21u, __ATOMIC_ACQUIRE), 20u);
    EXPECT_EQ(c.word, 0xffffffffu);
    EXPECT_EQ(ua___atomic_sub_fetch_4(&c.word, 0xfffffffeu, __ATOMIC_RELEASE), 1u);
    c.word = 0xf0f0u;
    EXPECT_EQ(ua___atomic_fetch_and_4(&c.word, 0xff00u, __ATOMIC_ACQ_REL), 0xf0f0u);
    EXPECT_EQ(c.word, 0xf000u);
    EXPECT_EQ(ua___atomic_and_fetch_4(&c.word, 0x3000u, __ATOMIC_SEQ_CST), 0x3000u);
    EXPECT_EQ(ua___atomic_fetch_or_4(&c.word, 0x0003u, __ATOMIC_SEQ_CST), 0x3000u);
    EXPECT_EQ(ua___atomic_or_fetch_4(&c.word, 0x0300u, __ATOMIC_SEQ_CST), 0x3303u);
    EXPECT_EQ(ua___atomic_fetch_xor_4(&c.word, 0x1111u, __ATOMIC_SEQ_CST), 0x3303u);
    EXPECT_EQ(ua___atomic_xor_fetch_4(&c.word, 0x1111u, __ATOMIC_SEQ_CST), 0x3303u);
    EXPECT_EQ(ua___atomic_fetch_nand_4(&c.word, 0x0f0fu, __ATOMIC_SEQ_CST), 0x3303u);
    EXPECT_EQ(c.word, ~0x0303u);
    EXPECT_EQ(ua___atomic_nand_fetch_4(&c.word, 0xffffffffu, __ATOMIC_SEQ_CST), 0x0303u);
    EXPECT_EQ(ua___atomic_exchange_4(&c.word, 77u, __ATOMIC_SEQ_CST), 0x0303u);
    EXPECT_EQ(c.word, 77u);
    EXPECT_EQ(ua___sync_fetch_and_add_4(&c.word, 3u), 77u);
    EXPECT_EQ(ua___sync_fetch_and_nand_4(&c.word, 0xffu), 80u);
    EXPECT_EQ(c.word, ~80u);
    c.word = 9u;
    EXPECT_EQ(ua___sync_sub_and_fetch_4(&c.word, 4u), 5u);
    EXPECT_EQ(ua___sync_xor_and_fetch_4(&c.word, 1u), 4u);
    EXPECT_EQ(ua___sync_lock_test_and_set_4(&c.word, 6u), 4u);
    EXPECT_EQ(c.word, 6u);
}

TEST_F(UserAtomic, compare_exchange_stores_only_on_the_expected_value)
{
    Cell c{};
    c.word = 3u;
    uint32_t expected = 4u;
    EXPECT_FALSE(ua___atomic_compare_exchange_4(&c.word, &expected, 9u, __ATOMIC_SEQ_CST,
                                                __ATOMIC_RELAXED));
    EXPECT_EQ(expected, 3u);
    EXPECT_EQ(c.word, 3u);
    EXPECT_TRUE(ua___atomic_compare_exchange_4(&c.word, &expected, 9u, __ATOMIC_ACQ_REL,
                                               __ATOMIC_ACQUIRE));
    EXPECT_EQ(expected, 3u);
    EXPECT_EQ(c.word, 9u);
    EXPECT_EQ(ua___sync_val_compare_and_swap_4(&c.word, 8u, 1u), 9u);
    EXPECT_EQ(c.word, 9u);
    EXPECT_EQ(ua___sync_val_compare_and_swap_4(&c.word, 9u, 1u), 9u);
    EXPECT_EQ(c.word, 1u);
    EXPECT_FALSE(ua___sync_bool_compare_and_swap_4(&c.word, 2u, 5u));
    EXPECT_TRUE(ua___sync_bool_compare_and_swap_4(&c.word, 1u, 5u));
    EXPECT_EQ(c.word, 5u);
}

// Every byte and half-word offset: the object changes as asked, and its word's other bytes keep
// their values.
TEST_F(UserAtomic, a_byte_or_half_operation_touches_its_own_lane_alone)
{
    for (unsigned at = 0; at < 4u; ++at)
    {
        Cell c{};
        c.word = BACKDROP;
        uint8_t const was = c.bytes[at];
        EXPECT_EQ(ua___atomic_fetch_add_1(&c.bytes[at], 0x70u, __ATOMIC_SEQ_CST), was);
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 1u, static_cast<uint8_t>(was + 0x70u)));
        EXPECT_EQ(ua___atomic_sub_fetch_1(&c.bytes[at], 0x71u, __ATOMIC_SEQ_CST),
                  static_cast<uint8_t>(was - 1u));
        EXPECT_EQ(ua___atomic_fetch_nand_1(&c.bytes[at], 0xffu, __ATOMIC_SEQ_CST),
                  static_cast<uint8_t>(was - 1u));
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 1u, static_cast<uint8_t>(~(was - 1u))));
        EXPECT_EQ(ua___atomic_exchange_1(&c.bytes[at], 0x5au, __ATOMIC_SEQ_CST),
                  static_cast<uint8_t>(~(was - 1u)));
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 1u, 0x5au));
        uint8_t expected = 0x5bu;
        EXPECT_FALSE(ua___atomic_compare_exchange_1(&c.bytes[at], &expected, 0x11u,
                                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        EXPECT_EQ(expected, 0x5au);
        EXPECT_TRUE(ua___atomic_compare_exchange_1(&c.bytes[at], &expected, 0x11u,
                                                   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 1u, 0x11u));
        EXPECT_EQ(ua___sync_val_compare_and_swap_1(&c.bytes[at], 0x11u, 0x22u), 0x11u);
        EXPECT_EQ(ua___sync_lock_test_and_set_1(&c.bytes[at], 0x33u), 0x22u);
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 1u, 0x33u));
        c.bytes[at] = 0u;
        EXPECT_FALSE(ua___atomic_test_and_set(&c.bytes[at], __ATOMIC_SEQ_CST));
        EXPECT_TRUE(ua___atomic_test_and_set(&c.bytes[at], __ATOMIC_SEQ_CST));
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 1u, __GCC_ATOMIC_TEST_AND_SET_TRUEVAL));
    }
    for (unsigned at = 0; at < 4u; at += 2u)
    {
        Cell c{};
        c.word = BACKDROP;
        uint16_t const was = c.halves[at / 2u];
        EXPECT_EQ(ua___atomic_fetch_add_2(&c.halves[at / 2u], 0x9000u, __ATOMIC_SEQ_CST), was);
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 2u, static_cast<uint16_t>(was + 0x9000u)));
        EXPECT_EQ(ua___atomic_or_fetch_2(&c.halves[at / 2u], 0x000fu, __ATOMIC_SEQ_CST),
                  static_cast<uint16_t>((was + 0x9000u) | 0x000fu));
        EXPECT_EQ(ua___atomic_exchange_2(&c.halves[at / 2u], 0x1234u, __ATOMIC_SEQ_CST),
                  static_cast<uint16_t>((was + 0x9000u) | 0x000fu));
        uint16_t expected = 0x1234u;
        EXPECT_TRUE(ua___atomic_compare_exchange_2(&c.halves[at / 2u], &expected, 0xbeefu,
                                                   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 2u, 0xbeefu));
        EXPECT_TRUE(ua___sync_bool_compare_and_swap_2(&c.halves[at / 2u], 0xbeefu, 0x0001u));
        EXPECT_EQ(ua___sync_fetch_and_add_2(&c.halves[at / 2u], 0xffffu), 0x0001u);
        EXPECT_EQ(c.word, with(Cell{BACKDROP}, at, 2u, 0u));
    }
}

// A peer's add landing between the load and the compare is kept, and this add lands after it.
TEST_F(UserAtomic, a_peer_store_before_the_compare_is_kept)
{
    Cell c{};
    c.word = 100u;
    peer_stores(c, 140u);
    EXPECT_EQ(ua___atomic_fetch_add_4(&c.word, 1u, __ATOMIC_SEQ_CST), 140u);
    EXPECT_EQ(c.word, 141u);
    EXPECT_EQ(g_cas_calls, 2u);

    c.word = BACKDROP;
    peer_stores(c, with(c, 1u, 1u, 0x07u));
    EXPECT_EQ(ua___atomic_add_fetch_1(&c.bytes[1], 1u, __ATOMIC_SEQ_CST), 0x08u);
    EXPECT_EQ(c.word, with(Cell{BACKDROP}, 1u, 1u, 0x08u));
}

// A neighbour's store changes the word and not the object: the add and the compare both retry,
// keep the neighbour's value, and the compare does not answer false.
TEST_F(UserAtomic, a_neighbour_store_is_retried_and_kept)
{
    Cell c{};
    c.word = BACKDROP;
    uint32_t const neighbour = with(c, 3u, 1u, 0x99u);
    peer_stores(c, neighbour);
    EXPECT_EQ(ua___atomic_fetch_add_1(&c.bytes[0], 2u, __ATOMIC_SEQ_CST), 0xd4u);
    EXPECT_EQ(c.word, with(Cell{neighbour}, 0u, 1u, static_cast<uint8_t>(0xd4u + 2u)));
    EXPECT_EQ(g_cas_calls, 2u);

    c.word = BACKDROP;
    peer_stores(c, with(c, 0u, 2u, 0x4242u));
    uint16_t expected = c.halves[1];
    EXPECT_TRUE(ua___atomic_compare_exchange_2(&c.halves[1], &expected, 0x0badu,
                                               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    EXPECT_EQ(c.halves[0], 0x4242u);
    EXPECT_EQ(c.halves[1], 0x0badu);

    c.word = BACKDROP;
    peer_stores(c, with(c, 2u, 1u, 0x00u));
    EXPECT_TRUE(ua___sync_bool_compare_and_swap_1(&c.bytes[1], 0xc3u, 0x3cu));
    EXPECT_EQ(c.word, with(Cell{with(Cell{BACKDROP}, 2u, 1u, 0x00u)}, 1u, 1u, 0x3cu));
}

// Threads racing through every width, on one word and on the lanes of another: no update is lost.
TEST_F(UserAtomic, racing_threads_lose_no_update)
{
    constexpr unsigned THREADS = 4;
    constexpr uint32_t ROUNDS = 100000;
    Cell c{};
    Cell lanes{};
    std::vector<std::thread> racers;
    for (unsigned t = 0; t < THREADS; ++t)
    {
        racers.emplace_back([&c, &lanes, t]() {
            for (uint32_t i = 0; i < ROUNDS; ++i)
            {
                (void)ua___atomic_fetch_add_4(&c.word, 1u, __ATOMIC_RELAXED);
                (void)ua___atomic_fetch_add_1(&lanes.bytes[t % 2u], 1u, __ATOMIC_SEQ_CST);
                (void)ua___atomic_fetch_sub_2(&lanes.halves[1], 1u, __ATOMIC_ACQ_REL);
            }
        });
    }
    for (std::thread& r : racers)
    {
        r.join();
    }
    EXPECT_EQ(c.word, THREADS * ROUNDS);
    EXPECT_EQ(lanes.bytes[0], static_cast<uint8_t>(2u * ROUNDS));
    EXPECT_EQ(lanes.bytes[1], static_cast<uint8_t>(2u * ROUNDS));
    EXPECT_EQ(lanes.halves[1], static_cast<uint16_t>(0u - THREADS * ROUNDS));
}
