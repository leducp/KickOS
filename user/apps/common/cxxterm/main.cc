// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An exception nothing catches: the terminate handler must name its type and what() before
// it stops the image (tests/integration/check_qemu_cxxterm.sh).

#include <stdexcept>
#include <kickos/kos.h>

namespace
{
    // Out of line, so the throw is not folded into a direct call to terminate.
    [[gnu::noinline]] void raise_uncaught()
    {
        throw std::runtime_error("cxxterm uncaught");
    }
}

int main(int, char**)
{
    kos::print("cxxterm: throwing\n");
    raise_uncaught();
    kos::print("cxxterm: returned from an uncaught throw\n");
    return 0;
}
