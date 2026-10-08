// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "walk.h"

#include "driver_path.h"
#include "message.h"

#include <kickos/amp.h>
#include <kickos/sys/driver_service.h> // KOS_DRV_HANDOVER_PROBE_US
#include <kickos/sys/emit.h> // kconsole_write_all
#include <kickos/sys/errno.h>
#include <kickos/sys/task_trampoline.h>

namespace kickos::init
{
    namespace
    {
        bool dead_for_good(Phase phase)
        {
            return phase == Phase::GONE or phase == Phase::DOWN;
        }

        static_assert(Message::CAPACITY <= KOS_EP_MSG_MAX, "a diagnostic line is one send");

        // The capability the kernel seated in root for crossing `port` of `node`, or KOS_CAP_NONE.
        kos_cap_t seated_port(uint64_t node, uint64_t port)
        {
#if KOS_AMP_PORT_COUNT > 0
            return kos_amp_port(static_cast<uint32_t>(node), static_cast<uint32_t>(port));
#else
            (void)node;
            (void)port;
            return KOS_CAP_NONE;
#endif
        }

        // One diagnostic line on stdout, then `m` emptied for the next. Bounded: a console driver
        // the init holds slain or stalled never parks it, and what is not taken goes to the
        // kernel console.
        void say(Message& m)
        {
            char const* const text = m.add("\n").text();
            int32_t const sent = kos_send_timed(KOS_CAP_STDOUT, text, m.length(),
                                                kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
            size_t done = 0;
            if (sent > 0)
            {
                done = static_cast<size_t>(sent);
            }
            if (done < m.length())
            {
                kickos::kconsole_write_all(text + done, m.length() - done);
            }
            m.clear();
        }
    }

    Walk::Walk(kos_table_header const* table, Build const& build)
        : header_{table}
        , tasks_{reinterpret_cast<kos_table_task const*>(table + 1)}
        , grants_{reinterpret_cast<kos_table_grant const*>(tasks_ + table->task_count)}
        , refs_{reinterpret_cast<kos_table_ref const*>(grants_ + table->grant_count)}
        , regions_{reinterpret_cast<kos_table_region const*>(
              reinterpret_cast<kos_table_priv const*>(refs_ + table->ref_count) + table->priv_count)}
        , strings_{reinterpret_cast<char const*>(regions_ + table->region_count)}
        , build_{build}
        , ends_{KOS_TABLE_NONE}
        , notify_{KOS_CAP_NONE}
        , private_{nullptr}
        , reports_{}
        , report_count_{0}
    {
        if ((table->flags & KOS_TABLE_ENDS_TASK) != 0u)
        {
            ends_ = table->ends_task;
        }
    }

    kos_table_task const& Walk::task(uint16_t i) const
    {
        return tasks_[i];
    }

    char const* Walk::name(uint16_t i) const
    {
        return &strings_[tasks_[i].name];
    }

    uint16_t Walk::task_count() const
    {
        return header_->task_count;
    }

    kos_table_grant const& Walk::grant(uint16_t i, uint16_t k) const
    {
        return grants_[tasks_[i].first_grant + k];
    }

    TaskRecord& Walk::record(uint16_t i)
    {
        return private_[i].task;
    }

    kos_cap_t Walk::notify() const
    {
        return notify_;
    }

    void Walk::refused(char const* step, uint16_t task, char const* answer) const
    {
        Message m;
        m.add("init: ").add(step);
        if (task != KOS_TABLE_NONE)
        {
            m.add(" for `").add(name(task)).add("`");
        }
        m.add(" answered ").add(answer);
        kos_panic(m.text());
    }

    void Walk::check(int rc, char const* step, uint16_t task) const
    {
        if (rc != 0)
        {
            Message answer;
            refused(step, task, answer.add(rc).text());
        }
    }

    void* Walk::reserve(uint32_t size, char const* step, uint16_t task) const
    {
        void* const block = kos_ram_alloc(size);
        if (block == nullptr)
        {
            refused(step, task, "null");
        }
        return block;
    }

