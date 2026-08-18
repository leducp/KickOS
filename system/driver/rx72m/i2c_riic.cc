// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RX72M RIICa backend of the I2C class <kickos/driver/i2c.h>: the whole channel bring-up
// plus a polled master engine, for a thread that ALREADY HOLDS one RIIC channel's register
// window. Any of the three channels; the window base is the only thing that names one.
//
// Every function here touches the granted register window, so all of them belong to the one
// thread holding it; there is no locking. The register file is BYTE-mapped and 8-bit access
// only.
//
// POLLED, NOT IRQ-PACED: kos_i2c_bus_config.irq must be KOS_CAP_NONE. RIICa's EEI is a
// grouped source and its IR flag lives in the ICU, which is a kernel-reserved block, so an
// unprivileged holder of a channel window cannot retire a stale pending for it. That also
// leaves one step of sec.43.16.2 out of reach: it asks that the ICU IR flag be cleared before
// ICE is set. What this engine can do it does: ICIER is written 0 before the reset is
// released, so no RIIC source is armed to raise one.
//
// Register facts come from the RX72M Group User's Manual: Hardware (r01uh0804ej0120,
// Rev.1.20) sec.43, via arch/rx/chip/rx72m/regs/riic.h. Four shape the whole file:
//
//   1. THE DEADLINE IS THE ONLY WATCHDOG. ICFER.TMOE is 0 at reset and stays there
//      (docs/design-m5-i2c-seam.md section 3), and every hardware answer to a late driver is
//      to hold SCL low, which stops the bus for every device on it. So every wait is bounded
//      by the caller's deadline and every failure exit goes through bus_release.
//   2. ICMR3.ACKBT ONLY TAKES A WRITE WHEN ICMR3.ACKWP WAS ALREADY 1 IN AN EARLIER STORE
//      (sec.43.2.5 Note 1). ACKWP is raised on its own at the top of a transaction and
//      carried by every ICMR3 store after it.
//   3. READING ICDRR IS WHAT CLOCKS THE NEXT BYTE IN (sec.43.3.4), including the DUMMY read
//      that follows the address. So the NACK ending a read has to be armed one read early
//      and the low-hold one read before that.
//   4. A WHOLE-REGISTER WRITE OF 0 TO ICCR1 PULLS BOTH BUS LINES LOW: it clears SOWP in the
//      same store that clears SCLO and SDAO (sec.43.2.1). Every store here starts from
//      ICCR1_IDLE, which keeps SOWP set and the two output bits protected.

#include <kickos/driver/i2c.h>

#include <kickos/io/mmio.h> // r8
#include <kickos/sys.h>     // kos_periph_enable, kos_periph_clock_hz, kos_clock_now
#include <kickos/sys/errno.h>

#include <regs/riic.h>

#include <stdint.h>

namespace
{
    namespace rr = kickos::rx::reg::riic;

    // kos_i2c_device.prog slots. The rate word packs CKS in bits 2:0, ICBRH in 12:8 and
    // ICBRL in 20:16, which is everything a device's profile amounts to on this controller.
    constexpr unsigned PROG_RATE = 0u;
    constexpr unsigned PROG_RSV = 1u;

    // ICFER as this engine runs it. TMOE stays 0 by ruling; MALE catches a second master and
    // is dropped only for the duration of a bus recovery; NALE and SALE belong to roles this
    // engine does not play, and their being 0 is what makes that recovery safe. NACKE is what
    // makes a NACK stop the transfer instead of clocking on into a dead device. This is also
    // the reset value, and it is written anyway because the reset it follows is ours.
    constexpr uint8_t FUNCTION_ENABLE =
        rr::ICFER_SCLE | rr::ICFER_NFE | rr::ICFER_NACKE | rr::ICFER_MALE;

    // Register reads between two kos_clock_now calls. The clock is a syscall and a flag can
    // turn over in a few bus cycles, so checking the deadline on every pass would cost more
    // than the wait.
    constexpr uint32_t CLOCK_POLL_BATCH = 64u;

    // Bounded spins for the recovery path, which runs with the caller's deadline already
    // spent and must still terminate.
    constexpr uint32_t RECOVER_SPIN = 100000u;
    constexpr unsigned RECOVER_PULSES = 9u;

