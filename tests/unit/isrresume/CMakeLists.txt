# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The frame x86_64's interrupt exit resumes, and where it ends the lock span.
kickos_add_unit_test(NAME isr_resume
  SOURCES isr_resume.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")
