// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kos_errno catalogue: one line per code carrying the sentence and the short form
// together, so a code cannot be added without the prose it stands for. The short form is
// the enumerator name, which is what a reader greps.

#include <kickos/sys/strerror.h>

#include <kickos/diag.h>
#include <kickos/sys/errno.h>

// The switch that consumes this has no default label, so -Wswitch (-Wall -Werror)
// refuses a code added to kos_errno with no row here. That warning is the only thing
// keeping the two in step; do not add a default.
#define KICKOS_ERRNO_TABLE(X)                                                                     \
    X(KOS_EPERM,      "privilege denied, missing capability right, or not the owner", "EPERM")    \
    X(KOS_ESRCH,      "reply target gone: the caller is stale",                       "ESRCH")    \
    X(KOS_EBADF,      "handle names nothing valid",                                   "EBADF")    \
    X(KOS_ENOMEM,     "a pool, arena or descriptor budget could not allocate",        "ENOMEM")   \
    X(KOS_EFAULT,     "buffer not owned by the caller",                               "EFAULT")   \
    X(KOS_EBUSY,      "resource held or in use",                                      "EBUSY")    \
    X(KOS_EINVAL,     "malformed argument",                                           "EINVAL")   \
    X(KOS_EMFILE,     "the capability table has no free slot",                        "EMFILE")   \
    X(KOS_EPIPE,      "endpoint has no receiver",                                     "EPIPE")    \
    X(KOS_EDEADLK,    "self, recursive, or cycle-closing lock",                       "EDEADLK")  \
    X(KOS_ENOSYS,     "not implemented on this chip",                                 "ENOSYS")   \
    X(KOS_EOVERFLOW,  "a bounded counter is at its ceiling",                          "EOVERFLOW")\
    X(KOS_ENOTSUP,    "well formed, but this backend cannot express it",             "ENOTSUP")  \
    X(KOS_ETIMEDOUT,  "the deadline passed and nothing happened",                     "ETIMEDOUT")\
    X(KOS_ECANCELED,  "this thread was cancelled and is expected to exit",            "ECANCELED")\
    X(KOS_EOWNERDEAD, "mutex ACQUIRED but the prior owner died holding it",           "EOWNERDEAD")

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
#define KICKOS_ERRNO_CASE(sym, full, terse) \
    case sym:                               \
    {                                       \
        return KICKOS_DIAG_PICK(full, terse); \
    }
            KICKOS_ERRNO_TABLE(KICKOS_ERRNO_CASE)
#undef KICKOS_ERRNO_CASE
        }
    }
    return KICKOS_DIAG_PICK("unknown error code", "E?");
}
