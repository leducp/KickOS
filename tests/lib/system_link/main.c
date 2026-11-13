// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifdef KICKOS_PROBE_TLS
int kickos_probe_tls_read(void);
#endif
#ifdef KICKOS_PROBE_CAPTURE
int kickos_probe_capture(void);
#endif

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
#ifdef KICKOS_PROBE_TLS
    return kickos_probe_tls_read();
#elif defined(KICKOS_PROBE_CAPTURE)
    return kickos_probe_capture();
#else
    return 0;
#endif
}