    // I2C-bus specification rise plus fall maxima, in nanoseconds. The RIICa rate formula
    // (sec.43.2.14) carries tr and tf explicitly, no controller can measure them, and they
    // are set by the board's capacitance and pull-ups; substituting the specification
    // ceiling for the mode makes the reported rate an upper bound on the real one.
    constexpr uint32_t TRF_STANDARD_NS = 1300u;
    constexpr uint32_t TRF_FAST_NS = 600u;
    constexpr uint32_t STANDARD_MAX_HZ = 100000u;
    constexpr uint32_t FAST_MAX_HZ = 400000u;

    // ICBRH/ICBRL register bounds. The floor is the noise filter's: sec.43.2.13 and
    // sec.43.2.14 both require a value at least one above the filter's stage count, and
    // FUNCTION_ENABLE keeps NFE set with ICMR3.NF at its one-stage reset.
    constexpr uint32_t ICBR_MIN = 2u;
    constexpr uint32_t ICBR_MAX = 31u;
    constexpr uint32_t COUNT_MIN = 2u * (ICBR_MIN + 1u);
    constexpr uint32_t COUNT_MAX = 2u * (ICBR_MAX + 1u);

    // The seed profile a bus is brought up on: the slowest this controller reaches. Nothing
    // clocks on it; a transfer applies its own device's profile first.
    constexpr uint32_t SEED_RATE = 7u | (ICBR_MAX << 8) | (ICBR_MAX << 16);

    constexpr uint32_t rate_word(uint32_t cks, uint32_t brh, uint32_t brl)
    {
        return cks | (brh << 8) | (brl << 16);
    }

    uint32_t live_rate_word(uintptr_t win)
    {
        uint32_t const cks =
            (uint32_t)(r8(win + rr::ICMR1) & rr::ICMR1_CKS_MASK) >> rr::ICMR1_CKS_SHIFT;
        return rate_word(cks, r8(win + rr::ICBRH) & rr::ICBR_MASK,
                         r8(win + rr::ICBRL) & rr::ICBR_MASK);
    }

    // A status flag is cleared by writing 0 to a bit that read 1; writing 1 to one that read
    // 0 does not set it, so the read-modify-write is safe (sec.43.2.10).
    void status_clear(uintptr_t win, uint8_t mask)
    {
        uint8_t const s = r8(win + rr::ICSR2);
        r8(win + rr::ICSR2) = (uint8_t)(s & (uint8_t)~mask);
    }

    void icmr3_update(uintptr_t win, uint8_t set, uint8_t clear)
    {
        uint8_t const v = r8(win + rr::ICMR3);
        r8(win + rr::ICMR3) = (uint8_t)((v | set | rr::ICMR3_ACKWP) & (uint8_t)~clear);
    }

    // Wait for any bit of `want` in ICSR2. `fail` names the error flags that abort this
    // particular wait: NACKF is expected on a wait that follows an aborted transfer, and a
    // wait for STOP must not take it as a failure.
    int32_t wait_status(uintptr_t win, uint8_t want, uint8_t fail, uint64_t deadline)
    {
        uint32_t batch = 0u;
        while (true)
        {
            uint8_t const s = r8(win + rr::ICSR2);
            if ((s & fail & rr::ICSR2_AL) != 0u)
            {
                return -KOS_EBUSY;
            }
            if ((s & fail & rr::ICSR2_NACKF) != 0u)
            {
                return -KOS_EIO;
            }
            if ((s & want) != 0u)
            {
                return 0;
            }
            batch++;
            if (batch >= CLOCK_POLL_BATCH)
            {
                batch = 0u;
                if (kos_clock_now() >= deadline)
                {
                    return -KOS_ETIMEDOUT;
                }
            }
        }
    }

