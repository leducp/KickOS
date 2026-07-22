// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The OS-agnostic application entry contract (dependency inversion, invariant
// #8). The app writes a plain
//
//     int main(int argc, char** argv)
//
// and kickos_add_application() compiles it with -Dmain=kickos_app_main so the
// app source stays portable -- the same file builds unchanged as a plain hosted
// program or on KickOS. The kernel's boot path calls kickos_app_main after init;
// its int return becomes the process exit status on the sim (a daemon-style app
// simply never returns).
//
// C++ global constructors of an app (and of any library it links, e.g. libstdc++)
// run on MCU targets from the kernel's root thread just before kickos_app_main --
// the kernel is live, so a ctor may use KickOS syscalls -- but they run on ONE
// thread in sequence: an app global ctor MUST NOT block (sleep/wait), or a
// higher-priority thread it already spawned may run before the remaining globals
// are constructed. Do blocking work inside main, not in a global ctor.
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

// Per-app build stamp: the app's OWN source compile time, distinct from the banner's
// `build` line (the CMake-generated image link time). It moves only when the app TU
// itself recompiles, so it tells "did the APP change" vs "was the image relinked".
// Weak: the kernel sees only this declaration and calls through it.
char const* kickos_app_build_stamp(void) __attribute__((weak));

// Defined ONLY in an app TU (the build force-includes this header with
// -Dmain=kickos_app_main, so __DATE__/__TIME__ capture the APP's compile time, not the
// kernel's). Reformats C's "Mmm dd yyyy" + "HH:MM:SS" to "yyyy-mm-dd HH:MM:SS" so it
// ALIGNS with the CMake build stamp. C-compatible (this header is included by C apps):
// no &&/ternary (nested ifs), C casts.
#ifdef main
__attribute__((weak)) char const* kickos_app_build_stamp(void)
{
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* d = __DATE__; // "Mmm dd yyyy" (day space-padded)
    const char* t = __TIME__; // "HH:MM:SS"
    static char b[28];
    int m = 1;
    for (int i = 0; i < 12; i++)
    {
        if (d[0] == mon[i * 3])
        {
            if (d[1] == mon[i * 3 + 1])
            {
                if (d[2] == mon[i * 3 + 2])
                {
                    m = i + 1;
                }
            }
        }
    }
    b[0] = d[7]; b[1] = d[8]; b[2] = d[9]; b[3] = d[10]; // yyyy
    b[4] = '-';
    b[5] = (char)('0' + m / 10);
    b[6] = (char)('0' + m % 10);
    b[7] = '-';
    b[8] = d[4]; // day tens (space-padded)
    if (b[8] == ' ')
    {
        b[8] = '0';
    }
    b[9] = d[5]; // day units
    b[10] = ' ';
    b[11] = t[0]; b[12] = t[1]; b[13] = t[2]; b[14] = t[3];
    b[15] = t[4]; b[16] = t[5]; b[17] = t[6]; b[18] = t[7];
    int i = 19;
#ifdef KICKOS_APP_TZ
    /* KICKOS_APP_TZ arrives as a bare token (e.g. +0200); stringize it here so the
       CMake define carries no quotes to double-escape on a build-dir reconfigure. */
#define KOS_TZ_STR2(x) #x
#define KOS_TZ_STR(x) KOS_TZ_STR2(x)
    b[i] = ' ';
    i++;
    const char* z = KOS_TZ_STR(KICKOS_APP_TZ);
    for (int j = 0; z[j] != '\0'; j++)
    {
        b[i] = z[j];
        i++;
    }
#undef KOS_TZ_STR
#undef KOS_TZ_STR2
#endif
    b[i] = '\0';
    return b;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