    void Walk::close(kos_cap_t cap, char const* step, uint16_t task)
    {
        check(kos_handle_close(cap), step, task);
    }

    kos_table_grant const* Walk::grant_of(uint16_t i, uint8_t kind) const
    {
        kos_table_task const& t = tasks_[i];
        for (uint16_t k = 0; k < t.grant_count; k++)
        {
            kos_table_grant const* const grant = &grants_[t.first_grant + k];
            if (grant->kind == kind)
            {
                return grant;
            }
        }
        return nullptr;
    }

    void Walk::run()
    {
        boot();
        while (true)
        {
            scan();
            flush();
            wake();
        }
    }

    void Walk::boot()
    {
        if (header_->magic != KOS_TABLE_MAGIC or header_->version != KICKOS_TABLE_VERSION)
        {
            Message m;
            m.add("init: the table is not a layout ").add(KICKOS_TABLE_VERSION).add(" table");
            kos_panic(m.text());
        }
        check(kos_thread_set_priority(header_->init_priority), "kos_thread_set_priority", KOS_TABLE_NONE);
        uint16_t const count = header_->task_count;
        for (uint16_t i = 0; i < count; i++)
        {
            check_task(i);
        }

        check(kos_notify_create(&notify_), "kos_notify_create", KOS_TABLE_NONE);
        check(kos_notify_bind(notify_), "kos_notify_bind", KOS_TABLE_NONE);

        // The order of the reservations is the arena's, which admission replays.
        uint32_t const private_size = (count + header_->region_count) * KICKOS_INIT_PRIVATE_RECORD_SIZE;
        private_ = static_cast<PrivateRecord*>(reserve(private_size, "kos_ram_alloc of the private block",
                                                       KOS_TABLE_NONE));
        check(kos_mem_self_grant(private_, private_size, 0u), "kos_mem_self_grant of the private block",
              KOS_TABLE_NONE);
        for (uint16_t i = 0; i < count; i++)
        {
            kos_table_task const& t = tasks_[i];
            TaskRecord& r = record(i);
            r.handle = KOS_TASK_NONE;
            r.ready = KOS_TASK_NONE;
            r.endpoint = KOS_CAP_NONE;
            r.notify = KOS_CAP_NONE;
            r.block = nullptr;
            r.status = nullptr;
            r.deaths = 0u;
            r.restarts_left = t.restart_max;
            r.phase = Phase::WAITING;
        }
        // Each watcher's own status block holds the tasks it watches alone, in `watches` order.
        for (uint16_t i = 0; i < count; i++)
        {
            uint32_t const status_size = tasks_[i].watch_count * KICKOS_INIT_STATUS_RECORD_SIZE;
            if (status_size == 0u)
            {
                continue;
            }
            TaskRecord& r = record(i);
            r.status = static_cast<StatusRecord*>(reserve(status_size, "kos_ram_alloc of the status block", i));
            check(kos_mem_self_grant(r.status, status_size, 0u), "kos_mem_self_grant of the status block", i);
            // status_write keeps the count's parity, so a count left odd would hold every
            // reader off for good.
            for (uint16_t k = 0; k < tasks_[i].watch_count; k++)
            {
                StatusWriter::clear(&r.status[k]);
            }
        }
        for (uint16_t r = 0; r < header_->region_count; r++)
        {
            if ((regions_[r].flags & KOS_TABLE_REGION_PARTITION) != 0u)
            {
                private_[count + r].region = reinterpret_cast<void*>(KOS_AMP_SHARE_BASE + regions_[r].offset);
                continue;
            }
            private_[count + r].region = reserve(regions_[r].size, "kos_ram_alloc of a shared region",
                                                 KOS_TABLE_NONE);
        }
        for (uint16_t i = 0; i < count; i++)
        {
            if (tasks_[i].driver != KOS_TABLE_NONE)
            {
                driver_path::reserve(*this, i);
            }
        }
        for (uint16_t i = 0; i < count; i++)
        {
            kos_table_task const& t = tasks_[i];
            if (t.driver != KOS_TABLE_NONE)
            {
                continue;
            }
            if (not build_.sp_masked)
            {
                record(i).block = reserve(t.stack, "kos_ram_alloc of the stack", i);
            }
        }

        for (uint16_t i = 0; i < count; i++)
        {
            if (grant_of(i, KOS_GRANT_ENDPOINT_SERVE) == nullptr)
            {
                continue;
            }
            TaskRecord& r = record(i);
            check(kos_endpoint_create(&r.endpoint), "kos_endpoint_create", i);
            if (not driver_path::narrows_at_handover(*this, i))
            {
                // The init never receives, so a caller answers -KOS_EAGAIN until a server does,
                // and the endpoint outlives every instance of its server.
                check(kos_cap_narrow(r.endpoint, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT),
                      "kos_cap_narrow", i);
            }
        }
        for (uint16_t i = 0; i < count; i++)
        {
            if (tasks_[i].watch_count != 0u)
            {
                check(kos_notify_create(&record(i).notify), "kos_notify_create", i);
            }
        }
        for (uint16_t i = 0; i < count; i++)
        {
            write_status(i);
        }
    }

