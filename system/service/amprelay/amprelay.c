// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The owner node's half of another node's standard output: it serves the one /amp/ port its
// declaration names, and writes each message it receives in one write on its
// own standard output. A call is made again, as a call, on that standard output, and its answer
// is the reply: a console request such as a drain reaches the console that serves the relay.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <kickos/sys.h>
#include <kickos/sys/table.h>

// The decimal digits of `value` at `out`, which has room for ten: their count.
static size_t put_decimal(char* out, uint32_t value)
{
    char digits[10];
    size_t d = 0;
    while (true)
    {
        digits[d] = (char)('0' + value % 10u);
        d++;
        value = value / 10u;
        if (value == 0u)
        {
            break;
        }
    }
    size_t n = 0;
    while (d > 0u)
    {
        d--;
        out[n] = digits[d];
        n++;
    }
    return n;
}

// `/amp/<port>` into `name`, which holds 16 bytes.
static void port_name(char* name, uint32_t port)
{
    char const prefix[] = "/amp/";
    size_t n = 0;
    while (prefix[n] != '\0')
    {
        name[n] = prefix[n];
        n++;
    }
    n += put_decimal(name + n, port);
    name[n] = '\0';
}

// Writes `what`, then the error number of `error` and a newline.
static void say_error(char const* what, size_t len, int32_t error)
{
    char line[96];
    size_t n = 0;
    while (n < len and n < sizeof(line) - 12u)
    {
        line[n] = what[n];
        n++;
    }
    n += put_decimal(line + n, (uint32_t)(-error));
    line[n] = '\n';
    n++;
    (void)write(STDOUT_FILENO, line, n);
}

// The port this task serves, as `/amp/<port>`, or KOS_CAP_NONE.
static kos_cap_t served_port(void)
{
    kos_self_t const* const self = kos_self();
    if (self == NULL)
    {
        return KOS_CAP_NONE;
    }
    struct kos_table_header const* const h = kickos_table;
    struct kos_table_task const* const tasks = (struct kos_table_task const*)(h + 1);
    struct kos_table_grant const* const grants =
        (struct kos_table_grant const*)(tasks + h->task_count);
    for (uint16_t k = 0; k < self->grant_count; k++)
    {
        struct kos_table_grant const* const g = &grants[self->first_grant + k];
        if (g->kind != KOS_GRANT_PORT or (g->flags & KOS_CAP_WAIT) == 0u)
        {
            continue;
        }
        char name[16];
        port_name(name, (uint32_t)g->base);
        return kos_grant_endpoint(name);
    }
    return KOS_CAP_NONE;
}

int main(void)
{
    kos_cap_t const ep = served_port();
    if (ep == KOS_CAP_NONE)
    {
        static char const NONE[] = "amprelay: serves no /amp/ port\n";
        (void)write(STDOUT_FILENO, NONE, sizeof(NONE) - 1u);
        return 1;
    }
    char msg[KOS_EP_MSG_MAX];
    struct kos_reply_recv_opts opts;
    kos_cap_t reply = KOS_CAP_NONE;
    size_t answer = 0;
    while (true)
    {
        kos_reply_recv_opts_init(&opts, ep, 0u, KOS_NEVER);
        int32_t const n =
            kos_reply_recv(reply, msg, kos_call_lens_pack(answer, sizeof(msg)), &opts);
        reply = KOS_CAP_NONE;
        answer = 0;
        if (n == -KOS_ECANCELED)
        {
            continue;
        }
        if (n < 0)
        {
            static char const REFUSED[] = "amprelay: its port refused a receive, error ";
            say_error(REFUSED, sizeof(REFUSED) - 1u, n);
            return 1;
        }
        reply = opts.info.reply_cap;
        if (reply != KOS_CAP_NONE)
        {
            // A refused forward is answered empty, which its caller reads as no answer.
            int32_t const got = kos_call(KOS_CAP_STDOUT, msg, (size_t)n, sizeof(msg), KOS_NEVER);
            if (got > 0)
            {
                answer = (size_t)got;
            }
        }
        else if (n > 0)
        {
            (void)write(STDOUT_FILENO, msg, (size_t)n);
        }
    }
}
