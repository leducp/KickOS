// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "fake_kernel.h"

#include <kickos/board_config.h>
#include <kickos/config/priorities.h> // KICKOS_PRIO_MAX
#include <kickos/sys/errno.h>

#include <stdlib.h>
#include <string.h>

namespace fake
{
    std::function<void(Call const&)> on_call;
    std::function<void(Thread const&)> on_spawn;

    namespace
    {
        constexpr kos_thread_t INIT_THREAD = 0x42u;
        constexpr size_t PAGE = 4096u;
        constexpr uint32_t ALL_RIGHTS = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;
        constexpr uint32_t ALL_AUTHORITY = KOS_AUTH_MEMORY | KOS_AUTH_PINMUX | KOS_AUTH_PSTATE | KOS_AUTH_IRQ
                                           | KOS_AUTH_SYSTEM | KOS_AUTH_CONSOLE | KOS_AUTH_TASKS
                                           | KOS_AUTH_BUS_MASTER;
        // Past every walk's needs: a walk looping on an answer that never changes fails here
        // rather than hanging its test.
        constexpr size_t RUNAWAY = 100000u;

        struct Rule
        {
            std::string fn;
            int result;
            uint32_t times;
            uint32_t skip;
            std::function<bool(Call const&)> when;
        };

        Config g_config;
        std::vector<Call> g_calls;
        std::vector<Rule> g_rules;
        std::vector<std::function<void()>> g_steps;
        size_t g_step = 0;
        uint64_t g_now = 0;
        std::vector<Object> g_objects;
        std::vector<Cap> g_table;
        std::vector<Task> g_tasks;
        std::vector<Thread> g_threads;
        std::vector<Block> g_reservations;
        std::vector<Block> g_self_grants;
        uint32_t g_affinity = 0;
        std::string g_console;
        std::vector<kos_task_t> g_free_handles;
        int64_t g_stdout = -1;
        // The kernel's console ownership (kernel/init/console.cc, ConsoleState).
        enum class Owner
        {
            KERNEL,
            USER,
            RECLAIMED
        };
        Owner g_owner = Owner::KERNEL;
        uint32_t g_reclaims = 0;
        // The task the console was published for, into g_tasks, or CONSOLE_SELF for the init's
        // own, which never ends.
        constexpr int64_t CONSOLE_SELF = -1;
        int64_t g_console_task = CONSOLE_SELF;
        kos_cap_t g_next_cap = 0;
        Peaks g_peaks;

        void peak(uint32_t& at, uint32_t now)
        {
            if (now > at)
            {
                at = now;
            }
        }

        void measure()
        {
            uint32_t endpoints = 0;
            uint32_t notifications = 0;
            for (Object const& o : g_objects)
            {
                if (o.refs == 0u)
                {
                    continue;
                }
                if (o.kind == Kind::ENDPOINT)
                {
                    endpoints++;
                }
                if (o.kind == Kind::NOTIFY)
                {
                    notifications++;
                }
            }
            uint32_t threads = 0;
            for (Thread const& t : g_threads)
            {
                if (t.live)
                {
                    threads++;
                }
            }
            uint32_t tasks = 0;
            uint32_t domains = 2u;
            if (g_config.translating)
            {
                domains++;
            }
            for (Task const& t : g_tasks)
            {
                if (t.released or t.creator != INIT_THREAD)
                {
                    continue;
                }
                tasks++;
                if (g_config.translating or t.data != nullptr)
                {
                    domains++;
                }
            }
            uint32_t lines = 0;
            for (Object const& o : g_objects)
            {
                if (o.kind == Kind::LINE and o.refs != 0u)
                {
                    lines++;
                }
            }
            peak(g_peaks.cap_slots, g_config.cap_reserved + static_cast<uint32_t>(g_table.size()));
            peak(g_peaks.endpoints, endpoints);
            peak(g_peaks.notifications, notifications);
            peak(g_peaks.threads, threads);
            peak(g_peaks.tasks, tasks);
            peak(g_peaks.irq_handles, lines);
            peak(g_peaks.domains, domains);
            peak(g_peaks.reservations, static_cast<uint32_t>(g_reservations.size()));
            peak(g_peaks.self_grants, static_cast<uint32_t>(g_self_grants.size()));
        }

        size_t record(char const* fn, std::vector<uint64_t> args)
        {
            if (g_calls.size() >= RUNAWAY)
            {
                throw Panic{"fake: runaway"};
            }
            g_calls.push_back(Call{fn, args, 0, ""});
            if (on_call)
            {
                on_call(g_calls.back());
            }
            return g_calls.size() - 1u;
        }

        // The scripted answer of this call, or 1 for none.
        int scripted(size_t call)
        {
            for (Rule& rule : g_rules)
            {
                if (rule.fn != g_calls[call].fn or rule.times == 0u or (rule.when and not rule.when(g_calls[call])))
                {
                    continue;
                }
                if (rule.skip != 0u)
                {
                    rule.skip--;
                    return 1;
                }
                rule.times--;
                return rule.result;
            }
            return 1;
        }

        void settle();

        int finish(size_t call, int result)
        {
            g_calls[call].result = result;
            settle();
            measure();
            return result;
        }

        size_t make(Kind kind)
        {
            g_objects.push_back(Object{kind, 0u, 0u, -1, false, -1, 0u, false});
            return g_objects.size() - 1u;
        }

        // One reference to `object` goes; a line's last takes the binding's reference with it.
        void drop_ref(size_t object)
        {
            Object& o = g_objects[object];
            o.refs--;
            if (o.refs == 0u and o.binding >= 0)
            {
                size_t const note = static_cast<size_t>(o.binding);
                o.binding = -1;
                drop_ref(note);
            }
        }

        // The holders of WAIT on an endpoint: the init's capabilities and every live thread's.
        uint32_t receivers(size_t object)
        {
            uint32_t n = 0;
            for (Cap const& c : g_table)
            {
                if (c.object == object and (c.rights & KOS_CAP_WAIT) != 0u)
                {
                    n++;
                }
            }
            for (Thread const& t : g_threads)
            {
                for (Cap const& c : t.caps)
                {
                    if (t.live and c.object == object and (c.rights & KOS_CAP_WAIT) != 0u)
                    {
                        n++;
                    }
                }
            }
            return n;
        }

