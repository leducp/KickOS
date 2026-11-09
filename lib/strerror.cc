// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kos_errno catalogue: one line per code carrying its sentence.

#include <kickos/sys/strerror.h>

#include <kickos/sys/errno.h>

// The switch that consumes this has no default label, so -Wswitch (-Wall -Werror)
// refuses a code added to kos_errno with no row here. That warning is the only thing
// keeping the two in step; do not add a default.
#define KICKOS_ERRNO_TABLE(X)                                                                     \
    X(KOS_EPERM,      "privilege or authority denied, or not the owner")                          \
    X(KOS_ENOENT,     "no program or declared instance of that name")                             \
    X(KOS_ESRCH,      "reply target gone: the caller is stale")                                   \
    X(KOS_EIO,        "the device did not play its part in the transfer")                         \
    X(KOS_EBADF,      "handle names nothing valid")                                               \
    X(KOS_EAGAIN,     "a later try may succeed; check for partial effects")                       \
    X(KOS_EACCES,     "the capability lacks a right the operation needs")                         \
    X(KOS_ENOMEM,     "a pool, arena or descriptor budget could not allocate")                    \
    X(KOS_EFAULT,     "buffer not owned by the caller")                                           \
    X(KOS_EBUSY,      "resource held or in use")                                                  \
    X(KOS_EINVAL,     "malformed argument")                                                       \
    X(KOS_EMFILE,     "the capability table has no free slot")                                    \
    X(KOS_EPIPE,      "the server died holding the request")                                      \
    X(KOS_EDEADLK,    "self, recursive or cycle-closing lock, or a wait on one's own task")       \
    X(KOS_ENOSYS,     "not implemented on this chip")                                             \
    X(KOS_ENOTIFY,    "an interrupt notification ended the wait, no message")                     \
    X(KOS_EOVERFLOW,  "a count is at its maximum and cannot take one more")                       \
    X(KOS_ENOTSUP,    "well formed, but this backend cannot express it")                          \
    X(KOS_ETIMEDOUT,  "the deadline passed and nothing happened")                                 \
    X(KOS_ECONNREFUSED, "no receiver, and nothing left that could hand one out")                  \
    X(KOS_EALREADY,   "already in that state, so the call changed nothing")                       \
    X(KOS_ECANCELED,  "this thread was cancelled and is expected to exit")                        \
    X(KOS_EOWNERDEAD, "mutex ACQUIRED but the prior owner died holding it")

// Casting a value the enumeration cannot represent is what the guard below prevents; the
// range is the smallest bit-field holding every enumerator, so this is the bound to test.
static_assert(KOS_EOWNERDEAD < 256, "kos_errno no longer fits the range kos_strerror guards");

char const* kos_strerror(int rc)
{
    unsigned magnitude = static_cast<unsigned>(rc);
    if (rc < 0)
    {
        magnitude = 0u - static_cast<unsigned>(rc); // negating rc itself is UB at INT_MIN
    }
    if (magnitude < 256u)
    {
        switch (static_cast<kos_errno>(magnitude))
        {
#define KICKOS_ERRNO_CASE(sym, text) \
    case sym:                        \
    {                                \
        return text;                 \
    }
            KICKOS_ERRNO_TABLE(KICKOS_ERRNO_CASE)
#undef KICKOS_ERRNO_CASE
        }
    }
    return "unknown error code";
}
