// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A scripted kernel for the init's walk: every kos_* call the walk makes, recorded, over a model
// of the init's capability table, the objects it names, the tasks and threads it creates and the
// blocks it reserves. A test scripts what each wait reports, the calls refused, and whether a
// member's teardown sweep finishes, then reads the calls, the model and its peaks.
//
// A task's member that ends stops running at once and keeps what it holds until its sweep
// finishes. The task's DEAD bit is set, and its watch raised, once every member is swept; a slay
// returns 0 only then, and a kill releases the task only then. A reservation comes back filled
// with DIRTY, every 32-bit word of it odd, so the walk reads nothing it has not written. A run
// that makes more calls than any walk needs throws Panic{"fake: runaway"}.
//
// HOST-ONLY. These are public kos_* names, which a target image linking this TU would take from
// the executable instead of the syscall stubs.

#ifndef KICKOS_TESTS_UNIT_INITWALK_FAKE_KERNEL_H
#define KICKOS_TESTS_UNIT_INITWALK_FAKE_KERNEL_H

#include <kickos/sys.h>

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace fake
{
    // The byte every reservation is filled with.
    constexpr unsigned char DIRTY = 0xA5u;

    // What ends a run of the walk: a panic, the shutdown, or a wait nothing is scripted to end.
    struct Panic
    {
        std::string message;
    };
    struct Shutdown
    {
        int status;
    };
    struct Idle
    {
    };
    // kos_exit, which only the trampoline calls.
    struct Exit
    {
        int code;
    };

    enum class Kind
    {
        NOTIFY,
        ENDPOINT,
        LINE
    };

    struct Object
    {
        Kind kind;
        uint32_t refs;    // capabilities naming it, the init's and every live thread's, and a bind
        uint32_t pending; // a notification's raised bits
        int line;
        bool bound;       // a notification the init has bound
        int64_t binding;  // a line's: the notification it raises, which it holds a reference on
        uint32_t holders; // an endpoint's WAIT holders when last counted
        bool vacated;     // an endpoint whose WAIT holders fell to none, until a receiver next waits
    };

    struct Cap
    {
        kos_cap_t handle;
        size_t object;
        uint32_t rights;
        int badge; // -1 for none
    };

    struct Thread
    {
        kos_thread_t handle;
        kos_task_t task;
        size_t owner; // into tasks()
        std::string name;
        kos_thread_params params;
        std::vector<kos_window> windows;
        std::vector<Cap> caps; // the delegated capabilities, at slot KOS_SPAWN_DELEGATED_CAP0 + i
        bool live;
        bool sweeping; // ended, its capabilities not yet released
    };

    struct Task
    {
        kos_task_t handle;
        kos_thread_t creator;
        void* data;
        uint32_t data_size;
        uint8_t ceiling;
        uint32_t core_mask;
        bool granted;
        int64_t watch_object; // -1 for no watch
        int watch_badge;
        int64_t ready_object; // -1 for none
        uint32_t state;
        int status;
        bool held;     // its creator's hold, which a kill drops
        bool stalled;  // its members' sweeps wait for sweep()
        bool released; // its slot freed: a held slay or a dropped hold, every member swept
        std::vector<size_t> members; // into threads()
    };

    struct Call
    {
        std::string fn;
        std::vector<uint64_t> args;
        int64_t result;
        std::string text; // a spawned thread's name
    };

    struct Block
    {
        void* base;
        size_t size;
        size_t extent;
    };

    // The highest each count reached.
    struct Peaks
    {
        uint32_t cap_slots;     // the init's table, the reserved indices included
        uint32_t endpoints;     // live endpoint objects
        uint32_t notifications; // live notification objects
        uint32_t threads;       // live threads
        uint32_t tasks;         // tasks not yet released
        uint32_t irq_handles;   // live line objects
        uint32_t domains;       // the kernel's two, and the init's and each task's space, or each data region
        uint32_t reservations;
        uint32_t self_grants;
    };

    // The kernel build's ceilings, 0 for none. A call past one answers as the kernel does.
    struct Limits
    {
        uint32_t cap_table;     // KICKOS_CAP_TABLE_SUPPLY, the reserved indices included
        uint32_t tasks;         // KICKOS_MAX_TASKS less the idle tasks and the init's
        uint32_t threads;       // KICKOS_MAX_THREADS
        uint32_t endpoints;     // KICKOS_MAX_ENDPOINTS
        uint32_t notifications; // KICKOS_MAX_NOTIFY
        uint32_t irq_handles;   // KICKOS_MAX_IRQ_HANDLES
        uint32_t domains;       // KICKOS_MAX_DOMAINS
        uint32_t free_regions;  // the regions root's set takes past its static ones and its stack
        uint32_t ram_owners;    // KICKOS_RAM_OWNER_SLOTS, on a region board that enforces its unit
        // The per-task budgets: KICKOS_TASK_ENDPOINT_BUDGET, KICKOS_TASK_NOTIFY_BUDGET and
        // KICKOS_TASK_IRQ_HANDLE_BUDGET.
        uint32_t task_endpoints;
        uint32_t task_notifications;
        uint32_t task_lines;
    };

    struct Config
    {
        uint32_t cap_reserved = 2;
        uint32_t amp_ports = 0;
        bool multicore = false;
        uint32_t kernel_cores = 1;
        uint32_t max_irq = 1024;        // KICKOS_MAX_IRQ
        std::vector<int> kernel_lines;  // lines the kernel owns
        uint8_t init_ceiling = 31;      // the init's own priority ceiling
        bool translating = false;
        bool sp_masked = false;
        uint32_t stride = 0;    // a masked stack's one size and alignment
        uint32_t min_stack = 0; // KICKOS_MIN_STACK_SIZE
        uint32_t authority = KOS_AUTH_ALL; // the init's
        // Whether a task created after a release takes the released task's handle.
        bool reuse_handles = false;
        // Whether a published console's receiver takes what is sent it.
        bool console_receives = true;
        Limits limits = {};
    };

    void reset(Config const& config);

    // The `skip`+1-th to `skip`+`times`-th calls of `fn` answer `result` (a kos_ram_alloc refused
    // answers null).
    void refuse(char const* fn, int result, uint32_t times = 1, uint32_t skip = 0);
    // The first `times` calls of `fn` that `when` selects answer `result`.
    void refuse_if(char const* fn, int result, uint32_t times, std::function<bool(Call const&)> when);

    // One step per kos_notify_wait, run before it reads the bits: a wait with no bit raised runs
    // the next step, or with a bound times out after advancing the clock by it, or with none and
    // no step left throws Idle.
    void script(std::vector<std::function<void()>> steps);
    void advance(uint64_t ns);
    uint64_t now();

    // What the kernel reports of the current instance of the task the walk spawned as `name`,
    // raising its watch: a member waiting on its endpoint, its end with `status` with its DEAD bit
    // still to come, its death with every member swept.
    void ready(char const* name);
    void end(char const* name, int status);
    void die(char const* name);
    // The same for the newest task under `handle`, which may have no member.
    void die_task(kos_task_t handle);
    // The current instance's members end without finishing their sweeps until sweep() runs: a
    // cancel, a slay or a kill leaves them sweeping.
    void stall(char const* name);
    // `name`'s entry ends with `status` and the kernel cancels its group: every member stops, and
    // the DEAD bit follows once each is swept.
    void cancel(char const* name, int status);
    // The stalled sweeps of `name`'s current instance finish: its DEAD bit is set and its watch
    // raised, and a task its creator has dropped is released.
    void sweep(char const* name);
    // Every unreleased task with a ready endpoint that is not ready yet becomes ready.
    void ready_all();
    // Raises `bit` of the init's notification with no task behind it.
    void raise_bit(uint32_t bit);

    // Run on every call once recorded, before its result is decided.
    extern std::function<void(Call const&)> on_call;
    // Run on every thread a spawn creates, as the thread's first effect on what the init sees.
    extern std::function<void(Thread const&)> on_spawn;

    std::vector<Call> const& calls();
    // The calls to `fn`, in order.
    std::vector<Call> calls_of(char const* fn);
    // The names of the calls, in order, the ones in `fns` only.
    std::vector<std::string> sequence(std::vector<std::string> const& fns);

    std::vector<Task> const& tasks();
    std::vector<Thread> const& threads();
    std::vector<Object> const& objects();
    std::vector<Block> const& reservations();
    std::vector<Block> const& self_grants();
    // The init's table.
    std::vector<Cap> const& table();
    // The object a capability of the init's names, or -1.
    int64_t object_of(kos_cap_t cap);
    // The init's notification: the first it created.
    size_t init_notification();
    // The entry threads the walk spawned as `name`, oldest first.
    std::vector<Thread const*> spawned(char const* name);
    // The task the newest of them joined.
    Task const* instance(char const* name);
    // Whether an endpoint object has a holder of HANDOUT left.
    bool handout(size_t object);
    // Adds a live task the init did not create, as another creator's.
    kos_task_t foreign_task();

    // What reached the console, through the kernel's or through a published console's receiver:
    // a kernel console write is dropped from a publish until the published endpoint's last WAIT
    // goes, which gives the kernel its console back (kernel/init/console.cc).
    std::string const& console();
    // Whether the published console's receiver takes what is sent it from now on.
    void set_console_receives(bool receives);
    // The endpoint object published as the console, or -1.
    int64_t stdout_object();
    // How many times the kernel took its console back.
    uint32_t reclaims();

    Peaks const& peaks();
}

#endif