    // UM Fig.43.4 fixes both the order and which step is last: ICE=0, IICRST=1, ICE=1, every
    // setting, IICRST=0. The first three are an RIIC reset, which clears BBSY and every flag
    // along with the registers, so this doubles as the way back from a wedged controller.
    void program(uintptr_t win, uint32_t rate)
    {
        r8(win + rr::ICCR1) = rr::ICCR1_IDLE;
        r8(win + rr::ICCR1) = (uint8_t)(rr::ICCR1_IDLE | rr::ICCR1_IICRST);
        r8(win + rr::ICCR1) = (uint8_t)(rr::ICCR1_IDLE | rr::ICCR1_IICRST | rr::ICCR1_ICE);

        // ICSER resets to 0x09, which is the general call AND own-address-0 enabled: a master
        // that never writes this register answers as a slave at address 0x00.
        r8(win + rr::ICSER) = 0u;
        r8(win + rr::ICMR1) =
            (uint8_t)(((rate & 0x07u) << rr::ICMR1_CKS_SHIFT) | rr::ICMR1_BCWP);
        r8(win + rr::ICBRH) = (uint8_t)(rr::ICBR_RESERVED | ((rate >> 8) & rr::ICBR_MASK));
        r8(win + rr::ICBRL) = (uint8_t)(rr::ICBR_RESERVED | ((rate >> 16) & rr::ICBR_MASK));
        r8(win + rr::ICFER) = FUNCTION_ENABLE;
        r8(win + rr::ICIER) = 0u;

        r8(win + rr::ICCR1) = (uint8_t)(rr::ICCR1_IDLE | rr::ICCR1_ICE);
    }

    // Request a stop and spin for it. Bounded by iterations rather than by the deadline: this
    // is the recovery path, and its caller reached it with the deadline already spent.
    bool stop_bounded(uintptr_t win)
    {
        uint8_t const c = r8(win + rr::ICCR2);
        if ((c & rr::ICCR2_MST) == 0u)
        {
            return (c & rr::ICCR2_BBSY) == 0u; // not the master: nothing here can stop it
        }
        status_clear(win, rr::ICSR2_STOP);
        r8(win + rr::ICCR2) = rr::ICCR2_SP;
        for (uint32_t i = 0u; i < RECOVER_SPIN; i++)
        {
            if ((r8(win + rr::ICSR2) & rr::ICSR2_STOP) != 0u)
            {
                status_clear(win, (uint8_t)(rr::ICSR2_STOP | rr::ICSR2_NACKF));
                return true;
            }
        }
        return false;
    }

    // Two different things hold SCL low here and only one of them is a fault.
    //
    //   THIS DRIVER WAS LATE: the controller's own automatic low-hold, with TDRE or RDRF
    //   asserted and the byte not yet moved. Nothing on the bus is wrong; we are.
    //
    //   THE SLAVE IS STRETCHING: no flag pending and the transfer simply not finished. Real
    //   parts do this as their documented, every-measurement data path, so treating it as a
    //   wedged bus and clocking recovery pulses at a device that is mid-measurement is the
    //   wrong move. It lets go by itself and the stop lands then.
    //
    // The two are indistinguishable from the BUS; from inside this controller the pending
    // flag separates them.
    bool driver_late(uintptr_t win)
    {
        return (r8(win + rr::ICSR2) & (uint8_t)(rr::ICSR2_TDRE | rr::ICSR2_RDRF)) != 0u;
    }

    void await_scl(uintptr_t win)
    {
        for (uint32_t i = 0u; i < RECOVER_SPIN; i++)
        {
            if ((r8(win + rr::ICCR1) & rr::ICCR1_SCLI) != 0u)
            {
                return;
            }
        }
    }

    // sec.43.11.2: one extra SCL pulse per write, the bit clears itself, and it is the only
    // way to walk a slave off a stuck-low SDA. It must run with MALE down; NALE and SALE are
    // already 0 in FUNCTION_ENABLE, so with MALE cleared no arbitration-lost source is live.
    //
    // Held SDA, not held SCL, is what this is for. A stretching slave holds SCL and needs
    // waiting out, not pulsing.
    void clock_out(uintptr_t win)
    {
        uint8_t const c = r8(win + rr::ICCR2);
        if ((c & rr::ICCR2_BBSY) != 0u)
        {
            if ((c & rr::ICCR2_MST) == 0u)
            {
                return; // busy and not ours: CLO is not permitted here
            }
        }
        uint8_t const fer = r8(win + rr::ICFER);
        r8(win + rr::ICFER) = (uint8_t)(fer & (uint8_t)~rr::ICFER_MALE);
        for (unsigned p = 0u; p < RECOVER_PULSES; p++)
        {
            if ((r8(win + rr::ICCR1) & rr::ICCR1_SDAI) != 0u)
            {
                break;
            }
            r8(win + rr::ICCR1) = (uint8_t)(rr::ICCR1_IDLE | rr::ICCR1_ICE | rr::ICCR1_CLO);
            for (uint32_t i = 0u; i < RECOVER_SPIN; i++)
            {
                if ((r8(win + rr::ICCR1) & rr::ICCR1_CLO) == 0u)
                {
                    break;
                }
            }
        }
        r8(win + rr::ICFER) = fer;
    }