    void Walk::check_task(uint16_t i)
    {
        kos_table_task const& t = tasks_[i];
        Message m;
        m.add("init: `").add(name(i)).add("` ");
        bool lines = false;
        uint16_t windows = 0;
        for (uint16_t k = 0; k < t.grant_count; k++)
        {
            kos_table_grant const& g = grants_[t.first_grant + k];
            if (g.kind == KOS_GRANT_LINE)
            {
                lines = true;
            }
            if (g.window != KOS_TABLE_NONE)
            {
                windows++;
            }
        }
        if (lines and build_.multicore and t.core_mask == 0u)
        {
            m.add("takes a line and declares no core, on a build of more than one kernel core");
            kos_panic(m.text());
        }
        if (t.driver != KOS_TABLE_NONE)
        {
            // A driver's descriptor takes every window role as a device window.
            for (uint16_t k = 0; k < t.grant_count; k++)
            {
                if (grants_[t.first_grant + k].kind == KOS_GRANT_PORTS)
                {
                    m.add("binds a port range to a window role of a packaged driver, which takes it as a device window");
                    kos_panic(m.text());
                }
            }
            return;
        }
        if (t.block != 0u)
        {
            m.add("carries a ring block and runs no packaged driver");
            kos_panic(m.text());
        }
        if (t.ceiling < t.priority)
        {
            m.add("has a ceiling ").add(t.ceiling).add(" below its priority ").add(t.priority);
            kos_panic(m.text());
        }
        if (t.cap_grant_count > KICKOS_MAX_SPAWN_GRANTS or windows > KICKOS_MAX_THREAD_WINDOWS)
        {
            m.add("is spawned with ").add(t.cap_grant_count).add(" capabilities and ").add(windows)
                .add(" windows, and a spawn takes at most ").add(KICKOS_MAX_SPAWN_GRANTS).add(" and ")
                .add(KICKOS_MAX_THREAD_WINDOWS);
            kos_panic(m.text());
        }
        for (uint16_t k = 0; k < t.grant_count; k++)
        {
            kos_table_grant const& g = grants_[t.first_grant + k];
            if (g.window != KOS_TABLE_NONE and g.window >= windows)
            {
                m.add("places a window at ").add(g.window).add(", past its ").add(windows);
                kos_panic(m.text());
            }
            if (g.kind == KOS_GRANT_PORT and seated_port(g.target, g.base) == KOS_CAP_NONE)
            {
                m.add("names crossing ").add(&strings_[g.path]).add(", which the kernel seated no port for");
                kos_panic(m.text());
            }
            if (g.cap_slot != KOS_TABLE_NONE
                and static_cast<uint16_t>(g.cap_slot - KOS_SPAWN_DELEGATED_CAP0) >= t.cap_grant_count)
            {
                m.add("delegates a capability at slot ").add(g.cap_slot).add(", past its ")
                    .add(t.cap_grant_count);
                kos_panic(m.text());
            }
        }
    }

