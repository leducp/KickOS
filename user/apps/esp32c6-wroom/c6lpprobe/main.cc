// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// M9.9 silicon bring-up witness: prove one flash starts the LP core and that it
// writes a word in HP SRAM. Restricted to the flat diagnostic image by CMake.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

extern "C" uint32_t kickos_c6_lp_marker[28];

namespace
{
    constexpr uint32_t EXPECTED = 0x4C503939u;
}

int main(int, char**)
{
    for (uint32_t spin = 0; spin < 10000000u; spin++)
    {
        volatile uint32_t const* const live = kickos_c6_lp_marker;
        if (live[0] == EXPECTED and live[22] == 0x8000001eu)
        {
            char line[96];
            ksnprintf(line, sizeof(line), "c6lpprobe: payload IRQ %x source %x status %x fault %x\n",
                      static_cast<unsigned>(live[22]), static_cast<unsigned>(live[23]),
                      static_cast<unsigned>(live[24]), static_cast<unsigned>(live[10]));
            kos::print(line);
            ksnprintf(line, sizeof(line), "c6lpprobe: LP APM M1 %x/%x/%x\n",
                      static_cast<unsigned>(live[25]), static_cast<unsigned>(live[26]),
                      static_cast<unsigned>(live[27]));
            kos::print(line);
            ksnprintf(line, sizeof(line), "c6lpprobe: LP APM %x/%x/%x LP APM0 %x/%x/%x\n",
                      static_cast<unsigned>(live[15]), static_cast<unsigned>(live[16]),
                      static_cast<unsigned>(live[17]), static_cast<unsigned>(live[18]),
                      static_cast<unsigned>(live[19]), static_cast<unsigned>(live[20]));
            kos::print(line);
            ksnprintf(line, sizeof(line), "c6lpprobe: HP PMU raw %x LP raw %x\n",
                      static_cast<unsigned>(live[6]), static_cast<unsigned>(live[7]));
            kos::print(line);
            if (live[10] == 0 and (live[23] & 0x20u) != 0
                and (live[24] & 0x80000000u) != 0
                and (live[6] & 0x20000000u) != 0)
            {
                kos::print("c6lpprobe: PASS bidirectional PMU doorbells\n");
                return 0;
            }
            return 2;
        }
    }
    char line[128];
    volatile uint32_t const* const marker = kickos_c6_lp_marker;
    ksnprintf(line, sizeof(line), "c6lpprobe: timeout marker %x code %x pwr1 %x mux %x pwr0 %x local %x\n",
              static_cast<unsigned>(marker[0]),
              static_cast<unsigned>(marker[1]),
              static_cast<unsigned>(marker[2]),
              static_cast<unsigned>(marker[3]),
              static_cast<unsigned>(marker[4]),
              static_cast<unsigned>(marker[5]));
    kos::print(line);
    ksnprintf(line, sizeof(line), "c6lpprobe: irq %x source %x\n",
              static_cast<unsigned>(marker[22]), static_cast<unsigned>(marker[23]));
    kos::print(line);
    ksnprintf(line, sizeof(line), "c6lpprobe: lp-apm %x/%x/%x lp-apm0 %x/%x/%x after %x\n",
              static_cast<unsigned>(marker[15]),
              static_cast<unsigned>(marker[16]),
              static_cast<unsigned>(marker[17]),
              static_cast<unsigned>(marker[18]),
              static_cast<unsigned>(marker[19]),
              static_cast<unsigned>(marker[20]),
              static_cast<unsigned>(marker[21]));
    kos::print(line);
    ksnprintf(line, sizeof(line), "c6lpprobe: mcause %x mepc %x mtval %x\n",
              static_cast<unsigned>(marker[10]),
              static_cast<unsigned>(marker[11]),
              static_cast<unsigned>(marker[12]));
    kos::print(line);
    ksnprintf(line, sizeof(line), "c6lpprobe: pmuraw %x lpraw %x main %x power %x apm %x/%x\n",
              static_cast<unsigned>(marker[6]),
              static_cast<unsigned>(marker[7]),
              static_cast<unsigned>(marker[8]),
              static_cast<unsigned>(marker[9]),
              static_cast<unsigned>(marker[13]),
              static_cast<unsigned>(marker[14]));
    kos::print(line);
    return 2;
}