    // Get the bus back to idle after an abandoned transaction, best effort and reporting
    // nothing: the caller returns the error that brought it here, not this one.
    void bus_release(uintptr_t win, uint32_t rate)
    {
        // A slave still stretching is not a bus to recover, and a stop cannot land until it
        // lets go, so give it its chance before anything here reads as stuck.
        if (not driver_late(win))
        {
            await_scl(win);
        }
        bool ok = stop_bounded(win);
        if ((r8(win + rr::ICCR1) & rr::ICCR1_SDAI) == 0u)
        {
            clock_out(win);
            ok = stop_bounded(win);
        }
        if (not ok)
        {
            program(win, rate); // the full reset ladder, which is what clears BBSY
            return;
        }
        status_clear(win, (uint8_t)(rr::ICSR2_NACKF | rr::ICSR2_STOP | rr::ICSR2_START
                                    | rr::ICSR2_AL | rr::ICSR2_TMOF));
    }

    // The rate the counts in `word` produce, rounded down. The rise/fall substitution is
    // total: standard-mode figures apply exactly when they yield a standard-mode rate, and
    // the fast-mode figures otherwise.
    uint32_t achieved_hz(uint32_t pclkb, uint32_t word)
    {
        uint32_t const phi = pclkb >> (word & 0x07u);
        if (phi == 0u)
        {
            return 0u;
        }
        uint32_t const n =
            ((word >> 8) & rr::ICBR_MASK) + 1u + ((word >> 16) & rr::ICBR_MASK) + 1u;
        uint64_t const counts_ns = ((uint64_t)n * 1000000000ull) / phi;
        uint32_t rate = (uint32_t)(1000000000ull / (counts_ns + TRF_STANDARD_NS));
        if (rate > STANDARD_MAX_HZ)
        {
            rate = (uint32_t)(1000000000ull / (counts_ns + TRF_FAST_NS));
        }
        return rate;
    }

    // Split a total count into the two registers, SCL low period no shorter than the high
    // one, both inside the noise filter's floor.
    uint32_t split_rate(uint32_t cks, uint32_t total)
    {
        uint32_t const sum = total - 2u;
        uint32_t brh = sum / 2u;
        if (brh < ICBR_MIN)
        {
            brh = ICBR_MIN;
        }
        uint32_t const brl = sum - brh;
        return rate_word(cks, brh, brl);
    }

    // The FASTEST setting that does not EXCEED `hz`, which is not the setting nearest to it.
    //
    // Searched, not solved: inverting sec.43.2.14 rounds twice, and a count one high is a
    // rate several percent slow with nothing to show it. achieved_hz falls monotonically in
    // the count, so within one divider the first count that fits is the answer; the outer
    // loop is load-bearing because a slower divider reaches rates a faster one cannot.
    int32_t derive_rate(uint32_t pclkb, uint32_t hz, uint32_t* out)
    {
        if (hz > FAST_MAX_HZ)
        {
            return -KOS_ENOTSUP; // ICFER.FMPE stays 0: no fast mode plus
        }
        for (uint32_t cks = 0u; cks < 8u; cks++)
        {
            if ((pclkb >> cks) == 0u)
            {
                break;
            }
            for (uint32_t n = COUNT_MIN; n <= COUNT_MAX; n++)
            {
                uint32_t const word = split_rate(cks, n);
                if (achieved_hz(pclkb, word) <= hz)
                {
                    *out = word;
                    return 0;
                }
            }
        }
        return -KOS_ENOTSUP;
    }

    // Put the named profile on the wire, doing nothing when it is already there. The ladder
    // is an RIIC reset, so it may only run with the bus free.
    int32_t apply_rate(uintptr_t win, uint32_t rate)
    {
        if (live_rate_word(win) == rate)
        {
            return 0;
        }
        if ((r8(win + rr::ICCR2) & rr::ICCR2_BBSY) != 0u)
        {
            return -KOS_EBUSY;
        }
        program(win, rate);
        if (live_rate_word(win) != rate)
        {
            return -KOS_EPERM; // the window did not take the store
        }
        return 0;
    }

