// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The OS-agnostic application entry contract (dependency inversion, invariant
// #8). The app writes a plain
//
//     int main(int argc, char** argv)
//
// and kickos_add_application() compiles it with -Dmain=kickos_app_main (a portable-main convention) so the app source stays portable -- the same file builds on KickOS or a plain host. The kernel's boot path calls kickos_app_main after init;
// its int return becomes the process exit status on the sim (a daemon-style app
// simply never returns).
//
// This declaration is force-included into every app TU by the build. Declaring
// it extern "C" gives the -Dmain-renamed C++ `main` C language linkage, so it
// resolves to the unmangled symbol the kernel calls (a C app already matches).

#ifndef KICKOS_APP_H
#define KICKOS_APP_H

#ifdef __cplusplus
extern "C"
{
#endif

int kickos_app_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif
