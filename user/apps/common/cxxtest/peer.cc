// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// cxxtest's peer, a program of its own: its constructors ran in its image before main, and a
// throw unwinds on its own stack. The exit status is the verdict.

#include <stdexcept>

#include <kickos/kos.h>

#include "peer.h"

namespace
{
    // Read through a volatile, so the word is constructed at the program's startup.
    volatile int g_ctor_seed = 41;
    int const g_ctor_word = g_ctor_seed + 1;

    struct PeerError : std::runtime_error
    {
        explicit PeerError(int code)
            : std::runtime_error("peer error")
            , code_(code)
        {
        }
        int code_;
    };
}

int main(int argc, char**)
{
    int status = 0;
    if (g_ctor_word != 42)
    {
        status |= CXXTEST_PEER_CTOR_BAD;
    }
    // An argument asks for the constructor check alone, which fits the least stack.
    if (argc > 1)
    {
        return status;
    }
    int caught = 0;
    try
    {
        throw PeerError(7);
    }
    catch (PeerError const& e)
    {
        caught = e.code_;
    }
    if (caught != 7)
    {
        status |= CXXTEST_PEER_CAUGHT_BAD;
    }
    return status;
}
