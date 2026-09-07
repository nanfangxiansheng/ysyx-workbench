#include <klib.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n] != '\0') {
    n++;
  }
  return n;
}

char *strcpy(char *dst, const char *src) {
  char *p = dst;
  while ((*p++ = *src++) != '\0');
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i] != '\0'; i++) {
    dst[i] = src[i];
  }
  for (; i < n; i++) { // src不足n时剩余部分补'\0'
    dst[i] = '\0';
  }
  return dst;
}

char *strcat(char *dst, const char *src) {
  char *p = dst + strlen(dst);
  while ((*p++ = *src++) != '\0');
  return dst;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 != '\0' && *s1 == *s2) {
    s1++;
    s2++;
  }
  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s1[i] != s2[i]) {
      return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
    }
    if (s1[i] == '\0') { // 同时到达结尾
      return 0;
    }
  }
  return 0;
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  for (; n > 0; n--) {
    *p++ = (unsigned char)c;
  }
  return s;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d < s) { // 从前往后拷贝
    for (; n > 0; n--) {
      *d++ = *s++;
    }
  } else if (d > s) { // 可能重叠, 从后往前拷贝
    d += n;
    s += n;
    for (; n > 0; n--) {
      *--d = *--s;
    }
  }
  return dst;
}

void *memcpy(void *out, const void *in, size_t n) {
  unsigned char *d = out;
  const unsigned char *s = in;
  for (; n > 0; n--) {
    *d++ = *s++;
  }
  return out;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *a = s1;
  const unsigned char *b = s2;
  for (; n > 0; n--, a++, b++) {
    if (*a != *b) {
      return (int)*a - (int)*b;
    }
  }
  return 0;
}

char *strchr(const char *s, int c) {
  do {
    if (*s == c) return (char *)s;
    if (*s == '\0') break;
    s ++;
  } while (1);
  return NULL;
}

char *strrchr(const char *s, int c) {
  const char *p = s + strlen(s);
  do {
    if (*p == c) return (char *)p;
    if (s == p) break;
    p --;
  } while (1);
  return NULL;
}

#endif