    bool Walk::serving(uint16_t i)
    {
        TaskRecord& r = record(i);
        return r.phase == Phase::RUNNING and r.handle != KOS_TASK_NONE and r.ready == r.handle;
    }

    bool Walk::startable(uint16_t i)
    {
        TaskRecord& r = record(i);
        if (r.phase != Phase::WAITING and r.phase != Phase::PENDING)
        {
            return false;
        }
        kos_table_task const& t = tasks_[i];
        for (uint16_t k = 0; k < t.use_count; k++)
        {
            if (not serving(refs_[t.first_use + k].task))
            {
                return false;
            }
        }
        return true;
    }

    void Walk::scan()
    {
        // Uses point backward, so one pass in file order starts every task its servers allow; a
        // failed start leaves a restart for the next pass.
        bool started = true;
        while (started)
        {
            started = false;
            for (uint16_t i = 0; i < header_->task_count; i++)
            {
                if (startable(i))
                {
                    started = true;
                    start(i);
                }
            }
        }
    }

    void Walk::start(uint16_t i)
    {
        kos_table_task const& t = tasks_[i];
        TaskRecord& r = record(i);
        kos_cap_t copy = KOS_CAP_NONE;
        check(kos_notify_badge(notify_, i % 32u, &copy), "kos_notify_badge", i);

        kos_task_t handle = KOS_TASK_NONE;
        int rc = 0;
        char const* step = "the driver's start";
        if (t.driver != KOS_TABLE_NONE)
        {
            rc = driver_path::start(*this, i, copy, &handle);
            close(copy, "kos_handle_close of the badged copy", i);
        }
        else
        {
            void* data = nullptr;
            uint32_t data_size = 0u;
            if (build_.translating)
            {
                data = r.block;
                data_size = t.stack;
            }
            check(kos_task_create(data, data_size, 0u, &handle), "kos_task_create", i);
            Start s;
            s.task = i;
            s.handle = handle;
            s.copy = copy;
            s.line_count = 0u;
            s.pinned = false;
            s.step = "kos_task_sched_grant";
            rc = kos_task_sched_grant(handle, t.ceiling, t.core_mask);
            if (rc == 0)
            {
                s.step = "kos_task_watch";
                rc = kos_task_watch(handle, copy, r.endpoint);
            }
            if (rc == 0)
            {
                rc = take_lines(s);
            }
            if (rc == 0)
            {
                s.step = "kos_thread_create";
                rc = spawn(s);
            }
            release(s);
            step = s.step;
        }
        if (rc != 0)
        {
            failed(i, handle, step, rc);
            return;
        }
        r.handle = handle;
        r.phase = Phase::RUNNING;
        // A server is ready once a member waits on its endpoint, which the watch reports.
        r.ready = KOS_TASK_NONE;
        if (r.endpoint == KOS_CAP_NONE)
        {
            r.ready = handle;
        }
    }

    int Walk::take_lines(Start& s)
    {
        kos_table_task const& t = tasks_[s.task];
        for (uint16_t k = 0; k < t.grant_count; k++)
        {
            kos_table_grant const& g = grants_[t.first_grant + k];
            if (g.kind != KOS_GRANT_LINE)
            {
                continue;
            }
            if (build_.multicore and not s.pinned)
            {
                s.step = "kos_thread_set_affinity";
                int const rc = kos_thread_set_affinity(kos_thread_self(), t.core_mask);
                if (rc != 0)
                {
                    return rc;
                }
                s.pinned = true;
            }
            s.step = "kos_irq_claim";
            int const rc = claim(g.line, &s.lines[s.line_count]);
            if (rc != 0)
            {
                return rc;
            }
            s.line_count++;
        }
        return 0;
    }

