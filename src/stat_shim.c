#include <sys/stat.h>

// MMF sysroot lacks libc_nonshared.a, so provide a small stat() shim.
// glibc exports __xstat() in libc; we forward stat() to it.
#ifndef _STAT_VER
#define _STAT_VER 1
#endif

extern int __xstat(int ver, const char *path, struct stat *buf);

int stat(const char *path, struct stat *buf) {
  return __xstat(_STAT_VER, path, buf);
}
