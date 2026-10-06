// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 witness of the console's transmission-complete wait. arch_console_flush_sync returns
// once TXFIFO_CNT is 0 and FSM_STATUS.ST_UTX_OUT reads the idle encoding, which the C6 TRM does
// not print and the kernel takes from the ESP32's. This image loads a line straight into UART0's
// FIFO, waits for the FIFO's last byte to leave it, calls that flush, and the moment it returns
// takes the TX pad from the UART and holds it at the idle level. A frame still shifting then
// loses its remaining zero bits, so the line's last byte reaches the wire whole only where the
// flush waited for it. It also times the wait against the measured frame and reads the
// transmitter's state once the line has been quiet for 10 ms.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU
#error "c6txidle drives UART0 and the IO MUX directly: build the board's flat variant"
#endif

extern "C" void arch_console_flush_sync(void);

namespace
{
    // Mirrored from arch/riscv/chip/esp32c6/regs/ (uart.h, gpio.h, io_mux.h) and the chip file.
    constexpr uintptr_t UART0 = 0x60000000u;
    constexpr uintptr_t UART_FIFO = UART0 + 0x00u;
    constexpr uintptr_t UART_STATUS = UART0 + 0x1Cu;   // TXFIFO_CNT [23:16]
    constexpr uintptr_t UART_IDLE_CONF = UART0 + 0x48u; // TX_IDLE_NUM [19:10]
    constexpr uintptr_t UART_FSM = UART0 + 0x70u;       // ST_UTX_OUT [3:0]
    constexpr uint32_t TXFIFO_CNT_S = 16u;
    constexpr uint32_t TXFIFO_CNT_MASK = 0xFFu;
    constexpr uint32_t TXFIFO_LIMIT = 126u;
    constexpr uint32_t TX_IDLE_NUM_S = 10u;
    constexpr uint32_t TX_IDLE_NUM_MASK = 0x3FFu;
    constexpr uint32_t ST_UTX_OUT_MASK = 0xFu;
    constexpr uint32_t ST_UTX_OUT_IDLE = 0u; // the encoding under test

    constexpr uint32_t TX_PIN = 16u; // U0TXD
    constexpr uint32_t TX_BIT = 1u << TX_PIN;
    constexpr uintptr_t GPIO = 0x60091000u;
    constexpr uintptr_t GPIO_OUT = GPIO + 0x04u;
    constexpr uintptr_t GPIO_OUT_W1TS = GPIO + 0x08u;
    constexpr uintptr_t GPIO_OUT_W1TC = GPIO + 0x0Cu;
    constexpr uintptr_t GPIO_ENABLE = GPIO + 0x20u;
    constexpr uintptr_t GPIO_ENABLE_W1TS = GPIO + 0x24u;
    constexpr uintptr_t GPIO_ENABLE_W1TC = GPIO + 0x28u;
    constexpr uintptr_t GPIO_TX_OUT_SEL = GPIO + 0x554u + 4u * TX_PIN; // FUNCn_OUT_SEL_CFG
    constexpr uint32_t OUT_SEL_SIMPLE = 128u;                          // GPIO_OUT drives the pad
    constexpr uintptr_t IO_MUX_TX = 0x60090000u + 0x04u + 4u * TX_PIN;
    constexpr uint32_t MCU_SEL_MASK = 7u << 12;
    constexpr uint32_t MCU_SEL_GPIO = 1u << 12; // the pad follows the GPIO matrix

    // The console ring refills the FIFO within microseconds of it draining, so a FIFO empty this
    // long means the ring is empty too; and it outlasts TX_IDLE_NUM's ceiling of 1023 bit times
    // at 115200 baud, so the transmitter has idled by its end.
    constexpr uint64_t QUIET_NS = 10000000u;
    constexpr uint64_t QUIET_GIVE_UP_NS = 2000000000u;
    constexpr uint64_t HOLD_NS = 2000000u;

    char const TAIL[] = "[c6txidle] TAIL 0123456789abcdef0123456789abcdef <<<TXIDLE-END>>>";

    uint32_t r32(uintptr_t a)
    {
        return *reinterpret_cast<uint32_t volatile*>(a);
    }

    void w32(uintptr_t a, uint32_t v)
    {
        *reinterpret_cast<uint32_t volatile*>(a) = v;
    }

    uint32_t tx_queued()
    {
        return (r32(UART_STATUS) >> TXFIFO_CNT_S) & TXFIFO_CNT_MASK;
    }

    uint32_t tx_state()
    {
        return r32(UART_FSM) & ST_UTX_OUT_MASK;
    }

    bool console_quiet()
    {
        uint64_t const give_up = kos_clock_now() + QUIET_GIVE_UP_NS;
        uint64_t empty_since = kos_clock_now();
        while (kos_clock_now() < give_up)
        {
            uint64_t const now = kos_clock_now();
            if (tx_queued() != 0)
            {
                empty_since = now;
            }
            else if (now - empty_since >= QUIET_NS)
            {
                return true;
            }
        }
        return false;
    }