    int32_t wait_bus_free(uintptr_t win, uint64_t deadline)
    {
        uint32_t batch = 0u;
        while (true)
        {
            if ((r8(win + rr::ICCR2) & rr::ICCR2_BBSY) == 0u)
            {
                return 0;
            }
            batch++;
            if (batch >= CLOCK_POLL_BATCH)
            {
                batch = 0u;
                if (kos_clock_now() >= deadline)
                {
                    return -KOS_EBUSY;
                }
            }
        }
    }

    // sec.43.2.2: RS is honoured only with BBSY and MST both set, and never while a stop
    // condition is being generated. Its own caller has just seen TEND or has the last read
    // byte still held, so no stop is in flight here.
    int32_t restart_condition(uintptr_t win)
    {
        uint8_t const c = r8(win + rr::ICCR2);
        uint8_t const need = (uint8_t)(rr::ICCR2_BBSY | rr::ICCR2_MST);
        if ((c & need) != need)
        {
            return -KOS_EBUSY;
        }
        status_clear(win, rr::ICSR2_START);
        r8(win + rr::ICCR2) = rr::ICCR2_RS;
        return 0;
    }

    // sec.43.3.3(2): ST needs BBSY clear, and raising it with BBSY set is itself one of the
    // arbitration-lost sources. Hardware sets BBSY, START, MST and TRS and clears ST on its
    // own; software writes none of them.
    int32_t start_condition(uintptr_t win, uint64_t deadline)
    {
        int32_t const rc = wait_bus_free(win, deadline);
        if (rc != 0)
        {
            return rc;
        }
        status_clear(win, (uint8_t)(rr::ICSR2_START | rr::ICSR2_STOP | rr::ICSR2_NACKF));
        r8(win + rr::ICCR2) = rr::ICCR2_ST;
        return 0;
    }

    int32_t put_byte(uintptr_t win, uint8_t v, uint8_t fail, uint64_t deadline)
    {
        int32_t const rc = wait_status(win, rr::ICSR2_TDRE, fail, deadline);
        if (rc != 0)
        {
            return rc;
        }
        r8(win + rr::ICDRT) = v;
        return 0;
    }

    // Hardware peels R/W# out of bit 0 and drives TRS itself at the ninth rising edge
    // (sec.43.3.3(3)).
    //
    // A 10-bit read has no addressing byte of its own (sec.43.3.4(3)): the full address goes
    // out in the write direction first, and only a restart then makes 11110xx1 select for
    // reading. So an unlatched read segment issues a restart the segment list did not ask
    // for; `latched` records that an earlier write in this transaction already did it.
    int32_t address_phase(uintptr_t win, struct kos_i2c_device const* d, bool rd, bool* latched,
                          uint64_t deadline)
    {
        uint8_t const fail = (uint8_t)(rr::ICSR2_NACKF | rr::ICSR2_AL);
        uint8_t rw = 0u;
        if (rd)
        {
            rw = 1u;
        }

        if ((d->mode & (uint8_t)KOS_BUS_ADDR_10BIT) == 0u)
        {
            int32_t const rc =
                put_byte(win, (uint8_t)((d->addr << 1) | rw), rr::ICSR2_AL, deadline);
            if (rc != 0)
            {
                return rc;
            }
            if (not rd)
            {
                return 0; // an address NACK surfaces at the next TDRE or at TEND
            }
            return wait_status(win, rr::ICSR2_RDRF, fail, deadline);
        }

        uint8_t const head = (uint8_t)(0xF0u | ((d->addr >> 7) & 0x06u));
        if (not rd)
        {
            int32_t rc = put_byte(win, head, rr::ICSR2_AL, deadline);
            if (rc == 0)
            {
                rc = put_byte(win, (uint8_t)(d->addr & 0xFFu), fail, deadline);
            }
            if (rc == 0)
            {
                *latched = true;
            }
            return rc;
        }

        if (not *latched)
        {
            int32_t rc = put_byte(win, head, rr::ICSR2_AL, deadline);
            if (rc == 0)
            {
                rc = put_byte(win, (uint8_t)(d->addr & 0xFFu), fail, deadline);
            }
            if (rc == 0)
            {
                rc = wait_status(win, rr::ICSR2_TEND, fail, deadline);
            }
            if (rc == 0)
            {
                rc = restart_condition(win);
            }
            if (rc != 0)
            {
                return rc;
            }
            *latched = true;
        }
        int32_t const rc = put_byte(win, (uint8_t)(head | 1u), rr::ICSR2_AL, deadline);
        if (rc != 0)
        {
            return rc;
        }
        return wait_status(win, rr::ICSR2_RDRF, fail, deadline);
    }