    int Walk::claim(uint16_t line, kos_cap_t* out)
    {
        uint32_t retries = 0;
        while (true)
        {
            // The table carries no trigger, so a user task takes its lines edge-triggered.
            int const rc = kos_irq_claim(line, KOS_IRQ_EDGE, out);
            if (rc != -KOS_EAGAIN or retries == CLAIM_RETRIES)
            {
                return rc;
            }
            // The line is retiring from its last holder.
            retries++;
            kos_sleep_ns(CLAIM_RETRY_NS);
        }
    }

    int Walk::spawn(Start& s)
    {
        kos_table_task const& t = tasks_[s.task];
        TaskRecord& r = record(s.task);
        kos_window windows[KICKOS_MAX_THREAD_WINDOWS] = {};
        kos_cap_grant caps[KICKOS_MAX_SPAWN_GRANTS] = {};
        uint16_t window_count = 0;
        uint16_t line = 0;
        for (uint16_t k = 0; k < t.grant_count; k++)
        {
            kos_table_grant const& g = grants_[t.first_grant + k];
            if (g.window != KOS_TABLE_NONE)
            {
                kos_window& w = windows[g.window];
                w.base = static_cast<uintptr_t>(g.base);
                w.size = g.size;
                w.kind = KOS_WINDOW_DEVICE;
                w.flags = g.flags;
                if (g.kind == KOS_GRANT_PORTS)
                {
                    w.kind = KOS_WINDOW_PORTS;
                }
                else if (g.kind == KOS_GRANT_REGION)
                {
                    w.base = reinterpret_cast<uintptr_t>(private_[header_->task_count + g.target].region);
                    w.size = regions_[g.target].size;
                    w.kind = KOS_WINDOW_MEMORY;
                }
                else if (g.kind == KOS_GRANT_STATUS)
                {
                    w.base = reinterpret_cast<uintptr_t>(r.status);
                    w.kind = KOS_WINDOW_MEMORY;
                }
                window_count++;
            }
            if (g.cap_slot == KOS_TABLE_NONE)
            {
                continue;
            }
            kos_cap_grant& c = caps[g.cap_slot - KOS_SPAWN_DELEGATED_CAP0];
            if (g.kind == KOS_GRANT_ENDPOINT_SERVE)
            {
                // WAIT from the HANDOUT the init holds.
                c.source_cap = r.endpoint;
                c.rights_mask = KOS_CAP_WAIT;
            }
            else if (g.kind == KOS_GRANT_ENDPOINT_USE)
            {
                c.source_cap = record(g.target).endpoint;
                c.rights_mask = KOS_CAP_SIGNAL;
            }
            else if (g.kind == KOS_GRANT_NOTIFICATION)
            {
                c.source_cap = r.notify;
                c.rights_mask = KOS_CAP_WAIT;
            }
            else if (g.kind == KOS_GRANT_PORT)
            {
                c.source_cap = seated_port(g.target, g.base);
                c.rights_mask = g.flags;
            }
            else
            {
                c.source_cap = s.lines[line];
                c.rights_mask = KOS_CAP_WAIT;
                line++;
            }
        }

        kos_thread_params p = {};
        p.entry = task_trampoline;
        p.arg = const_cast<kos_table_task*>(&t);
        p.name = name(s.task);
        p.prio = t.priority;
        p.policy = KOS_POLICY_FIFO;
        p.cap_count = static_cast<uint8_t>(t.cap_grant_count);
        if (window_count != 0u)
        {
            p.windows = windows;
            p.window_count = window_count;
        }
        if (t.cap_grant_count != 0u)
        {
            p.caps = caps;
        }
        // Where SP is masked the kernel gives the thread one whole stride, and the composition's
        // figure is never handed over.
        if (not build_.sp_masked)
        {
            p.stack_base = r.block;
            p.stack_size = t.stack;
        }
        p.core_mask = t.core_mask;
        p.authority = t.authority;
        p.task = s.handle;
        kos_thread_t thread = KOS_THREAD_NONE;
        return kos_thread_create(&p, &thread);
    }

