// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Storage for the kernel -> init argument handoff (see <kickos/sys/init.h> for why
// it must be app-side). Its own TU, and in libkickos_user.a rather than the init
// provider's archive: kmain references the object unconditionally, so a build that
// names its own KICKOS_INIT_PROVIDER must not be able to take the definition away.

#include <kickos/sys/init.h>

// No linkage-specification on purpose: init.h declared this name inside extern "C",
// so this definition keeps C linkage ([dcl.link]). `extern "C" struct ... = ...`
// parses as an extern declaration carrying an initializer and is rejected.
struct kos_init_args kickos_init_args = {0, nullptr};
