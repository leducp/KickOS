// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the instances of the image C++ checks and the host program that links them name of each
// other: each instance keeps only its renamed `main` global, and calls the host program's hooks.

#ifndef KICKOS_TESTS_UNIT_IMAGECXX_IMAGECXX_H
#define KICKOS_TESTS_UNIT_IMAGECXX_IMAGECXX_H

extern "C"
{
    void imagecxx_note_typeinfo(char const* instance, void const* typeinfo);
    void imagecxx_cross(void);
    void imagecxx_note_exit(char const* instance);

    int imagecxx_throw_main(int argc, char** argv);
    int imagecxx_catch_main(int argc, char** argv);
    int imagecxx_exit_a_main(int argc, char** argv);
    int imagecxx_exit_b_main(int argc, char** argv);
}

// One definition in every instance that includes it, which only the localization keeps apart.
inline char const* imagecxx_instance = nullptr;

#endif