    void spin_ns(uint64_t ns)
    {
        uint64_t const until = kos_clock_now() + ns;
        while (kos_clock_now() < until)
        {
        }
    }
}

int main(int, char**)
{
    kos::print("[c6txidle] UART0 TX idle witness: a line loaded into the FIFO, the console flush,\n");
    kos::print("[c6txidle] then the TX pad taken from the UART and held idle. The TAIL line below\n");
    kos::print("[c6txidle] must end in its sentinel whole; a cut last byte is a flush that\n");
    kos::print("[c6txidle] returned with the frame still shifting.\n");
    if (not console_quiet())
    {
        kos::print("[c6txidle] ERROR: the console FIFO never stayed empty\n");
        return 1;
    }
    uint32_t const quiet_state = tx_state();
    uint32_t const idle_bits = (r32(UART_IDLE_CONF) >> TX_IDLE_NUM_S) & TX_IDLE_NUM_MASK;

    uint32_t const mux = r32(IO_MUX_TX);
    uint32_t const out_sel = r32(GPIO_TX_OUT_SEL);
    uint32_t const out = r32(GPIO_OUT) & TX_BIT;
    uint32_t const enable = r32(GPIO_ENABLE) & TX_BIT;
    // The pad switches on ONE store below: the IO MUX select where the ROM routes U0TXD
    // directly, the matrix select where it routes it through the matrix.
    bool const via_matrix = (mux & MCU_SEL_MASK) == MCU_SEL_GPIO;
    w32(GPIO_OUT_W1TS, TX_BIT);
    w32(GPIO_ENABLE_W1TS, TX_BIT);
    if (not via_matrix)
    {
        w32(GPIO_TX_OUT_SEL, OUT_SEL_SIMPLE);
    }

    size_t const n = sizeof(TAIL) - 1u;
    if (n > TXFIFO_LIMIT)
    {
        kos::print("[c6txidle] ERROR: the TAIL line is longer than the FIFO\n");
        return 1;
    }
    for (size_t i = 0; i < n; i++)
    {
        w32(UART_FIFO, static_cast<uint8_t>(TAIL[i]));
    }
    uint64_t const loaded = kos_clock_now();
    while (tx_queued() != 0)
    {
    }
    uint64_t const emptied = kos_clock_now();
    arch_console_flush_sync();
    if (via_matrix)
    {
        w32(GPIO_TX_OUT_SEL, OUT_SEL_SIMPLE);
    }
    else
    {
        w32(IO_MUX_TX, (mux & ~MCU_SEL_MASK) | MCU_SEL_GPIO);
    }
    uint64_t const returned = kos_clock_now();
    uint32_t const return_state = tx_state();

    spin_ns(HOLD_NS);
    if (via_matrix)
    {
        w32(GPIO_TX_OUT_SEL, out_sel);
    }
    else
    {
        w32(IO_MUX_TX, mux);
        w32(GPIO_TX_OUT_SEL, out_sel);
    }
    if (out == 0)
    {
        w32(GPIO_OUT_W1TC, TX_BIT);
    }
    if (enable == 0)
    {
        w32(GPIO_ENABLE_W1TC, TX_BIT);
    }

    // The FIFO's count falls to 0 as its last byte enters the shifter: n - 1 frames after the
    // first did.
    uint64_t const frame_ns = (emptied - loaded) / (n - 1u);
    uint64_t const held_ns = returned - emptied;
    // The transmitter may count TX_IDLE_NUM bit times after the stop bit before it idles; past
    // twice that, the flush ran to its spin bound instead.
    uint64_t const late_ns = frame_ns * (2u + (2u * idle_bits) / 10u);
    char line[160];
    ksnprintf(line, sizeof(line),
              "\n[c6txidle] frame %lu ns over %u bytes, flush held %lu ns past the FIFO's last"
              " byte (late past %lu ns)\n",
              static_cast<unsigned long>(frame_ns), static_cast<unsigned>(n),
              static_cast<unsigned long>(held_ns), static_cast<unsigned long>(late_ns));
    kos::print(line);
    ksnprintf(line, sizeof(line),
              "[c6txidle] ST_UTX_OUT %u after 10 ms quiet, %u at the flush's return; idle"
              " is %u\n",
              static_cast<unsigned>(quiet_state), static_cast<unsigned>(return_state),
              static_cast<unsigned>(ST_UTX_OUT_IDLE));
    kos::print(line);
    if (quiet_state != ST_UTX_OUT_IDLE or return_state != ST_UTX_OUT_IDLE)
    {
        kos::print("[c6txidle] FAIL the transmitter does not idle at the encoding the flush waits for\n");
        return 1;
    }
    // Half a frame: both timestamps trail their events by a system call.
    if (held_ns < frame_ns / 2u)
    {
        kos::print("[c6txidle] FAIL the flush returned before the last frame could finish\n");
        return 1;
    }
    if (held_ns > late_ns)
    {
        kos::print("[c6txidle] FAIL the flush returned late, at its spin bound\n");
        return 1;
    }
    kos::print("[c6txidle] PASS the flush returned once the last frame had left\n");
    return 0;
}
