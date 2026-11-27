// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The user's console contract, the same on every posture: writers racing on one console each
// see every line go out whole and in their order, a non-blocking write goes whole or is answered
// EAGAIN, a write longer than a chunk goes out whole, and a writer killed in a write leaves the
// console usable. Run as an image's own app,
// and as an AMP node's whose stdout is another node's relay.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace
{
    constexpr int WRITERS = 4;
    constexpr int LINES = 32;
    constexpr int NONBLOCKING = 64;
    constexpr int PAYLOAD = 40;
    // Past KICKOS_KCONSOLE_CHUNK.
    constexpr int LONG = 200;
    constexpr uint8_t WRITER_PRIO = 10;
    // Below main, which preempts it to kill it whether or not its write waits.
    constexpr uint8_t DOOMED_PRIO = 4;

    kos_sem_t g_go = KOS_SEM_INIT(0);
    bool g_short[WRITERS];

    void say(char const* s)
    {
        (void)write(STDOUT_FILENO, s, strlen(s));
    }

    // `ampcon <tag> i<i> ` then PAYLOAD copies of one letter: its length.
    int line(char* buf, size_t cap, char const* tag, int key, int i)
    {
        int n = snprintf(buf, cap, "ampcon %s i%d ", tag, i);
        char const letter = static_cast<char>('a' + (key * 7 + i) % 26);
        for (int k = 0; k < PAYLOAD; k++)
        {
            buf[n] = letter;
            n++;
        }
        buf[n] = '\n';
        return n + 1;
    }

    void writer(void* arg)
    {
        int const w = static_cast<int>(reinterpret_cast<intptr_t>(arg));
        char tag[8];
        (void)snprintf(tag, sizeof(tag), "w%d", w);
        (void)kos_sem_wait(&g_go, KOS_NEVER);
        for (int i = 0; i < LINES; i++)
        {
            char buf[96];
            int const n = line(buf, sizeof(buf), tag, w, i);
            if (write(STDOUT_FILENO, buf, static_cast<size_t>(n)) != n)
            {
                g_short[w] = true;
            }
        }
    }

    void doomed(void*)
    {
        int i = 0;
        while (true)
        {
            char buf[96];
            int const n = line(buf, sizeof(buf), "k", 0, i);
            (void)write(STDOUT_FILENO, buf, static_cast<size_t>(n));
            i++;
        }
    }
}

int main(void)
{
    char head[64];
    (void)snprintf(head, sizeof(head), "ampcon: %d writers x %d lines\n", WRITERS, LINES);
    say(head);

    kos::thread::Handle writers[WRITERS];
    for (int w = 0; w < WRITERS; w++)
    {
        writers[w] = kos::thread::create(writer, reinterpret_cast<void*>(static_cast<intptr_t>(w)),
                                         "writer", WRITER_PRIO);
    }
    for (int w = 0; w < WRITERS; w++)
    {
        (void)kos_sem_post(&g_go);
    }
    bool whole = true;
    for (int w = 0; w < WRITERS; w++)
    {
        if (not writers[w].valid() or writers[w].join() != 0 or g_short[w])
        {
            whole = false;
        }
    }
    if (whole)
    {
        say("ampcon: every blocking write returned whole\n");
    }
    else
    {
        say("ampcon: a blocking write returned short\n");
    }

    int const flags = fcntl(STDOUT_FILENO, F_GETFL);
    (void)fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
    int taken = 0;
    int again = 0;
    int other = 0;
    for (int i = 0; i < NONBLOCKING; i++)
    {
        char buf[96];
        int const n = line(buf, sizeof(buf), "nb", 0, i);
        ssize_t const r = write(STDOUT_FILENO, buf, static_cast<size_t>(n));
        if (r == n)
        {
            taken++;
        }
        else if (r < 0 and errno == EAGAIN)
        {
            again++;
        }
        else
        {
            other++;
        }
    }
    (void)fcntl(STDOUT_FILENO, F_SETFL, flags);
    char tally[96];
    (void)snprintf(tally, sizeof(tally), "ampcon: nonblocking %d whole, %d EAGAIN, %d other\n", taken,
                   again, other);
    say(tally);

    char longline[LONG];
    int at = snprintf(longline, sizeof(longline), "ampcon long ");
    while (at < LONG - 1)
    {
        longline[at] = 'q';
        at++;
    }
    longline[at] = '\n';
    if (write(STDOUT_FILENO, longline, sizeof(longline)) != LONG)
    {
        say("ampcon: the long write returned short\n");
    }

    kos::thread::Handle const victim = kos::thread::create(doomed, nullptr, "doomed", DOOMED_PRIO);
    kos::sleep_for(std::chrono::milliseconds(20));
    bool const killed = victim.valid() and victim.cancel(KOS_CANCEL_KILL) == 0 and victim.join() == 0;
    if (killed)
    {
        say("ampcon: a writer killed in its write ended\n");
    }
    else
    {
        say("ampcon: a writer killed in its write did not end\n");
    }
    say("ampcon: end\n");
    return 0;
}
