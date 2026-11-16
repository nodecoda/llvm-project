/* Chapter 08: the two freestanding helpers clang may emit for struct/array
 * initialization.  There is no libc in the kernel, so the link fails without
 * them as soon as a loop is recognized as memset or a struct is copied. */
#include <stddef.h>

void *memset(void *dst, int value, size_t count) {
  unsigned char *p = (unsigned char *)dst;
  while (count--)
    *p++ = (unsigned char)value;
  return dst;
}

void *memcpy(void *dst, const void *src, size_t count) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  while (count--)
    *d++ = *s++;
  return dst;
}
