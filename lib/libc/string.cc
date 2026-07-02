// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/libc/string.h>

#include <stdint.h>

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
  unsigned char* d = static_cast<unsigned char*>(dst);
  const unsigned char* s = static_cast<const unsigned char*>(src);
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

void* memset(void* dst, int c, size_t n) {
  unsigned char* d = static_cast<unsigned char*>(dst);
  for (size_t i = 0; i < n; i++) d[i] = static_cast<unsigned char>(c);
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  unsigned char* d = static_cast<unsigned char*>(dst);
  const unsigned char* s = static_cast<const unsigned char*>(src);
  if (d == s || n == 0) return dst;
  if (d < s) {
    for (size_t i = 0; i < n; i++) d[i] = s[i];
  } else {
    for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
  }
  return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
  const unsigned char* pa = static_cast<const unsigned char*>(a);
  const unsigned char* pb = static_cast<const unsigned char*>(b);
  for (size_t i = 0; i < n; i++) {
    if (pa[i] != pb[i]) return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
  }
  return 0;
}

size_t strlen(const char* s) {
  size_t n = 0;
  while (s[n] != '\0') n++;
  return n;
}

size_t strnlen(const char* s, size_t maxlen) {
  size_t n = 0;
  while (n < maxlen && s[n] != '\0') n++;
  return n;
}

} // extern "C"
