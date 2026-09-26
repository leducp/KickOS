// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Native x86 contention probe for M9.4.1. This is a synthetic ceiling for a
// partitioned BKL, not a KickOS SMP throughput measurement.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sched.h>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <pthread.h>
#include <cpuid.h>

namespace
{
    struct alignas(64) TicketLock
    {
        alignas(64) std::atomic<uint32_t> next{0};
        alignas(64) std::atomic<uint32_t> serving{0};

        void lock()
        {
            uint32_t const ticket = next.fetch_add(1, std::memory_order_relaxed);
            while (serving.load(std::memory_order_acquire) != ticket)
            {
                __asm__ volatile("pause" ::: "memory");
            }
        }

        void unlock()
        {
            uint32_t const ticket = serving.load(std::memory_order_relaxed);
            serving.store(ticket + 1, std::memory_order_release);
        }
    };

    uint64_t rdtsc()
    {
        unsigned lo = 0;
        unsigned hi = 0;
        __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    void busy_cycles(unsigned cycles)
    {
        uint64_t const until = rdtsc() + cycles;
        while (rdtsc() < until)
        {
            __asm__ volatile("pause" ::: "memory");
        }
    }

    int read_topology(int cpu, char const* name)
    {
        char path[128];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/%s", cpu,
                      name);
        FILE* const f = std::fopen(path, "r");
        if (f == nullptr)
        {
            std::perror(path);
            std::exit(1);
        }
        int value = -1;
        if (std::fscanf(f, "%d", &value) != 1)
        {
            std::fprintf(stderr, "cannot read %s\n", path);
            std::exit(1);
        }
        std::fclose(f);
        return value;
    }

    struct CpuList
    {
        std::vector<int> physical;
        std::vector<int> siblings;
    };

    CpuList discover_cpus()
    {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        {
            std::perror("sched_getaffinity");
            std::exit(1);
        }
        CpuList list;
        std::set<std::pair<int, int>> seen;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        {
            if (!CPU_ISSET(cpu, &allowed))
            {
                continue;
            }
            auto const key = std::make_pair(read_topology(cpu, "physical_package_id"),
                                            read_topology(cpu, "core_id"));
            if (seen.insert(key).second)
            {
                list.physical.push_back(cpu);
            }
            else
            {
                list.siblings.push_back(cpu);
            }
        }
        return list;
    }

    bool invariant_tsc()
    {
        unsigned eax = 0;
        unsigned ebx = 0;
        unsigned ecx = 0;
        unsigned edx = 0;
        if (__get_cpuid_max(0x80000000, nullptr) < 0x80000007)
        {
            return false;
        }
        __cpuid(0x80000007, eax, ebx, ecx, edx);
        return (edx & (1u << 8)) != 0;
    }

    struct Run
    {
        unsigned threads;
        unsigned hold;
        unsigned outside;
        bool sharded;
        std::vector<int> const* cpus;
        std::atomic<unsigned> ready{0};
        std::atomic<bool> start{false};
        std::atomic<bool> stop{false};
        std::atomic<bool> affinity_failed{false};
        std::unique_ptr<TicketLock[]> locks;

        Run(unsigned n, unsigned h, unsigned o, bool s, std::vector<int> const* available)
            : threads(n), hold(h), outside(o), sharded(s), cpus(available), locks(new TicketLock[n])
        {
        }
    };

    struct alignas(64) Worker
    {
        Run* run;
        unsigned index;
        uint64_t operations = 0;
    };

