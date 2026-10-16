/* SPDX-License-Identifier: GPL-2.0-or-later */
/* From shadPS4 guest/custom/v1/runtime.c: the memory functions a freestanding
 * payload may need, with no import outside the package. */
#include "xrg_guest.h"
void *memcpy(void *d, const void *s, xrg_size n) {
  unsigned char *a = d;
  const unsigned char *b = s;
  for (xrg_size i = 0; i < n; ++i)
    a[i] = b[i];
  return d;
}
void *memmove(void *d, const void *s, xrg_size n) {
  unsigned char *a = d;
  const unsigned char *b = s;
  if ((__UINTPTR_TYPE__)a < (__UINTPTR_TYPE__)b)
    return memcpy(d, s, n);
  for (xrg_size i = n; i; --i)
    a[i - 1] = b[i - 1];
  return d;
}
void *memset(void *d, int v, xrg_size n) {
  unsigned char *a = d;
  for (xrg_size i = 0; i < n; ++i)
    a[i] = (unsigned char)v;
  return d;
}
int memcmp(const void *a, const void *b, xrg_size n) {
  const unsigned char *x = a;
  const unsigned char *y = b;
  for (xrg_size i = 0; i < n; ++i)
    if (x[i] != y[i])
      return x[i] - y[i];
  return 0;
}
