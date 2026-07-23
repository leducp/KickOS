// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default init body: run the app's main. This TU is NOT compiled with -Dmain
// (it does not link kickos_core), so app.h's #ifdef main weak-stamp block never
// fires here.

#include <kickos/sys/init.h>
#include <kickos/app.h>

extern "C" int kickos_default_init_run(int argc, char** argv)
{
    return kickos_app_main(argc, argv);
}