    int32_t stop_condition(uintptr_t win, uint64_t deadline)
    {
        int32_t const rc = wait_status(win, rr::ICSR2_STOP, 0u, deadline);
        if (rc != 0)
        {
            return rc;
        }
        // sec.43.3.3(7): both flags are software-cleared, and a NACKF left standing blocks
        // the data path of the next transfer.
        status_clear(win, (uint8_t)(rr::ICSR2_STOP | rr::ICSR2_NACKF));
        return 0;
    }

    // How many of the `issued` bytes this segment handed to ICDRT the slave acknowledged, read
    // at the moment NACKF was seen. TWO BYTES CAN BE IN FLIGHT: ICDRT holds one while ICDRS
    // clocks the previous, so TDRE clear means the last byte written never left and the byte
    // the slave refused is the one before it. ICFER.NACKE suspends the transfer on the NACK
    // (sec.43.2.9), so nothing moves ICDRT afterwards and this reading does not decay.
    //
    // The refused byte is NOT counted: the count is the position OF the NACK, and 0 is the
    // address phase, which the payload never includes.
    uint32_t acked_before_nack(uintptr_t win, uint32_t issued)
    {
        uint32_t wire = issued;
        if (wire != 0u)
        {
            if ((r8(win + rr::ICSR2) & rr::ICSR2_TDRE) == 0u)
            {
                wire--;
            }
        }
        if (wire == 0u)
        {
            return 0u;
        }
        return wire - 1u;
    }

    int32_t write_segment(uintptr_t win, unsigned char const* p, uint32_t n, bool stop,
                          uint64_t deadline, uint32_t* moved)
    {
        uint8_t const fail = (uint8_t)(rr::ICSR2_NACKF | rr::ICSR2_AL);
        *moved = 0u;
        for (uint32_t k = 0u; k < n; k++)
        {
            int32_t const rc = put_byte(win, (uint8_t)p[k], fail, deadline);
            if (rc != 0)
            {
                if (rc == -KOS_EIO)
                {
                    *moved = acked_before_nack(win, k);
                }
                return rc;
            }
        }
        // TEND, not TDRE: TDRE says the holding register is free, TEND says the last byte has
        // left the shift register with its acknowledge phase over. It is also the manual's
        // precondition for both SP and RS.
        //
        // wait_status weighs NACKF before TEND in the same read, so the last byte's NACK is
        // never mistaken for a completed segment.
        int32_t const rc = wait_status(win, rr::ICSR2_TEND, fail, deadline);
        if (rc != 0)
        {
            if (rc == -KOS_EIO)
            {
                *moved = acked_before_nack(win, n);
            }
            return rc;
        }
        *moved = n;
        if (not stop)
        {
            return 0; // the next segment raises RS
        }
        status_clear(win, rr::ICSR2_STOP);
        r8(win + rr::ICCR2) = rr::ICCR2_SP;
        return stop_condition(win, deadline);
    }

    // sec.43.3.4 with Fig.43.9 and Fig.43.10. There are n+1 reads of ICDRR: read 0 is the
    // DUMMY that starts reception and yields garbage, read k yields byte k. Each read is what
    // clocks the NEXT byte in, so the NACK that ends the transfer is armed before the read at
    // n-1, and the low-hold that makes room to raise it one read earlier still. For n == 1
    // both land on the dummy read, which is exactly Fig.43.9's one-byte branch.
    int32_t read_segment(uintptr_t win, unsigned char* p, uint32_t n, bool stop,
                         uint64_t deadline, uint32_t* moved)
    {
        *moved = 0u;
        uint32_t wait_at = 0u;
        if (n >= 2u)
        {
            wait_at = n - 2u;
        }
        uint32_t const nack_at = n - 1u;

        for (uint32_t k = 0u; k <= n; k++)
        {
            // NACKF is not a receive-side flag here: this master's own NACK goes out through
            // ACKBT, and with NALE clear it raises nothing.
            int32_t rc = wait_status(win, rr::ICSR2_RDRF, rr::ICSR2_AL, deadline);
            if (rc != 0)
            {
                return rc;
            }
            if (k == wait_at)
            {
                icmr3_update(win, rr::ICMR3_WAIT, 0u);
            }
            if (k == nack_at)
            {
                icmr3_update(win, rr::ICMR3_ACKBT, 0u);
            }
            if (k == n)
            {
                if (stop)
                {
                    status_clear(win, rr::ICSR2_STOP);
                    r8(win + rr::ICCR2) = rr::ICCR2_SP;
                }
                else
                {
                    // The manual puts SP here, before the ICDRR read releases the low-hold;
                    // RS takes the same slot when the transaction continues.
                    rc = restart_condition(win);
                }
                if (rc != 0)
                {
                    return rc;
                }
            }
            uint8_t const v = r8(win + rr::ICDRR); // releases the low-hold and clears RDRF
            if (k != 0u)
            {
                p[k - 1u] = (unsigned char)v;
                *moved = k;
            }
        }
        icmr3_update(win, 0u, rr::ICMR3_WAIT);
        if (not stop)
        {
            return 0;
        }
        return stop_condition(win, deadline);
    }
}