    void* work(void* arg)
    {
        Worker* const worker = static_cast<Worker*>(arg);
        Run& run = *worker->run;
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((*run.cpus)[worker->index], &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        {
            run.affinity_failed.store(true, std::memory_order_relaxed);
        }
        run.ready.fetch_add(1, std::memory_order_release);
        while (!run.start.load(std::memory_order_acquire))
        {
            __asm__ volatile("pause" ::: "memory");
        }
        unsigned lock_index = 0;
        if (run.sharded)
        {
            lock_index = worker->index;
        }
        TicketLock& lock = run.locks[lock_index];
        uint64_t operations = 0;
        while (!run.stop.load(std::memory_order_relaxed))
        {
            busy_cycles(run.outside);
            lock.lock();
            busy_cycles(run.hold);
            lock.unlock();
            ++operations;
        }
        worker->operations = operations;
        return nullptr;
    }

    double run_once(unsigned n, unsigned hold, unsigned outside, bool sharded,
                    double seconds, std::vector<int> const& cpus)
    {
        Run run(n, hold, outside, sharded, &cpus);
        std::unique_ptr<Worker[]> workers(new Worker[n]);
        std::unique_ptr<pthread_t[]> threads(new pthread_t[n]);
        for (unsigned i = 0; i < n; ++i)
        {
            workers[i].run = &run;
            workers[i].index = i;
            int const error = pthread_create(&threads[i], nullptr, work, &workers[i]);
            if (error != 0)
            {
                std::fprintf(stderr, "pthread_create: %s\n", std::strerror(error));
                std::exit(1);
            }
        }
        while (run.ready.load(std::memory_order_acquire) != n)
        {
            std::this_thread::yield();
        }
        if (run.affinity_failed.load(std::memory_order_relaxed))
        {
            std::fprintf(stderr, "could not pin all workers\n");
            std::exit(1);
        }
        auto const begin = std::chrono::steady_clock::now();
        run.start.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        run.stop.store(true, std::memory_order_release);
        auto const end = std::chrono::steady_clock::now();
        uint64_t total = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            pthread_join(threads[i], nullptr);
            total += workers[i].operations;
        }
        return total / std::chrono::duration<double>(end - begin).count();
    }

    double median(std::vector<double>& values)
    {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    void print_cpus(char const* name, std::vector<int> const& cpus)
    {
        std::printf("# %s=", name);
        for (size_t i = 0; i < cpus.size(); ++i)
        {
            if (i != 0)
            {
                std::printf(",");
            }
            std::printf("%d", cpus[i]);
        }
        std::printf("\n");
    }
}

int main(int argc, char** argv)
{
    // Keep runs short enough for repeated, pinned measurements without a long test gate.
    double seconds = 0.5;
    int repeats = 3;
    if (argc >= 2)
    {
        seconds = std::atof(argv[1]);
    }
    if (argc >= 3)
    {
        repeats = std::atoi(argv[2]);
    }
    if (argc > 3 || seconds <= 0 || repeats < 1 || repeats % 2 == 0)
    {
        std::fprintf(stderr, "usage: %s [seconds-per-case [odd-repeat-count]]\n", argv[0]);
        return 2;
    }
    if (!invariant_tsc())
    {
        std::fprintf(stderr, "CPU has no invariant TSC; cycle-duration work is not valid\n");
        return 2;
    }
    CpuList const discovered = discover_cpus();
    std::vector<int> cpus = discovered.physical;
    cpus.insert(cpus.end(), discovered.siblings.begin(), discovered.siblings.end());
    if (discovered.physical.empty())
    {
        std::fprintf(stderr, "no usable physical cores\n");
        return 2;
    }
    std::printf("# SPDX-License-Identifier: CECILL-C\n");
    std::printf("# Copyright (c) 2026 Philippe Leduc\n");
    print_cpus("physical_cpus", discovered.physical);
    print_cpus("smt_siblings", discovered.siblings);
    std::printf("# seconds_per_case=%.3f repeats=%d invariant_tsc=1\n", seconds, repeats);
    std::printf("threads\thold_cycles\toutside_cycles\tmode\tmedian_ops_per_s\tmin_ops_per_s\tmax_ops_per_s\n");

    unsigned const counts[] = {1, 2, 4, 8, 12, 24};
    unsigned const holds[] = {256, 512, 2048};
    unsigned const outside[] = {1024, 8192};
    for (unsigned h : holds)
    {
        for (unsigned o : outside)
        {
            for (unsigned n : counts)
            {
                if (n > cpus.size() || (n > discovered.physical.size() && n != 24))
                {
                    continue;
                }
                for (unsigned mode = 0; mode < 2; ++mode)
                {
                    std::vector<double> samples;
                    for (int rep = 0; rep < repeats; ++rep)
                    {
                        samples.push_back(run_once(n, h, o, mode != 0, seconds, cpus));
                    }
                    double const mid = median(samples);
                    char const* mode_name = "global";
                    if (mode != 0)
                    {
                        mode_name = "sharded";
                    }
                    std::printf("%u\t%u\t%u\t%s\t%.0f\t%.0f\t%.0f\n", n, h, o,
                                mode_name, mid, samples.front(),
                                samples.back());
                    std::fflush(stdout);
                }
            }
        }
    }
}
