#include "archive_io.h"

#include <errno.h>
#include <string.h>

int archive_stdio_setup(FILE *file, const char *path, const char *mode) {
  if (!file || !path || !mode || mode[0] != 'r' || strchr(mode, '+')) return 0;
  const size_t length = strlen(path);
  if (length < 8) return 0;
  const char *suffix = path + length - 8;
  if (suffix[0] != '_' || suffix[1] < '0' || suffix[1] > '9' ||
      suffix[2] < '0' || suffix[2] > '9' || suffix[3] < '0' || suffix[3] > '9' ||
      strcmp(suffix + 4, ".vpk")) return 0;

  // Source already caches these archive files in 1 MiB blocks. Newlib's extra
  // stdio buffer splits each block into two SD requests (1 MiB - 1 KiB, 1 KiB).
  const int saved_errno = errno;
  const int configured = setvbuf(file, NULL, _IONBF, 0) == 0;
  errno = saved_errno;
  return configured;
}