    void Walk::release(Start& s)
    {
        for (uint16_t k = 0; k < s.line_count; k++)
        {
            close(s.lines[k], "kos_handle_close of a line", s.task);
        }
        if (s.pinned)
        {
            check(kos_thread_set_affinity(kos_thread_self(), 0u), "kos_thread_set_affinity", s.task);
        }
        close(s.copy, "kos_handle_close of the badged copy", s.task);
    }

    void Walk::failed(uint16_t i, kos_task_t handle, char const* step, int rc)
    {
        // Slain before it is reported: a console driver's failed start may still hold stdout.
        if (handle != KOS_TASK_NONE)
        {
            int const slay = kos_task_slay(handle, SLAY_TIMEOUT_US);
            if (slay == -KOS_ETIMEDOUT)
            {
                // The init keeps its hold: the death arrives through the wait.
                TaskRecord& r = record(i);
                r.handle = handle;
                r.ready = KOS_TASK_NONE;
                r.phase = Phase::RUNNING;
                report(Report::Kind::FAILED, i, step, rc);
                report(Report::Kind::KEEPS, i, nullptr, 0);
                return;
            }
            check(slay, "kos_task_slay", i);
        }
        // Queued ahead of the death, which may end the system and print it then.
        report(Report::Kind::FAILED, i, step, rc);
        death(i);
    }

    void Walk::wake()
    {
        uint32_t bits = 0u;
        check(kos_notify_wait(notify_, 0xFFFFFFFFu, KOS_TIMEOUT_NONE, &bits), "kos_notify_wait",
              KOS_TABLE_NONE);

        // The state says what happened and the bit only where to look: one wake may carry an
        // end, a death and a readiness, and tasks share a bit.
        bool ending = false;
        for (uint16_t i = 0; i < header_->task_count; i++)
        {
            TaskRecord& r = record(i);
            if (r.handle == KOS_TASK_NONE)
            {
                continue;
            }
            if (((bits >> (i % 32u)) & 1u) == 0u)
            {
                continue;
            }
            int const state = kos_task_state(r.handle);
            if (state < 0)
            {
                Message answer;
                refused("kos_task_state", i, answer.add(state).text());
            }
            bool const ended = (state & KOS_TASK_ENDED) != 0;
            if (i == ends_ and ended)
            {
                ending = true;
                continue;
            }
            if ((state & KOS_TASK_DEAD) != 0)
            {
                check(kos_task_kill(r.handle), "kos_task_kill", i);
                report(Report::Kind::RELEASED, i, nullptr, 0);
                death(i);
                continue;
            }
            // Ended and not dead: its members are stopped, and the DEAD bit follows their sweeps.
            if (ended)
            {
                r.ready = KOS_TASK_NONE;
                continue;
            }
            if ((state & KOS_TASK_READY) != 0)
            {
                r.ready = r.handle;
            }
        }

        // After every death this wake carries.
        if (ending)
        {
            int status = 0;
            check(kos_task_exit_status(record(ends_).handle, &status), "kos_task_exit_status", ends_);
            end(status);
        }
    }

    void Walk::death(uint16_t i)
    {
        TaskRecord& r = record(i);
        r.handle = KOS_TASK_NONE;
        r.ready = KOS_TASK_NONE;
        r.deaths++;
        if (r.restarts_left != 0u)
        {
            r.restarts_left--;
            r.phase = Phase::PENDING;
        }
        else
        {
            r.phase = Phase::GONE;
        }
        write_status(i);
        tell(i);
        if (r.phase == Phase::GONE)
        {
            drop(i);
        }
        propagate();
        end_if_cancelled();
    }

    StatusFields status_of(TaskRecord const& record)
    {
        StatusFields fields;
        fields.deaths = record.deaths;
        fields.restarts_left = record.restarts_left;
        fields.state = 0u;
        if (not dead_for_good(record.phase))
        {
            fields.state = STATUS_ALIVE;
        }
        if (record.phase == Phase::DOWN)
        {
            fields.state = STATUS_DEPENDENCY_DOWN;
        }
        return fields;
    }