extern "C"
{

int32_t kos_i2c_bus_open(struct kos_i2c_bus* b, struct kos_i2c_bus_config const* cfg)
{
    if (cfg->base == 0u)
    {
        return -KOS_EINVAL;
    }
    if (cfg->irq != KOS_CAP_NONE)
    {
        return -KOS_EINVAL; // this engine polls; an armed line would have no owner
    }

    // Until this returns, RIICa is in module stop: every register reads back its reset value
    // and every store is dropped (sec.43.16.1).
    if (kos_periph_enable(cfg->base) != 0)
    {
        return -KOS_EPERM;
    }

    b->base = cfg->base;
    b->ep = cfg->ep;
    b->irq = cfg->irq;

    uintptr_t const win = b->base;
    program(win, SEED_RATE);
    if (live_rate_word(win) != SEED_RATE)
    {
        b->base = 0u;
        return -KOS_EPERM; // the window did not take the bring-up stores
    }

    // A peer left mid-transaction by a reset holds SDA low and no transfer will ever
    // complete over it, so the recovery runs here rather than at the first timeout.
    if ((r8(win + rr::ICCR1) & rr::ICCR1_SDAI) == 0u)
    {
        clock_out(win);
        (void)stop_bounded(win);
    }
    if ((r8(win + rr::ICCR1) & rr::ICCR1_SDAI) == 0u)
    {
        b->base = 0u;
        return -KOS_EBUSY;
    }
    return 0;
}

int32_t kos_i2c_device_open(struct kos_i2c_device* d, struct kos_i2c_bus* b,
                            struct kos_i2c_device_config const* cfg)
{
    if (b->base == 0u)
    {
        return -KOS_EINVAL; // no open bus behind this handle
    }
    if (cfg->slot >= KOS_BUS_DEV_MAX)
    {
        return -KOS_EINVAL;
    }
    if (cfg->rsv[0] != 0u or cfg->rsv[1] != 0u or cfg->rsv[2] != 0u or cfg->rsv[3] != 0u)
    {
        return -KOS_EINVAL;
    }
    if ((cfg->mode & (uint8_t)~(uint8_t)KOS_BUS_ADDR_10BIT) != 0u)
    {
        return -KOS_EINVAL;
    }
    if (cfg->addr == 0u)
    {
        return -KOS_EINVAL; // address 0 is the general call, which is not a device
    }
    if ((cfg->mode & (uint8_t)KOS_BUS_ADDR_10BIT) != 0u)
    {
        if (cfg->addr > 0x3FFu)
        {
            return -KOS_EINVAL;
        }
    }
    else
    {
        if (cfg->addr > 0x7Fu)
        {
            return -KOS_EINVAL;
        }
    }

    uint32_t const pclkb = kos_periph_clock_hz(b->base);
    if (pclkb == 0u)
    {
        return -KOS_ENOSYS; // no branch-clock oracle for this block: the rate is unknowable
    }

    uint32_t word = live_rate_word(b->base);
    if (cfg->hz != 0u)
    {
        int32_t const bad = derive_rate(pclkb, cfg->hz, &word);
        if (bad != 0)
        {
            return bad;
        }
    }
    // The rate reported below is read back off the live registers, never computed from what
    // was meant to be written.
    int32_t const rc = apply_rate(b->base, word);
    if (rc != 0)
    {
        return rc;
    }
    uint32_t const rate = achieved_hz(pclkb, live_rate_word(b->base));
    if (rate == 0u)
    {
        return -KOS_ENOTSUP;
    }

    d->bus = b;
    d->hz = rate;
    d->prog[PROG_RATE] = word;
    d->prog[PROG_RSV] = 0u;
    d->addr = cfg->addr;
    d->slot = cfg->slot;
    d->mode = cfg->mode;
    return (int32_t)rate;
}

int32_t kos_i2c_transfer(struct kos_i2c_device* d, struct kos_bus_seg const* seg, uint8_t nseg,
                         unsigned char* buf, uint32_t len, uint32_t timeout_us,
                         uint32_t* xferred)
{
    if (xferred == nullptr)
    {
        return -KOS_EINVAL;
    }
    *xferred = 0u;

    int32_t rc = kos_i2c_seg_check(seg, nseg, len);
    if (rc != 0)
    {
        return rc;
    }
    if (timeout_us == 0u or timeout_us > (uint32_t)KOS_I2C_TIMEOUT_MAX_US)
    {
        return -KOS_EINVAL;
    }
    if (len != 0u and buf == nullptr)
    {
        return -KOS_EINVAL;
    }
    struct kos_i2c_bus* const b = d->bus;
    if (b == nullptr or b->base == 0u)
    {
        return -KOS_EINVAL;
    }

    uintptr_t const win = b->base;
    uint64_t const deadline = kos_clock_now() + (uint64_t)timeout_us * 1000ull;

    rc = apply_rate(win, d->prog[PROG_RATE]);
    if (rc != 0)
    {
        return rc;
    }

    // A flag left standing from the last transaction blocks this one: NACKF alone stops both
    // the transmit and the receive path (sec.43.2.10).
    status_clear(win, (uint8_t)(rr::ICSR2_NACKF | rr::ICSR2_STOP | rr::ICSR2_START
                                | rr::ICSR2_AL | rr::ICSR2_TMOF));

    // Store one raises ACKWP by itself; only from the second store can ACKBT move at all.
    // ACKBT clears itself on a stop, but an abandoned transaction can leave it set.
    r8(win + rr::ICMR3) = (uint8_t)(r8(win + rr::ICMR3) | rr::ICMR3_ACKWP);
    icmr3_update(win, 0u, (uint8_t)(rr::ICMR3_ACKBT | rr::ICMR3_WAIT));

    // read_segment issues its own RS before the final ICDRR read, so after a read that
    // continues, the condition is ALREADY on the wire and neither branch below may fire.
    bool need_start = true;
    bool need_restart = false;
    bool latched = false;
    uint32_t off = 0u;
    for (uint8_t i = 0u; i < nseg; i++)
    {
        bool const rd = (seg[i].flags & (uint8_t)KOS_BUS_SEG_RD) != 0u;
        bool const stop = (seg[i].flags & (uint8_t)KOS_BUS_SEG_STOP) != 0u;

        if (need_restart)
        {
            rc = restart_condition(win);
        }
        else if (need_start)
        {
            rc = start_condition(win, deadline);
        }
        if (rc == 0)
        {
            rc = address_phase(win, d, rd, &latched, deadline);
        }
        uint32_t moved = 0u;
        if (rc == 0)
        {
            if (rd)
            {
                rc = read_segment(win, buf + off, seg[i].len, stop, deadline, &moved);
            }
            else
            {
                rc = write_segment(win, buf + off, seg[i].len, stop, deadline, &moved);
            }
        }
        if (rc != 0)
        {
            bus_release(win, d->prog[PROG_RATE]);
            *xferred = off + moved;
            return rc;
        }
        off += seg[i].len;
        need_restart = not stop and not rd;
        need_start = stop;
        if (stop)
        {
            latched = false;
        }
    }
    *xferred = len;
    return (int32_t)len;
}

int32_t kos_i2c_bus_close(struct kos_i2c_bus* b)
{
    if (b->base == 0u)
    {
        return 0; // never opened, or already closed
    }
    uintptr_t const win = b->base;

    bus_release(win, live_rate_word(win));
    r8(win + rr::ICIER) = 0u;
    // ICE = 0 puts both pins back inactive. ICCR1_IDLE and not 0: a zero store would clear
    // SOWP in the same cycle as SCLO and SDAO and drive the bus low.
    r8(win + rr::ICCR1) = rr::ICCR1_IDLE;
    b->base = 0u;
    return 0;
}
}
