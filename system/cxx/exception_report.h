// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_SYSTEM_CXX_EXCEPTION_REPORT_H
#define KICKOS_SYSTEM_CXX_EXCEPTION_REPORT_H

namespace kickos::cxx
{
    // Null unless exception_report.cc is linked, which only --wrap=__cxa_allocate_exception
    // pulls in; nothing may reference that object directly.
    extern void (*g_exception_report)();
}

#endif