    void Walk::write_status(uint16_t i)
    {
        StatusFields const fields = status_of(record(i));
        for (uint16_t w = 0; w < header_->task_count; w++)
        {
            kos_table_task const& t = tasks_[w];
            for (uint16_t k = 0; k < t.watch_count; k++)
            {
                if (refs_[t.first_watch + k].task == i)
                {
                    status_write(&record(w).status[k], fields);
                }
            }
        }
    }

    void Walk::report(Report::Kind kind, uint16_t task, char const* step, int rc)
    {
        if (report_count_ == REPORTS)
        {
            flush();
        }
        Report& r = reports_[report_count_];
        r.kind = kind;
        r.task = task;
        r.rc = rc;
        r.step = step;
        report_count_++;
    }

    void Walk::flush()
    {
        for (uint16_t k = 0; k < report_count_; k++)
        {
            Report const& r = reports_[k];
            Message m;
            m.add("init: `").add(name(r.task)).add("` ");
            if (r.kind == Report::Kind::FAILED)
            {
                m.add("failed to start: ").add(r.step).add(" answered ").add(r.rc);
            }
            else if (r.kind == Report::Kind::RELEASED)
            {
                m.add("is dead and released");
            }
            else
            {
                m.add("keeps members past its slay");
            }
            say(m);
        }
        report_count_ = 0;
    }

    void Walk::tell(uint16_t i)
    {
        for (uint16_t w = 0; w < header_->task_count; w++)
        {
            kos_table_task const& t = tasks_[w];
            for (uint16_t k = 0; k < t.watch_count; k++)
            {
                if (refs_[t.first_watch + k].task != i)
                {
                    continue;
                }
                kos_cap_t copy = KOS_CAP_NONE;
                check(kos_notify_badge(record(w).notify, k, &copy), "kos_notify_badge", w);
                int const rc = kos_notify(copy);
                // A watcher that has not taken the bit yet reads every record when it does.
                if (rc != 0 and rc != -KOS_EALREADY)
                {
                    Message answer;
                    refused("kos_notify", w, answer.add(rc).text());
                }
                close(copy, "kos_handle_close of a watcher's copy", w);
            }
        }
    }

    void Walk::drop(uint16_t i)
    {
        // The last HANDOUT goes, so a caller answers -KOS_ECONNREFUSED.
        TaskRecord& r = record(i);
        if (r.endpoint != KOS_CAP_NONE)
        {
            close(r.endpoint, "kos_handle_close of the endpoint", i);
            r.endpoint = KOS_CAP_NONE;
        }
    }

    void Walk::propagate()
    {
        // Uses point backward, so one pass in file order reaches every dependant in turn.
        for (uint16_t i = 0; i < header_->task_count; i++)
        {
            TaskRecord& r = record(i);
            if (r.phase != Phase::WAITING and r.phase != Phase::PENDING)
            {
                continue;
            }
            kos_table_task const& t = tasks_[i];
            for (uint16_t k = 0; k < t.use_count; k++)
            {
                if (dead_for_good(record(refs_[t.first_use + k].task).phase))
                {
                    r.phase = Phase::DOWN;
                    write_status(i);
                    tell(i);
                    drop(i);
                    break;
                }
            }
        }
    }

    void Walk::end_if_cancelled()
    {
        // The task `ends` names never runs its entry: admission refuses its restart, so a failed
        // start leaves it dead for good.
        if (ends_ != KOS_TABLE_NONE and dead_for_good(record(ends_).phase))
        {
            end(KOS_EXIT_CANCELLED);
        }
    }

    void Walk::end(int status)
    {
        flush();
        driver_path::drain(*this);
        Message answer;
        refused("kos_shutdown", KOS_TABLE_NONE, answer.add(kos_shutdown(status)).text());
    }
}