        // Whether a live thread, rather than the init, holds WAIT on an endpoint.
        bool serving(size_t object)
        {
            for (Thread const& t : g_threads)
            {
                for (Cap const& c : t.caps)
                {
                    if (t.live and c.object == object and (c.rights & KOS_CAP_WAIT) != 0u)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // Whether the task the console was published for has not ended
        // (kernel/syscall/cap.cc, EP_CONSOLE_SERVED).
        bool console_task_live()
        {
            if (g_console_task == CONSOLE_SELF)
            {
                return true;
            }
            return (g_tasks[static_cast<size_t>(g_console_task)].state & KOS_TASK_ENDED) == 0u;
        }

        // The kernel's vacated mark (kernel/syscall/cap.cc): set when an endpoint's WAIT holders
        // fall to none, whatever drops them. The published console's task ending gives the kernel
        // its console back.
        void settle()
        {
            if (g_owner == Owner::USER and not console_task_live())
            {
                g_owner = Owner::RECLAIMED;
                g_reclaims++;
            }
            for (size_t o = 0; o < g_objects.size(); o++)
            {
                Object& obj = g_objects[o];
                if (obj.kind != Kind::ENDPOINT)
                {
                    continue;
                }
                uint32_t const now = receivers(o);
                if (obj.holders != 0u and now == 0u)
                {
                    obj.vacated = true;
                }
                obj.holders = now;
            }
        }

        // Whether a live receiver waits on `object` now, which clears its vacated mark
        // (kernel/sync/sync.cc, endpoint_receiver_waits).
        bool waits(size_t object)
        {
            if (not serving(object) or not g_config.console_receives)
            {
                return false;
            }
            g_objects[object].vacated = false;
            return true;
        }

        // Whether a send that finds no waiting receiver parks rather than being refused
        // (kernel/include/kickos/endpoint.h, endpoint_receiving), the published console parking
        // it while its task lives.
        bool receiving(size_t object)
        {
            if (static_cast<int64_t>(object) == g_stdout and g_owner == Owner::USER
                and console_task_live())
            {
                return true;
            }
            return receivers(object) != 0u and not g_objects[object].vacated;
        }

        // The refusal of a send to an endpoint not receiving (endpoint_unserved).
        int unserved(size_t object)
        {
            if (receivers(object) != 0u or handout(object))
            {
                return -KOS_EAGAIN;
            }
            return -KOS_ECONNREFUSED;
        }

        uint32_t live(Kind kind)
        {
            uint32_t n = 0;
            for (Object const& o : g_objects)
            {
                if (o.kind == kind and o.refs != 0u)
                {
                    n++;
                }
            }
            return n;
        }

        // Whether one more of `used` passes `limit`, 0 being none.
        bool full(uint32_t used, uint32_t limit)
        {
            return limit != 0u and used >= limit;
        }

        bool table_full()
        {
            return full(g_config.cap_reserved + static_cast<uint32_t>(g_table.size()), g_config.limits.cap_table);
        }

        kos_cap_t install(size_t object, uint32_t rights, int badge)
        {
            g_next_cap++;
            g_objects[object].refs++;
            g_table.push_back(Cap{g_next_cap, object, rights, badge});
            return g_next_cap;
        }

        Cap* find_cap(kos_cap_t cap)
        {
            for (Cap& c : g_table)
            {
                if (c.handle == cap)
                {
                    return &c;
                }
            }
            return nullptr;
        }

        // The newest task under `handle`, which a reused handle names.
        Task* find_task(kos_task_t handle)
        {
            for (size_t i = g_tasks.size(); i > 0u; i--)
            {
                Task& t = g_tasks[i - 1u];
                if (t.handle == handle and handle != KOS_TASK_NONE)
                {
                    return &t;
                }
            }
            return nullptr;
        }

        // A task the init may act on, or the errno the kernel answers: an unknown or released
        // one, or one whose hold its kill dropped, is -KOS_EBADF, another creator's -KOS_EPERM.
        int resolve(kos_task_t handle, Task** out)
        {
            Task* const t = find_task(handle);
            if (t == nullptr or t->released or not t->held)
            {
                return -KOS_EBADF;
            }
            if (t->creator != INIT_THREAD)
            {
                return -KOS_EPERM;
            }
            *out = t;
            return 0;
        }

        uint32_t live_threads()
        {
            uint32_t n = 0;
            for (Thread const& t : g_threads)
            {
                if (t.live)
                {
                    n++;
                }
            }
            return n;
        }

        uint32_t live_tasks()
        {
            uint32_t n = 0;
            for (Task const& t : g_tasks)
            {
                if (not t.released and t.creator == INIT_THREAD)
                {
                    n++;
                }
            }
            return n;
        }

        uint32_t live_domains()
        {
            uint32_t n = 2u;
            if (g_config.translating)
            {
                n++;
            }
            for (Task const& t : g_tasks)
            {
                if (not t.released and t.creator == INIT_THREAD and (g_config.translating or t.data != nullptr))
                {
                    n++;
                }
            }
            return n;
        }

        uint32_t core_set_all()
        {
            return static_cast<uint32_t>((1ull << g_config.kernel_cores) - 1u);
        }

        // The distinct objects of `kind` a task's live members hold, with `extra` added.
        uint32_t task_objects(Task const& task, Kind kind, std::vector<size_t> extra)
        {
            for (size_t m : task.members)
            {
                Thread const& t = g_threads[m];
                for (Cap const& c : t.caps)
                {
                    if (t.live)
                    {
                        extra.push_back(c.object);
                    }
                }
            }
            std::vector<size_t> seen;
            for (size_t o : extra)
            {
                bool known = false;
                for (size_t s : seen)
                {
                    known = known or s == o;
                }
                if (not known and g_objects[o].kind == kind)
                {
                    seen.push_back(o);
                }
            }
            return static_cast<uint32_t>(seen.size());
        }

        // Whether a device or port window [base, base + size) overlaps one a live thread holds.
        bool window_held(kos_window const& w)
        {
            for (Thread const& t : g_threads)
            {
                if (not t.live)
                {
                    continue;
                }
                for (kos_window const& held : t.windows)
                {
                    if (held.kind == w.kind and held.base < w.base + w.size and w.base < held.base + held.size)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // The pages of `size` bytes.
        size_t pages(size_t size)
        {
            return (size + PAGE - 1u) / PAGE;
        }

        Block const* find_block(void const* base)
        {
            for (Block const& b : g_reservations)
            {
                if (b.base == base)
                {
                    return &b;
                }
            }
            return nullptr;
        }

        // A member's sweep: what it holds goes.
        void swept(Thread& t)
        {
            t.sweeping = false;
            for (Cap const& c : t.caps)
            {
                drop_ref(c.object);
            }
        }

        // A slay or a kill ends a task that has not ended, its entry cancelled.
        void condemn(Task& task)
        {
            if ((task.state & KOS_TASK_ENDED) == 0u)
            {
                task.status = KOS_EXIT_CANCELLED;
                task.state = task.state | KOS_TASK_ENDED;
            }
        }

        // Every live member stops, its sweep finishing unless the task's sweeps are stalled.
        void end_members(Task& task)
        {
            for (size_t m : task.members)
            {
                Thread& t = g_threads[m];
                if (not t.live)
                {
                    continue;
                }
                t.live = false;
                t.sweeping = true;
                if (not task.stalled)
                {
                    swept(t);
                }
            }
            task.state = task.state & ~static_cast<uint32_t>(KOS_TASK_LIVE | KOS_TASK_READY);
        }

        bool sweeping(Task const& task)
        {
            for (size_t m : task.members)
            {
                if (g_threads[m].sweeping)
                {
                    return true;
                }
            }
            return false;
        }

        // The slot is freed once the creator's hold is gone and every member is swept.
        void release_if_done(Task& task)
        {
            if (task.released or task.held or (task.state & KOS_TASK_LIVE) != 0u or sweeping(task))
            {
                return;
            }
            task.released = true;
            if (g_config.reuse_handles)
            {
                g_free_handles.push_back(task.handle);
            }
        }

        void raise(Task const& task)
        {
            if (task.watch_object >= 0 and task.watch_badge >= 0)
            {
                g_objects[static_cast<size_t>(task.watch_object)].pending |= 1u << task.watch_badge;
            }
        }

        Task& current(char const* name)
        {
            Task const* const task = instance(name);
            if (task == nullptr)
            {
                throw Panic{std::string{"fake: no instance of "} + name};
            }
            return *const_cast<Task*>(task);
        }

        // WAIT is derived from HANDOUT on an endpoint alone (kernel/syscall/syscall_thread.cc).
        bool rights_pass(Kind kind, uint32_t source, uint8_t mask)
        {
            for (uint32_t bit = 1u; bit <= KOS_CAP_HANDOUT; bit = bit << 1)
            {
                if ((mask & bit) == 0u or (source & bit) != 0u)
                {
                    continue;
                }
                if (bit == KOS_CAP_WAIT and kind == Kind::ENDPOINT and (source & KOS_CAP_HANDOUT) != 0u)
                {
                    continue;
                }
                return false;
            }
            return true;
        }
    }

    void reset(Config const& config)
    {
        for (Block const& b : g_reservations)
        {
            free(b.base);
        }
        g_config = config;
        g_calls.clear();
        g_rules.clear();
        g_steps.clear();
        g_step = 0;
        g_now = 1000000000ull;
        g_objects.clear();
        g_table.clear();
        g_tasks.clear();
        g_threads.clear();
        g_reservations.clear();
        g_self_grants.clear();
        g_affinity = 0u;
        g_console.clear();
        g_free_handles.clear();
        g_stdout = -1;
        g_owner = Owner::KERNEL;
        g_reclaims = 0;
        g_console_task = CONSOLE_SELF;
        on_spawn = nullptr;
        g_next_cap = 0x100u;
        g_peaks = Peaks{};
        on_call = nullptr;
        // The AMP ports the kernel seated in root's table.
        for (uint32_t i = 0; i < config.amp_ports; i++)
        {
            install(make(Kind::ENDPOINT), ALL_RIGHTS, -1);
        }
        measure();
    }

    void refuse(char const* fn, int result, uint32_t times, uint32_t skip)
    {
        g_rules.push_back(Rule{fn, result, times, skip, nullptr});
    }

    void refuse_if(char const* fn, int result, uint32_t times, std::function<bool(Call const&)> when)
    {
        g_rules.push_back(Rule{fn, result, times, 0u, when});
    }

    void script(std::vector<std::function<void()>> steps)
    {
        g_steps = steps;
        g_step = 0;
    }

    void advance(uint64_t ns)
    {
        g_now = g_now + ns;
    }

    uint64_t now()
    {
        return g_now;
    }

    void ready(char const* name)
    {
        Task& task = current(name);
        task.state = task.state | KOS_TASK_READY;
        raise(task);
    }

    void end(char const* name, int status)
    {
        Task& task = current(name);
        if ((task.state & KOS_TASK_ENDED) == 0u)
        {
            task.status = status;
        }
        task.state = task.state | KOS_TASK_ENDED;
        raise(task);
    }

    void die(char const* name)
    {
        die_task(current(name).handle);
    }

    void die_task(kos_task_t handle)
    {
        Task* const found = find_task(handle);
        if (found == nullptr)
        {
            throw Panic{"fake: no task to die"};
        }
        Task& task = *found;
        if ((task.state & KOS_TASK_ENDED) == 0u)
        {
            task.status = KOS_EXIT_FAULT;
        }
        task.stalled = false;
        end_members(task);
        for (size_t m : task.members)
        {
            if (g_threads[m].sweeping)
            {
                swept(g_threads[m]);
            }
        }
        task.state = task.state | KOS_TASK_ENDED | KOS_TASK_DEAD;
        raise(task);
        release_if_done(task);
    }

    void cancel(char const* name, int status)
    {
        Task& task = current(name);
        if ((task.state & KOS_TASK_ENDED) == 0u)
        {
            task.status = status;
        }
        end_members(task);
        task.state = task.state | KOS_TASK_ENDED;
        if (not sweeping(task))
        {
            task.state = task.state | KOS_TASK_DEAD;
        }
        raise(task);
        release_if_done(task);
    }

    void stall(char const* name)
    {
        current(name).stalled = true;
    }

    void sweep(char const* name)
    {
        Task& task = current(name);
        task.stalled = false;
        for (size_t m : task.members)
        {
            if (g_threads[m].sweeping)
            {
                swept(g_threads[m]);
            }
        }
        if ((task.state & KOS_TASK_ENDED) != 0u and (task.state & KOS_TASK_DEAD) == 0u)
        {
            task.state = task.state | KOS_TASK_DEAD;
            raise(task);
        }
        release_if_done(task);
    }

    void ready_all()
    {
        for (Task& task : g_tasks)
        {
            if (not task.released and task.ready_object >= 0 and (task.state & KOS_TASK_READY) == 0u
                and (task.state & KOS_TASK_LIVE) != 0u)
            {
                task.state = task.state | KOS_TASK_READY;
                raise(task);
            }
        }
    }

    void raise_bit(uint32_t bit)
    {
        g_objects[init_notification()].pending |= 1u << bit;
    }

    std::vector<Call> const& calls()
    {
        return g_calls;
    }

    std::vector<Call> calls_of(char const* fn)
    {
        std::vector<Call> out;
        for (Call const& c : g_calls)
        {
            if (c.fn == fn)
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::vector<std::string> sequence(std::vector<std::string> const& fns)
    {
        std::vector<std::string> out;
        for (Call const& c : g_calls)
        {
            for (std::string const& fn : fns)
            {
                if (c.fn == fn)
                {
                    out.push_back(c.fn);
                }
            }
        }
        return out;
    }

    std::vector<Task> const& tasks()
    {
        return g_tasks;
    }

    std::vector<Thread> const& threads()
    {
        return g_threads;
    }

    std::vector<Object> const& objects()
    {
        return g_objects;
    }

    std::vector<Block> const& reservations()
    {
        return g_reservations;
    }

    std::vector<Block> const& self_grants()
    {
        return g_self_grants;
    }

    std::vector<Cap> const& table()
    {
        return g_table;
    }

    int64_t object_of(kos_cap_t cap)
    {
        Cap const* const c = find_cap(cap);
        if (c == nullptr)
        {
            return -1;
        }
        return static_cast<int64_t>(c->object);
    }

    size_t init_notification()
    {
        for (size_t i = 0; i < g_objects.size(); i++)
        {
            if (g_objects[i].kind == Kind::NOTIFY)
            {
                return i;
            }
        }
        throw Panic{"fake: no notification"};
    }

    std::vector<Thread const*> spawned(char const* name)
    {
        std::vector<Thread const*> out;
        for (Thread const& t : g_threads)
        {
            if (t.name == name)
            {
                out.push_back(&t);
            }
        }
        return out;
    }

    Task const* instance(char const* name)
    {
        std::vector<Thread const*> const entries = spawned(name);
        if (entries.empty())
        {
            return nullptr;
        }
        return &g_tasks[entries.back()->owner];
    }

    kos_task_t foreign_task()
    {
        Task task{};
        task.handle = static_cast<kos_task_t>((g_tasks.size() + 1u) << 8);
        task.creator = INIT_THREAD + 1u;
        task.watch_object = -1;
        task.watch_badge = -1;
        task.ready_object = -1;
        task.ceiling = KICKOS_PRIO_MAX;
        task.core_mask = 0xFFFFFFFFu;
        task.state = KOS_TASK_LIVE;
        task.held = true;
        g_tasks.push_back(task);
        return task.handle;
    }

    bool handout(size_t object)
    {
        for (Cap const& c : g_table)
        {
            if (c.object == object and (c.rights & KOS_CAP_HANDOUT) != 0u)
            {
                return true;
            }
        }
        return false;
    }

    std::string const& console()
    {
        return g_console;
    }

    void set_console_receives(bool receives)
    {
        g_config.console_receives = receives;
    }

    int64_t stdout_object()
    {
        return g_stdout;
    }

    uint32_t reclaims()
    {
        return g_reclaims;
    }

    Peaks const& peaks()
    {
        return g_peaks;
    }
}

using namespace fake;

extern "C"
{

int kos_notify_create(kos_cap_t* out_cap)
{
    size_t const call = record("kos_notify_create", {});
    *out_cap = KOS_CAP_NONE;
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (full(live(Kind::NOTIFY), g_config.limits.notifications))
    {
        return finish(call, -KOS_ENOMEM);
    }
    if (table_full())
    {
        return finish(call, -KOS_EMFILE);
    }
    *out_cap = install(make(Kind::NOTIFY), ALL_RIGHTS, -1);
    return finish(call, 0);
}

int kos_notify_bind(kos_cap_t notify_cap)
{
    size_t const call = record("kos_notify_bind", {notify_cap});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Cap const* const c = find_cap(notify_cap);
    if (c == nullptr or g_objects[c->object].kind != Kind::NOTIFY)
    {
        return finish(call, -KOS_EBADF);
    }
    if ((c->rights & KOS_CAP_WAIT) == 0u)
    {
        return finish(call, -KOS_EACCES);
    }
    if (g_objects[c->object].bound)
    {
        return finish(call, 0);
    }
    // The init binds one object.
    for (Object const& o : g_objects)
    {
        if (o.bound)
        {
            return finish(call, -KOS_EBUSY);
        }
    }
    g_objects[c->object].bound = true;
    g_objects[c->object].refs++;
    return finish(call, 0);
}

int kos_notify_badge(kos_cap_t source_cap, uint32_t bit, kos_cap_t* out_cap)
{
    size_t const call = record("kos_notify_badge", {source_cap, bit});
    *out_cap = KOS_CAP_NONE;
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (bit >= 32u)
    {
        return finish(call, -KOS_EINVAL);
    }
    Cap const* const c = find_cap(source_cap);
    if (c == nullptr or g_objects[c->object].kind != Kind::NOTIFY)
    {
        return finish(call, -KOS_EBADF);
    }
    if (c->badge >= 0)
    {
        return finish(call, -KOS_EACCES);
    }
    if (table_full())
    {
        return finish(call, -KOS_EMFILE);
    }
    *out_cap = install(c->object, c->rights, static_cast<int>(bit));
    return finish(call, 0);
}

int kos_notify(kos_cap_t notify_cap)
{
    size_t const call = record("kos_notify", {notify_cap});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Cap const* const c = find_cap(notify_cap);
    if (c == nullptr or g_objects[c->object].kind != Kind::NOTIFY)
    {
        return finish(call, -KOS_EBADF);
    }
    if ((c->rights & KOS_CAP_SIGNAL) == 0u)
    {
        return finish(call, -KOS_EACCES);
    }
    // An unbadged capability raises bit 0.
    uint32_t bit = 1u;
    if (c->badge >= 0)
    {
        bit = 1u << c->badge;
    }
    Object& o = g_objects[c->object];
    if ((o.pending & bit) != 0u)
    {
        return finish(call, -KOS_EALREADY);
    }
    o.pending = o.pending | bit;
    return finish(call, 0);
}

int kos_notify_wait(kos_cap_t notify_cap, uint32_t mask, uint32_t timeout_us, uint32_t* out_bits)
{
    size_t const call = record("kos_notify_wait", {notify_cap, mask, timeout_us});
    *out_bits = 0u;
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (mask == 0u)
    {
        return finish(call, -KOS_EINVAL);
    }
    Cap const* const c = find_cap(notify_cap);
    if (c == nullptr or g_objects[c->object].kind != Kind::NOTIFY)
    {
        return finish(call, -KOS_EBADF);
    }
    if ((c->rights & KOS_CAP_WAIT) == 0u)
    {
        return finish(call, -KOS_EACCES);
    }
    if (not g_objects[c->object].bound)
    {
        return finish(call, -KOS_EPERM);
    }
    size_t const object = c->object;
    while (true)
    {
        if (g_step < g_steps.size())
        {
            g_step++;
            g_steps[g_step - 1u]();
        }
        uint32_t const raised = g_objects[object].pending & mask;
        if (raised != 0u)
        {
            g_objects[object].pending = g_objects[object].pending & ~raised;
            *out_bits = raised;
            return finish(call, 0);
        }
        if (timeout_us != KOS_TIMEOUT_NONE)
        {
            advance(static_cast<uint64_t>(timeout_us) * 1000u);
            return finish(call, -KOS_ETIMEDOUT);
        }
        if (g_step >= g_steps.size())
        {
            finish(call, 0);
            throw Idle{};
        }
    }
}

int kos_handle_close(kos_cap_t cap)
{
    size_t const call = record("kos_handle_close", {cap});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    for (size_t i = 0; i < g_table.size(); i++)
    {
        if (g_table[i].handle == cap)
        {
            size_t const object = g_table[i].object;
            g_table.erase(g_table.begin() + static_cast<ptrdiff_t>(i));
            drop_ref(object);
            return finish(call, 0);
        }
    }
    return finish(call, -KOS_EBADF);
}

int kos_cap_narrow(kos_cap_t cap, uint32_t mask)
{
    size_t const call = record("kos_cap_narrow", {cap, mask});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Cap* const c = find_cap(cap);
    if (c == nullptr)
    {
        return finish(call, -KOS_EBADF);
    }
    c->rights = c->rights & mask;
    return finish(call, 0);
}

int kos_endpoint_create(kos_cap_t* out_cap)
{
    size_t const call = record("kos_endpoint_create", {});
    *out_cap = KOS_CAP_NONE;
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (full(live(Kind::ENDPOINT), g_config.limits.endpoints))
    {
        return finish(call, -KOS_ENOMEM);
    }
    if (table_full())
    {
        return finish(call, -KOS_EMFILE);
    }
    *out_cap = install(make(Kind::ENDPOINT), ALL_RIGHTS | KOS_CAP_HANDOUT, -1);
    return finish(call, 0);
}

void* kos_ram_alloc(size_t size)
{
    size_t const call = record("kos_ram_alloc", {size});
    if (scripted(call) != 1 or size == 0u
        or full(static_cast<uint32_t>(g_reservations.size()), g_config.limits.ram_owners))
    {
        finish(call, 0);
        return nullptr;
    }
    size_t const extent = pages(size) * PAGE;
    void* const block = aligned_alloc(PAGE, extent);
    memset(block, DIRTY, extent);
    g_reservations.push_back(Block{block, size, extent});
    finish(call, 1);
    return block;
}

int kos_mem_self_grant(void* base, size_t size, uint32_t flags)
{
    size_t const call = record("kos_mem_self_grant", {reinterpret_cast<uintptr_t>(base), size, flags});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (size == 0u or (flags & ~static_cast<uint32_t>(KOS_MEM_NOCACHE)) != 0u)
    {
        return finish(call, -KOS_EINVAL);
    }
    Block const* const b = find_block(base);
    if (b == nullptr or size > b->extent)
    {
        return finish(call, -KOS_EPERM);
    }
    if (not g_config.translating and full(static_cast<uint32_t>(g_self_grants.size()), g_config.limits.free_regions))
    {
        return finish(call, -KOS_ENOMEM);
    }
    g_self_grants.push_back(Block{base, size, b->extent});
    return finish(call, 0);
}

int kos_task_create(void* mem_base, uint32_t mem_size, uint32_t mem_flags, kos_task_t* out_task)
{
    size_t const call = record("kos_task_create", {reinterpret_cast<uintptr_t>(mem_base), mem_size, mem_flags});
    *out_task = KOS_TASK_NONE;
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if ((mem_flags & ~static_cast<uint32_t>(KOS_MEM_NOCACHE)) != 0u)
    {
        return finish(call, -KOS_EINVAL);
    }
    if (mem_base != nullptr)
    {
        Block const* const b = find_block(mem_base);
        if (b == nullptr or mem_size > b->extent)
        {
            return finish(call, -KOS_EPERM);
        }
        // A translating board hands one whole reservation over (kernel/mem/aspace.cc).
        if (g_config.translating and pages(mem_size) * PAGE != b->extent)
        {
            return finish(call, -KOS_EPERM);
        }
    }
    if (full(live_tasks(), g_config.limits.tasks))
    {
        return finish(call, -KOS_ENOMEM);
    }
    if ((g_config.translating or mem_base != nullptr) and full(live_domains(), g_config.limits.domains))
    {
        return finish(call, -KOS_ENOMEM);
    }
    kos_task_t handle = static_cast<kos_task_t>((g_tasks.size() + 1u) << 8);
    if (not g_free_handles.empty())
    {
        handle = g_free_handles.back();
        g_free_handles.pop_back();
    }
    Task task{};
    task.handle = handle;
    task.creator = INIT_THREAD;
    task.data = mem_base;
    task.data_size = mem_size;
    task.ceiling = KICKOS_PRIO_MAX;
    task.core_mask = 0xFFFFFFFFu;
    task.watch_object = -1;
    task.watch_badge = -1;
    task.ready_object = -1;
    task.held = true;
    g_tasks.push_back(task);
    *out_task = task.handle;
    return finish(call, 0);
}

int kos_task_sched_grant(kos_task_t task, uint8_t prio_ceiling, uint32_t core_mask)
{
    size_t const call = record("kos_task_sched_grant", {task, prio_ceiling, core_mask});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Task* t = nullptr;
    int const err = resolve(task, &t);
    if (err != 0)
    {
        return finish(call, err);
    }
    if (not t->members.empty())
    {
        return finish(call, -KOS_EBUSY);
    }
    if (prio_ceiling > KICKOS_PRIO_MAX)
    {
        return finish(call, -KOS_EINVAL);
    }
    // Against the caller's own grant first (kernel/syscall/syscall_thread.cc:1148).
    if (core_mask != 0u and (core_mask & core_set_all()) == 0u)
    {
        return finish(call, -KOS_EINVAL);
    }
    if (prio_ceiling != 0u and prio_ceiling > g_config.init_ceiling)
    {
        return finish(call, -KOS_EPERM);
    }
    // 0 leaves a field as it is; a grant only narrows.
    if (prio_ceiling != 0u and prio_ceiling > t->ceiling)
    {
        return finish(call, -KOS_EPERM);
    }
    if (core_mask != 0u and (core_mask & ~t->core_mask) != 0u)
    {
        return finish(call, -KOS_EPERM);
    }
    if (prio_ceiling != 0u)
    {
        t->ceiling = prio_ceiling;
    }
    if (core_mask != 0u)
    {
        t->core_mask = core_mask;
    }
    t->granted = true;
    return finish(call, 0);
}

int kos_task_watch(kos_task_t task, kos_cap_t notify_cap, kos_cap_t ready_ep)
{
    size_t const call = record("kos_task_watch", {task, notify_cap, ready_ep});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Task* t = nullptr;
    int const err = resolve(task, &t);
    if (err != 0)
    {
        return finish(call, err);
    }
    Cap const* const n = find_cap(notify_cap);
    if (n == nullptr or g_objects[n->object].kind != Kind::NOTIFY)
    {
        return finish(call, -KOS_EBADF);
    }
    if ((n->rights & KOS_CAP_SIGNAL) == 0u)
    {
        return finish(call, -KOS_EACCES);
    }
    int64_t ready = -1;
    if (ready_ep != KOS_CAP_NONE)
    {
        ready = object_of(ready_ep);
        if (ready < 0 or g_objects[static_cast<size_t>(ready)].kind != Kind::ENDPOINT)
        {
            return finish(call, -KOS_EBADF);
        }
    }
    t->watch_object = static_cast<int64_t>(n->object);
    t->watch_badge = n->badge;
    if (t->watch_badge < 0)
    {
        t->watch_badge = 0;
    }
    t->ready_object = ready;
    return finish(call, 0);
}

kos_thread_t kos_thread_self(void)
{
    size_t const call = record("kos_thread_self", {});
    if (not g_config.multicore)
    {
        finish(call, 0);
        return KOS_THREAD_NONE;
    }
    finish(call, 0);
    return INIT_THREAD;
}

int kos_thread_set_affinity(kos_thread_t thread, uint32_t core_mask)
{
    size_t const call = record("kos_thread_set_affinity", {thread, core_mask});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (not g_config.multicore)
    {
        return finish(call, -KOS_ENOSYS);
    }
    if (thread != INIT_THREAD)
    {
        return finish(call, -KOS_EBADF);
    }
    g_affinity = core_mask;
    return finish(call, 0);
}

int kos_thread_set_priority(uint8_t priority)
{
    size_t const call = record("kos_thread_set_priority", {priority});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (priority < KICKOS_PRIO_MIN or priority > KICKOS_PRIO_MAX)
    {
        return finish(call, -KOS_EINVAL);
    }
    if (priority > g_config.init_ceiling)
    {
        return finish(call, -KOS_EPERM);
    }
    return finish(call, 0);
}

int kos_irq_claim(int line, unsigned int flags, kos_cap_t* out_cap)
{
    size_t const call = record("kos_irq_claim", {static_cast<uint64_t>(line), flags});
    *out_cap = KOS_CAP_NONE;
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (line < 0 or static_cast<uint32_t>(line) >= g_config.max_irq
        or (flags & ~static_cast<unsigned int>(KOS_IRQ_LEVEL)) != 0u)
    {
        return finish(call, -KOS_EINVAL);
    }
    for (int owned : g_config.kernel_lines)
    {
        if (owned == line)
        {
            return finish(call, -KOS_EPERM);
        }
    }
    // Above one kernel core the claimer's affinity is exactly the core it runs on, which a pin to
    // one core makes it (kernel/irq/irq.cc:558).
    if (g_config.multicore and (g_affinity == 0u or (g_affinity & (g_affinity - 1u)) != 0u
                                or (g_affinity & core_set_all()) == 0u))
    {
        return finish(call, -KOS_EPERM);
    }
    uint32_t claimed = 0;
    for (Cap const& c : g_table)
    {
        if (g_objects[c.object].kind == Kind::LINE)
        {
            claimed++;
        }
    }
    if (g_config.limits.task_lines != 0u and claimed >= g_config.limits.task_lines)
    {
        return finish(call, -KOS_EAGAIN);
    }
    for (Object const& o : g_objects)
    {
        if (o.kind == Kind::LINE and o.line == line and o.refs != 0u)
        {
            return finish(call, -KOS_EBUSY);
        }
    }
    if (full(live(Kind::LINE), g_config.limits.irq_handles))
    {
        return finish(call, -KOS_ENOMEM);
    }
    if (table_full())
    {
        return finish(call, -KOS_EMFILE);
    }
    size_t const object = make(Kind::LINE);
    g_objects[object].line = line;
    *out_cap = install(object, ALL_RIGHTS, -1);
    return finish(call, 0);
}

void kos_sleep_ns(uint64_t ns)
{
    size_t const call = record("kos_sleep_ns", {ns});
    advance(ns);
    finish(call, 0);
}

uint64_t kos_clock_now(void)
{
    return now();
}

int kos_thread_create(struct kos_thread_params const* params, kos_thread_t* out_thread)
{
    size_t const call = record("kos_thread_create", {params->task, params->cap_count, params->window_count});
    if (params->name != nullptr)
    {
        g_calls[call].text = params->name;
    }
    *out_thread = KOS_THREAD_NONE;
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    // kernel/syscall/syscall_thread.cc:390, before anything else is read.
    if (params->prio < KICKOS_PRIO_MIN or params->prio > KICKOS_PRIO_MAX)
    {
        return finish(call, -KOS_EINVAL);
    }
    if (params->stack_base != nullptr)
    {
        uintptr_t const base = reinterpret_cast<uintptr_t>(params->stack_base);
        if (params->stack_size < g_config.min_stack or (base % 16u) != 0u or (params->stack_size % 16u) != 0u)
        {
            return finish(call, -KOS_EINVAL);
        }
        // A masked stack is one whole stride on the stride (kernel/thread/tls.cc).
        if (g_config.sp_masked and ((base % g_config.stride) != 0u or params->stack_size != g_config.stride))
        {
            return finish(call, -KOS_EINVAL);
        }
    }
    // Bits past the defined authorities are refused, and only then a bit the creator lacks
    // (kernel/syscall/syscall_thread.cc:540).
    if ((params->authority & ~ALL_AUTHORITY) != 0u)
    {
        return finish(call, -KOS_EINVAL);
    }
    if ((params->authority & ~g_config.authority) != 0u)
    {
        return finish(call, -KOS_EPERM);
    }
    if (params->window_count > KICKOS_MAX_THREAD_WINDOWS)
    {
        return finish(call, -KOS_ENOMEM);
    }
    if (params->cap_count > KICKOS_MAX_SPAWN_GRANTS or (params->cap_count != 0u and params->caps == nullptr))
    {
        return finish(call, -KOS_EINVAL);
    }
    Task* task = nullptr;
    int const err = resolve(params->task, &task);
    if (err != 0)
    {
        return finish(call, err);
    }
    if ((task->state & KOS_TASK_ENDED) != 0u)
    {
        return finish(call, -KOS_EBUSY);
    }
    if (params->prio > task->ceiling)
    {
        return finish(call, -KOS_EPERM);
    }
    // A mask the task's grant does not meet at all is refused, a partial one intersected
    // (kernel/include/kickos/sched.h, sched_admit_mask at INTERSECT).
    if (g_config.kernel_cores > 1u)
    {
        uint32_t requested = params->core_mask;
        if (requested == 0u)
        {
            requested = task->core_mask;
        }
        uint32_t const wanted = requested & core_set_all();
        if (wanted == 0u)
        {
            return finish(call, -KOS_EINVAL);
        }
        if ((wanted & task->core_mask) == 0u)
        {
            return finish(call, -KOS_EPERM);
        }
    }
    if (params->stack_base != nullptr)
    {
        Block const* const b = find_block(params->stack_base);
        if (b == nullptr or params->stack_size > b->extent)
        {
            return finish(call, -KOS_EPERM);
        }
        // A translating board runs a caller stack in its task's data region.
        uintptr_t const base = reinterpret_cast<uintptr_t>(params->stack_base);
        uintptr_t const data = reinterpret_cast<uintptr_t>(task->data);
        if (g_config.translating
            and (task->data == nullptr or base < data or base + params->stack_size > data + task->data_size))
        {
            return finish(call, -KOS_EPERM);
        }
    }
    Thread thread{};
    for (uint16_t i = 0; i < params->window_count; i++)
    {
        kos_window const& w = params->windows[i];
        if (w.size == 0u)
        {
            return finish(call, -KOS_EINVAL);
        }
        if (w.kind == KOS_WINDOW_MEMORY)
        {
            if ((w.flags & ~static_cast<uint32_t>(KOS_WINDOW_RO | KOS_WINDOW_UNCACHED)) != 0u)
            {
                return finish(call, -KOS_EINVAL);
            }
            for (kos_window const& earlier : thread.windows)
            {
                if (earlier.kind == KOS_WINDOW_MEMORY and earlier.base == w.base)
                {
                    return finish(call, -KOS_EINVAL);
                }
            }
            Block const* const b = find_block(reinterpret_cast<void const*>(w.base));
            if (b == nullptr or w.size > b->extent)
            {
                return finish(call, -KOS_EPERM);
            }
            // A translating board maps one whole reservation of the spawner's
            // (kernel/syscall/syscall_thread.cc, window_admit).
            if (g_config.translating and pages(w.size) * PAGE != b->extent)
            {
                return finish(call, -KOS_EPERM);
            }
        }
        else
        {
            if (w.flags != 0u or (w.kind != KOS_WINDOW_DEVICE and w.kind != KOS_WINDOW_PORTS))
            {
                return finish(call, -KOS_EINVAL);
            }
            if (window_held(w))
            {
                return finish(call, -KOS_EBUSY);
            }
            for (kos_window const& earlier : thread.windows)
            {
                if (earlier.kind == w.kind and earlier.base < w.base + w.size and w.base < earlier.base + earlier.size)
                {
                    return finish(call, -KOS_EBUSY);
                }
            }
        }
        thread.windows.push_back(w);
    }
    for (uint8_t i = 0; i < params->cap_count; i++)
    {
        kos_cap_grant const& g = params->caps[i];
        Cap const* const source = find_cap(g.source_cap);
        if (source == nullptr)
        {
            return finish(call, -KOS_EBADF);
        }
        if ((source->rights & KOS_CAP_TRANSFER) == 0u
            or not rights_pass(g_objects[source->object].kind, source->rights, g.rights_mask))
        {
            return finish(call, -KOS_EACCES);
        }
        thread.caps.push_back(Cap{static_cast<kos_cap_t>(KOS_SPAWN_DELEGATED_CAP0 + i), source->object,
                                  g.rights_mask, source->badge});
    }
    if (full(live_threads(), g_config.limits.threads))
    {
        return finish(call, -KOS_ENOMEM);
    }
    // The grants against the destination task's ceilings (kernel/syscall/syscall_thread.cc:915).
    std::vector<size_t> granted;
    for (Cap const& c : thread.caps)
    {
        granted.push_back(c.object);
    }
    if ((g_config.limits.task_endpoints != 0u
         and task_objects(*task, Kind::ENDPOINT, granted) > g_config.limits.task_endpoints)
        or (g_config.limits.task_notifications != 0u
            and task_objects(*task, Kind::NOTIFY, granted) > g_config.limits.task_notifications)
        or (g_config.limits.task_lines != 0u and task_objects(*task, Kind::LINE, granted) > g_config.limits.task_lines))
    {
        return finish(call, -KOS_EAGAIN);
    }
    thread.handle = static_cast<kos_thread_t>(0x1000u + g_threads.size());
    thread.task = params->task;
    thread.owner = static_cast<size_t>(task - g_tasks.data());
    thread.name = params->name;
    thread.params = *params;
    thread.live = true;
    for (Cap const& c : thread.caps)
    {
        g_objects[c.object].refs++;
    }
    g_threads.push_back(thread);
    task->members.push_back(g_threads.size() - 1u);
    task->state = task->state | KOS_TASK_LIVE;
    *out_thread = g_threads.back().handle;
    if (on_spawn)
    {
        on_spawn(g_threads.back());
    }
    return finish(call, 0);
}

int kos_task_slay(kos_task_t task, uint32_t timeout_us)
{
    size_t const call = record("kos_task_slay", {task, timeout_us});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Task* t = nullptr;
    int const err = resolve(task, &t);
    if (err != 0)
    {
        return finish(call, err);
    }
    // Forcible: every member stops, and the slay returns once each is swept, or at its timeout
    // with the sweeps left to finish.
    condemn(*t);
    end_members(*t);
    if (sweeping(*t))
    {
        advance(static_cast<uint64_t>(timeout_us) * 1000u);
        return finish(call, -KOS_ETIMEDOUT);
    }
    t->held = false;
    release_if_done(*t);
    return finish(call, 0);
}

int kos_task_kill(kos_task_t task)
{
    size_t const call = record("kos_task_kill", {task});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Task* t = nullptr;
    int const err = resolve(task, &t);
    if (err != 0)
    {
        return finish(call, err);
    }
    // Cancels the group and drops the creator's hold: the slot stays taken, its handle unreused,
    // until every member is swept.
    t->held = false;
    condemn(*t);
    end_members(*t);
    release_if_done(*t);
    return finish(call, 0);
}

int kos_task_state(kos_task_t task)
{
    size_t const call = record("kos_task_state", {task});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    // Resolvable until its slot is freed, its creator's hold dropped or not.
    Task* const t = find_task(task);
    if (t == nullptr or t->released)
    {
        return finish(call, -KOS_EBADF);
    }
    if (t->creator != INIT_THREAD)
    {
        return finish(call, -KOS_EPERM);
    }
    return finish(call, static_cast<int>(t->state));
}

int kos_task_exit_status(kos_task_t task, int* status)
{
    size_t const call = record("kos_task_exit_status", {task});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Task* t = nullptr;
    int const err = resolve(task, &t);
    if (err != 0)
    {
        return finish(call, err);
    }
    if ((t->state & KOS_TASK_ENDED) == 0u)
    {
        return finish(call, -KOS_EBUSY);
    }
    *status = t->status;
    return finish(call, 0);
}

int kos_shutdown(int status)
{
    size_t const call = record("kos_shutdown", {static_cast<uint64_t>(static_cast<int64_t>(status))});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    finish(call, 0);
    throw Shutdown{status};
}

int32_t kos_send(kos_cap_t ep, void const* buf, size_t len)
{
    size_t const call = record("kos_send", {ep, len});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    // The init's stdout index: empty until a publish seats it (kernel/syscall/cap.cc).
    if (ep != KOS_CAP_STDOUT or g_stdout < 0)
    {
        return finish(call, -KOS_EBADF);
    }
    size_t const object = static_cast<size_t>(g_stdout);
    settle();
    if (waits(object))
    {
        g_console.append(static_cast<char const*>(buf), len);
        return finish(call, static_cast<int>(len));
    }
    if (receiving(object))
    {
        // A receiver seated and not vacated parks a plain send for good
        // (kernel/syscall/syscall_ipc.cc).
        throw Panic{"fake: a send parked on a console nothing receives"};
    }
    return finish(call, unserved(object));
}

int32_t kos_send_timed(kos_cap_t ep, void const* buf, size_t len, uint32_t timeout_us)
{
    size_t const call = record("kos_send_timed", {ep, len, timeout_us});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    if (ep != KOS_CAP_STDOUT or g_stdout < 0)
    {
        return finish(call, -KOS_EBADF);
    }
    size_t const object = static_cast<size_t>(g_stdout);
    settle();
    if (waits(object))
    {
        g_console.append(static_cast<char const*>(buf), len);
        return finish(call, static_cast<int>(len));
    }
    if (receiving(object))
    {
        // Parked until its deadline, never for a deadline of 0 (kernel/syscall/syscall_ipc.cc).
        advance(static_cast<uint64_t>(timeout_us) * 1000u);
        return finish(call, -KOS_ETIMEDOUT);
    }
    return finish(call, unserved(object));
}

int kos_console_publish(kos_cap_t ep, kos_task_t task)
{
    size_t const call = record("kos_console_publish", {ep, task});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    int64_t const object = object_of(ep);
    if (object < 0 or g_objects[static_cast<size_t>(object)].kind != Kind::ENDPOINT)
    {
        return finish(call, -KOS_EBADF);
    }
    int64_t served_by = CONSOLE_SELF;
    if (task != KOS_TASK_NONE)
    {
        Task* t = nullptr;
        int const err = resolve(task, &t);
        if (err != 0)
        {
            return finish(call, err);
        }
        if ((t->state & KOS_TASK_ENDED) != 0u)
        {
            return finish(call, -KOS_EBUSY);
        }
        served_by = t - g_tasks.data();
    }
    // kernel/syscall/cap.cc, cap_console_publish_through: HANDOUT, and a holder without WAIT
    // gains it and the endpoint receives.
    Cap* const c = find_cap(ep);
    if ((c->rights & KOS_CAP_HANDOUT) == 0u)
    {
        return finish(call, -KOS_EACCES);
    }
    if ((c->rights & KOS_CAP_WAIT) == 0u)
    {
        c->rights = c->rights | KOS_CAP_WAIT;
        g_objects[static_cast<size_t>(object)].vacated = false;
    }
    g_stdout = object;
    g_owner = Owner::USER;
    g_console_task = served_by;
    settle();
    return finish(call, 0);
}

int kos_irq_bind_notify(kos_cap_t irq_cap, kos_cap_t notify_cap)
{
    size_t const call = record("kos_irq_bind_notify", {irq_cap, notify_cap});
    int const rc = scripted(call);
    if (rc != 1)
    {
        return finish(call, rc);
    }
    Cap const* const line = find_cap(irq_cap);
    Cap const* const note = find_cap(notify_cap);
    if (line == nullptr or g_objects[line->object].kind != Kind::LINE or note == nullptr
        or g_objects[note->object].kind != Kind::NOTIFY)
    {
        return finish(call, -KOS_EBADF);
    }
    if ((line->rights & KOS_CAP_WAIT) == 0u or (note->rights & KOS_CAP_SIGNAL) == 0u)
    {
        return finish(call, -KOS_EACCES);
    }
    if (g_objects[line->object].binding >= 0)
    {
        return finish(call, -KOS_EBUSY);
    }
    g_objects[line->object].binding = static_cast<int64_t>(note->object);
    g_objects[note->object].refs++;
    return finish(call, 0);
}

void kos_print(char const* s)
{
    size_t const call = record("kos_print", {});
    if (g_owner != Owner::USER)
    {
        g_console.append(s);
    }
    finish(call, 0);
}

int32_t kos_kconsole_write(void const* buf, size_t len)
{
    size_t const call = record("kos_kconsole_write", {len});
    if (g_owner != Owner::USER)
    {
        g_console.append(static_cast<char const*>(buf), len);
    }
    return finish(call, static_cast<int>(len));
}

void kos_yield(void)
{
    size_t const call = record("kos_yield", {});
    finish(call, 0);
}

void kos_panic(char const* msg)
{
    size_t const call = record("kos_panic", {});
    finish(call, 0);
    throw Panic{msg};
}

void kos_exit(int code)
{
    throw Exit{code};
}

}
