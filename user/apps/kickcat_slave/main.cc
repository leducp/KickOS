// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Stage A of docs/design-kickcat-k64f.md: run a KickCAT EtherCAT slave on the
// KickOS sim over KickCAT's software EmulatedESC -- no hardware, host-runnable,
// CI-provable. Proves three things at once: the KickCAT KickOS OS backend (time
// routed through kos_clock_now/kos_sleep_ns), full C++ (exceptions/STL/heap) on a
// KickOS sim app, and the slave state machine advancing against EmulatedESC.
//
// The slave runs in its own unprivileged KickOS thread (the userspace-driver
// shape the architecture targets). KickCAT objects are heap-allocated: on the sim
// the host heap is outside the mprotect-governed user-RAM arena, so an
// unprivileged thread reaches them freely, while its 64 KB arena stack stays
// isolated. A mailboxless slave is used so INIT -> PRE_OP needs no EEPROM/SII: the
// only master action modelled is an APWR of AL_CONTROL = PRE_OP straight into the
// EmulatedESC (the ECAT-side seam), after which slave.routine() drives the ESM.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include "kickcat/ESC/EmulatedESC.h"
#include "kickcat/PDO.h"
#include "kickcat/protocol.h"
#include "kickcat/slave/Slave.h"

#include <memory>

using namespace kickcat;

namespace
{
    kos::Semaphore* g_done = nullptr;
    int g_result = -1;

    void log_state(char const* tag, State state)
    {
        char line[64];
        ksnprintf(line, sizeof(line), "  %s: AL_STATUS=0x%x (%s)\n",
                  tag, static_cast<unsigned>(state), toString(state));
        kos::print(line);
    }

    // Write AL_CONTROL from the ECAT (master) point of view via an auto-increment
    // physical write -- the minimal slice of a master needed to request a state.
    void request_state(EmulatedESC& esc, State requested)
    {
        uint16_t value = requested;
        uint16_t wkc = 0;
        DatagramHeader header{Command::APWR, 0, createAddress(0, reg::AL_CONTROL),
                              sizeof(uint16_t), 0, 0, 0, 0};
        esc.processDatagram(&header, &value, &wkc);
    }

    int run_slave()
    {
        // EmulatedESC::Memory is ~64 KB; keep it off the thread stack.
        auto esc = std::make_unique<EmulatedESC>();
        auto pdo = std::make_unique<PDO>(esc.get());
        slave::Slave slave(esc.get(), pdo.get());

        // Mailboxless terminal: no setMailbox(), so INIT -> PRE_OP takes the
        // no-mailbox path and needs no EEPROM-provided SM configuration.
        slave.start();
        log_state("start ", slave.state());

        // Idle routines while the master (here, nobody) leaves AL_CONTROL = INIT.
        for (int i = 0; i < 3; ++i)
        {
            slave.routine();
        }
        log_state("idle  ", slave.state());
        if (slave.state() != State::INIT)
        {
            kos::print("  FAIL: slave did not settle in INIT\n");
            return 2;
        }

        // A concrete PDI-side ESC access, to show read/write reach the emulated map.
        uint8_t esc_config = 0;
        esc->read(reg::ESC_CONFIG, &esc_config, sizeof(esc_config));
        char line[64];
        ksnprintf(line, sizeof(line), "  ESC access: ESC_CONFIG=0x%x\n",
                  static_cast<unsigned>(esc_config));
        kos::print(line);

        // Master requests PRE_OP; drive the ESM until it lands there.
        request_state(*esc, State::PRE_OP);
        State reached = slave.state();
        for (int i = 0; i < 16 and reached != State::PRE_OP; ++i)
        {
            slave.routine();
            reached = slave.state();
        }
        log_state("preop ", reached);

        if (reached == State::PRE_OP)
        {
            kos::print("  slave reached PRE_OP\n");
            return 0;
        }
        kos::print("  FAIL: slave did not reach PRE_OP\n");
        return 3;
    }

    void slave_thread(void*) // caps: done@1 (delegated by main)
    {
        g_result = run_slave();
        kos_sem_post(1); // g_done (delegated cap at child index 1, fresh table)
    }
}

int main(int, char**)
{
    kos::print("KickCAT EtherCAT slave on KickOS/sim over EmulatedESC\n");

    kos::Semaphore done(0);
    g_done = &done;

    kos_cap_grant caps[] = {{done.id(), KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
    int th = kos::thread::spawn_caps(slave_thread, nullptr, "kickcat-slave", 10, caps, 1);
    if (th < 0)
    {
        kos::print("  FAIL: could not spawn slave thread\n");
        return 1;
    }

    done.wait(); // root parks until the slave thread finishes its run
    return g_result;
}
