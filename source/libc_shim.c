/* libc_shim.c -- bionic-compatible libc wrappers for the HL2 Source modules
 *
 * The Source libs are linked against bionic. Where the bionic and newlib
 * ABIs differ (struct layouts, flag values, clock ids, missing functions) we
 * provide converting wrappers here; everything that matches is passed
 * straight through from imports.c.
 *
 * Covers mmap, scandir, statfs64, fnmatch, clock id conversion, socket stubs,
 * /proc/cpuinfo + /proc/meminfo synthesis, and per-thread fake bionic TLS.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"
#include "dl_emu.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}

char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}

char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}

size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen;
  return strlen(s);
}

char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncat(dst, src, n);
}

char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncpy(dst, src, n);
}

// newlib vsnprintf tracks remaining space in a signed int, so fortify's SIZE_MAX
// "unknown size" bound becomes -1 and it writes nothing. Clamp to INT_MAX.
static size_t pf_bound(size_t n) { return n > 0x7fffffff ? (size_t)0x7fffffff : n; }

int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsnprintf(s, pf_bound(maxlen), fmt, va);
}

// bionic fd_set is 1024 bits of unsigned long
void __FD_SET_chk_fake(int fd, void *set, size_t set_size) {
  (void)set_size;
  if (fd >= 0 && fd < 1024)
    ((unsigned long *)set)[fd / 64] |= 1ul << (fd % 64);
}

int __FD_ISSET_chk_fake(int fd, const void *set, size_t set_size) {
  (void)set_size;
  if (fd >= 0 && fd < 1024)
    return (int)((((const unsigned long *)set)[fd / 64] >> (fd % 64)) & 1ul);
  return 0;
}

void *__memset_chk_fake(void *s, int c, size_t n, size_t slen) {
  (void)slen;
  return memset(s, c, n);
}

long __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) {
  (void)buflen;
  return read(fd, buf, count);
}

long __recvfrom_chk_fake(int fd, void *buf, size_t len, size_t buflen,
                         int flags, void *from, int *fromlen) {
  (void)buflen;
  return sock_recvfrom_fake(fd, buf, len, flags, from, fromlen);
}

int __snprintf_chk_fake(char *s, size_t n, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va;
  va_start(va, fmt);
  int r = vsnprintf(s, pf_bound(n), fmt, va);
  va_end(va);
  return r;
}

int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...) {
  (void)flag;
  va_list va;
  va_start(va, fmt);
  int r = vsnprintf(s, pf_bound(slen), fmt, va);
  va_end(va);
  return r;
}

int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag;
  return vsnprintf(s, pf_bound(slen), fmt, va);
}

char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) {
  (void)dstlen; (void)srclen;
  return strncpy(dst, src, n);
}

char *__strrchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strrchr(s, c);
}

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  value[0] = '\0';
  return 0;
}

// Clang-16 libtier0 builds its mutexes/events on raw futex; map onto the Switch
// address arbiter. Everything else returns ENOSYS (nothing else uses syscall()).
long syscall_fake(long number, ...) {
  va_list va;
  va_start(va, number);
  if (number == 98) { // SYS_futex
    int *uaddr = va_arg(va, int *);
    const int op = va_arg(va, int) & 0x7f; // strip FUTEX_PRIVATE_FLAG/CLOCK
    const int val = va_arg(va, int);
    const struct timespec *ts = va_arg(va, const struct timespec *);
    va_end(va);
    if (op == 0 || op == 9) { // FUTEX_WAIT
      const s64 timeout = ts ? ((s64)ts->tv_sec * 1000000000LL + ts->tv_nsec)
                             : 1000000000LL; // re-check ~1/s when infinite
      const Result rc = svcWaitForAddress(uaddr, ArbitrationType_WaitIfEqual, (s64)val, timeout);
      if (R_SUCCEEDED(rc))
        return 0;
      errno = (R_VALUE(rc) == KERNELRESULT(TimedOut)) ? 110 : 11; // ETIMEDOUT : EAGAIN
      return -1;
    }
    if (op == 1 || op == 10) { // FUTEX_WAKE
      svcSignalToAddress(uaddr, SignalType_Signal, 0, val);
      return val;
    }
    errno = 38;
    return -1;
  }
  va_end(va);
  errno = 38; // ENOSYS
  return -1;
}

size_t __ctype_get_mb_cur_max_fake(void) {
  return 1; // C locale, single byte
}

// bionic profiling hooks; no-ops here
void __google_potentially_blocking_region_begin_fake(void) {}
void __google_potentially_blocking_region_end_fake(void) {}

// _ctype_ points at the base of a 257-byte BSD table (slot 0 = EOF, slot c+1 =
// char c); libstdc++'s ctype facet reads classic_table() = (*_ctype_) + 1.
static unsigned char g_ctype_table[1 + 256];
const char *bionic_ctype = (const char *)g_ctype_table;

__attribute__((constructor)) static void init_bionic_ctype(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c))            f |= 0x01;
    if (islower(c))            f |= 0x02;
    if (isdigit(c))            f |= 0x04;
    if (isspace(c))            f |= 0x08;
    if (ispunct(c))            f |= 0x10;
    if (iscntrl(c))            f |= 0x20;
    if (isxdigit(c))           f |= 0x40;
    if (c == ' ' || c == '\t') f |= 0x80;
    g_ctype_table[c + 1] = f;
  }
}

unsigned long getauxval_fake(unsigned long type) {
  (void)type;
  return 0;
}

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("abort message: %s\n", msg ? msg : "(null)");
}

int __register_atfork_fake(void) {
  return 0;
}

int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) {
  // threads never exit cleanly in this port; leak instead of running dtors
  (void)fn; (void)arg; (void)dso;
  return 0;
}

void __assert2_fake(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
  abort();
}

// ---------------------------------------------------------------------------
// per-thread fake bionic TLS
//
// libengine.so reads TPIDR_EL0 ~574 times, virtually all of it the bionic
// stack-protector slot ([tp + 40]). libnx leaves TPIDR_EL0 at 0, which would
// fault, so every thread that runs game code installs a zeroed block first.
// The pointer is centered in the block so the few odd negative offsets land
// inside it as well.
// ---------------------------------------------------------------------------

void fake_tls_install(void) {
  if (armGetTlsRw() != NULL)
    return; // already installed for this thread
  uint8_t *block = calloc(1, 0x1000);
  if (!block) {
    debugPrintf("fake_tls_install: out of memory\n");
    return;
  }
  // block is leaked on purpose: it must outlive the thread
  armSetTlsRw(block + 0x800);
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_FIONBIO 0x5421

// defined in the null-socket section below; fcntl/ioctl need them
static int is_fake_sock(int fd);
static void fake_sock_set_nonblock(int fd, int enabled);

static int convert_open_flags(int flags) {
  int out = flags & 3; // O_RDONLY/O_WRONLY/O_RDWR match
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

// the engine reads /proc through the raw fd API too (sys_dll.cpp does
// open("/proc/meminfo") and Sys_Error()s on failure; tier0's debugger check
// reads /proc/self/status). The synthetic contents are staged as real files
// at boot (proc_files_init) and open() is redirected here.
static char proc_meminfo_path[300];
static char proc_cpuinfo_path[300];
static char proc_status_path[300];

static const char *proc_redirect(const char *path) {
  if (!strncmp(path, "/proc/", 6)) {
    if (!strcmp(path, "/proc/meminfo") && proc_meminfo_path[0])
      return proc_meminfo_path;
    if (!strcmp(path, "/proc/cpuinfo") && proc_cpuinfo_path[0])
      return proc_cpuinfo_path;
    if (!strcmp(path, "/proc/self/status") && proc_status_path[0])
      return proc_status_path;
    return NULL;
  }
  return path;
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  path = proc_redirect(path);
  if (!path) {
    errno = ENOENT;
    return -1;
  }
  int fd = open(path, convert_open_flags(flags), mode);
  return fd;
}

int access_fake(const char *path, int mode) {
  (void)mode; // everything that exists is considered readable+writable
  struct stat st;
  int r = stat(path, &st);
  return r;
}

// devkitA64 getcwd() returns the path WITH a device prefix ("sdmc:/switch/
// hl2_nx"), but the env vars we set (APP_DATA_PATH/VALVE_GAME_PATH) are
// prefix-less ("/switch/hl2_nx"). The engine mixes the two when building
// paths, producing malformed write targets (e.g. ".../sdmc:/switch/...") so
// config/save writes fail. Strip the device prefix to keep everything
// consistent; "/switch/..." still resolves to sdmc on devkitA64.
char *getcwd_fake(char *buf, size_t size) {
  char *r = getcwd(buf, size);
  if (r) {
    char *slash = strchr(r, '/');
    char *colon = strchr(r, ':');
    if (colon && slash && colon < slash)
      memmove(r, colon + 1, strlen(colon + 1) + 1);
  }
  return r;
}

int fcntl_fake(int fd, int cmd, ...) {
  int arg = 0;
  if (cmd == LINUX_F_SETFL) {
    va_list va;
    va_start(va, cmd);
    arg = va_arg(va, int);
    va_end(va);
  }
  // the engine sets its UDP socket non-blocking via F_SETFL|O_NONBLOCK;
  // record it on the fake socket and report it back on F_GETFL
  if (is_fake_sock(fd)) {
    if (cmd == LINUX_F_SETFL)
      fake_sock_set_nonblock(fd, (arg & LINUX_O_NONBLOCK) != 0);
    return cmd == LINUX_F_GETFL ? LINUX_O_NONBLOCK : 0;
  }
  return 0;
}

// the engine also flips non-blocking via ioctl(fd, FIONBIO, &on); previously
// this returned -1 ("ioctl FIONBIO: No such file or directory")
int ioctl_fake(int fd, unsigned long req, ...) {
  if (is_fake_sock(fd)) {
    if (req == LINUX_FIONBIO) {
      int on = 0;
      va_list va;
      va_start(va, req);
      int *argp = va_arg(va, int *);
      va_end(va);
      if (argp)
        on = *argp;
      fake_sock_set_nonblock(fd, on != 0);
      return 0;
    }
    errno = EINVAL;
    return -1;
  }
  errno = EBADF;
  return -1;
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, void *st) {
  struct stat real;
  const int ret = stat(path, &real);
  if (ret == 0)
    convert_stat(&real, (struct bionic_stat *)st);
  return ret;
}

int fstat_fake(int fd, void *st) {
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0)
    convert_stat(&real, (struct bionic_stat *)st);
  return ret;
}

int lstat_fake(const char *path, void *st) {
  return stat_fake(path, st);
}

// linux/bionic struct statfs64 on arm64: all fields 64-bit
struct bionic_statfs64 {
  uint64_t f_type;
  uint64_t f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  uint64_t f_fsid;
  uint64_t f_namelen;
  uint64_t f_frsize;
  uint64_t f_flags;
  uint64_t f_spare[4];
};

int statfs64_fake(const char *path, void *buf) {
  (void)path; // report a roomy SD card; used for free-space checks
  struct bionic_statfs64 *out = buf;
  memset(out, 0, sizeof(*out));
  out->f_bsize = 4096;
  out->f_frsize = 4096;
  out->f_blocks = 16ull * 1024 * 1024 * 1024 / 4096;
  out->f_bfree = out->f_bavail = 8ull * 1024 * 1024 * 1024 / 4096;
  out->f_namelen = 255;
  return 0;
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

void *readdir_fake(void *dirp) {
  static __thread struct bionic_dirent out;
  struct dirent *e = readdir((DIR *)dirp);
  if (!e)
    return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// scandir building bionic dirents; filter/compar receive bionic layouts
int scandir_fake(const char *dir, void ***namelist,
                 int (*filter)(const void *),
                 int (*compar)(const void **, const void **)) {
  DIR *d = opendir(dir);
  if (!d)
    return -1;

  struct bionic_dirent **list = NULL;
  int count = 0, cap = 0;

  struct dirent *e;
  while ((e = readdir(d))) {
    struct bionic_dirent be;
    memset(&be, 0, sizeof(be));
    be.d_ino = e->d_ino;
    be.d_reclen = sizeof(be);
    be.d_type = e->d_type;
    snprintf(be.d_name, sizeof(be.d_name), "%s", e->d_name);

    if (filter && !filter(&be))
      continue;

    if (count == cap) {
      cap = cap ? cap * 2 : 32;
      list = realloc(list, cap * sizeof(*list));
    }
    list[count] = malloc(sizeof(be));
    memcpy(list[count], &be, sizeof(be));
    count++;
  }
  closedir(d);

  if (compar && count > 1)
    qsort(list, count, sizeof(*list), (int (*)(const void *, const void *))compar);

  *namelist = (void **)list;
  return count;
}

int alphasort_fake(const void **a, const void **b) {
  const struct bionic_dirent *da = *(const struct bionic_dirent **)a;
  const struct bionic_dirent *db = *(const struct bionic_dirent **)b;
  return strcmp(da->d_name, db->d_name);
}

// ---------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------

// bionic/linux clock ids differ from newlib's
#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_CLOCK_PROCESS_CPUTIME_ID 2
#define LINUX_CLOCK_THREAD_CPUTIME_ID 3
#define LINUX_CLOCK_MONOTONIC_RAW 4
#define LINUX_CLOCK_BOOTTIME 7

int clock_gettime_fake(int clk, struct timespec *ts) {
  switch (clk) {
    case LINUX_CLOCK_REALTIME:
      return clock_gettime(CLOCK_REALTIME, ts);
    case LINUX_CLOCK_MONOTONIC:
    case LINUX_CLOCK_MONOTONIC_RAW:
    case LINUX_CLOCK_BOOTTIME:
    case LINUX_CLOCK_PROCESS_CPUTIME_ID:
    case LINUX_CLOCK_THREAD_CPUTIME_ID: {
      // armGetSystemTick is cheaper and strictly monotonic
      const u64 t = armTicksToNs(armGetSystemTick());
      ts->tv_sec = t / 1000000000ull;
      ts->tv_nsec = t % 1000000000ull;
      return 0;
    }
    default:
      errno = EINVAL;
      return -1;
  }
}

time_t timegm_fake(struct tm *tm) {
  // days from civil algorithm (Howard Hinnant), no TZ dependence
  int y = tm->tm_year + 1900;
  const int m = tm->tm_mon + 1;
  const int d = tm->tm_mday;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = (long long)era * 146097 + (long long)doe - 719468;
  return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FAILED ((void *)-1)

void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, long long off) {
  (void)addr; (void)prot;
  void *p = memalign(0x1000, len);
  if (!p)
    return LINUX_MAP_FAILED;
  memset(p, 0, len);
  if (!(flags & LINUX_MAP_ANONYMOUS) && fd >= 0) {
    // file-backed mapping: emulate with a read; writes won't propagate back,
    // which is fine for the read-only mappings the engine uses
    const off_t old = lseek(fd, 0, SEEK_CUR);
    lseek(fd, (off_t)off, SEEK_SET);
    size_t done = 0;
    while (done < len) {
      const ssize_t r = read(fd, (char *)p + done, len - done);
      if (r <= 0)
        break;
      done += r;
    }
    lseek(fd, old, SEEK_SET);
  }
  return p;
}

int munmap_fake(void *addr, size_t len) {
  (void)len;
  if (addr && addr != LINUX_MAP_FAILED)
    free(addr);
  return 0;
}

// bionic struct mallinfo: 10 size_t fields
void *mallinfo_fake(void *out) {
  memset(out, 0, 10 * sizeof(size_t));
  return out;
}

// ---------------------------------------------------------------------------
// strings/util
// ---------------------------------------------------------------------------

char *basename_fake(const char *path) {
  static __thread char buf[256];
  if (!path || !*path) {
    strcpy(buf, ".");
    return buf;
  }
  const char *slash = strrchr(path, '/');
  snprintf(buf, sizeof(buf), "%s", slash ? slash + 1 : path);
  return buf;
}

void *memrchr_fake(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s + n;
  while (n--) {
    if (*--p == (unsigned char)c)
      return (void *)p;
  }
  return NULL;
}

// minimal fnmatch: *, ?, [set] with ranges and ^/! negation; enough for the
// filesystem's FindFirst-style wildcards
int fnmatch_fake(const char *pattern, const char *string, int flags) {
  (void)flags;
  const char *p = pattern, *s = string;
  const char *star_p = NULL, *star_s = NULL;
  while (*s) {
    if (*p == '*') {
      star_p = ++p;
      star_s = s;
    } else if (*p == '?' || *p == *s) {
      p++;
      s++;
    } else if (*p == '[') {
      const char *set = p + 1;
      int neg = 0, match = 0;
      if (*set == '!' || *set == '^') { neg = 1; set++; }
      while (*set && *set != ']') {
        if (set[1] == '-' && set[2] && set[2] != ']') {
          if (*s >= set[0] && *s <= set[2]) match = 1;
          set += 3;
        } else {
          if (*set == *s) match = 1;
          set++;
        }
      }
      if (*set != ']' || match == neg) {
        if (!star_p) return 1; // FNM_NOMATCH
        p = star_p;
        s = ++star_s;
        continue;
      }
      p = set + 1;
      s++;
    } else if (star_p) {
      p = star_p;
      s = ++star_s;
    } else {
      return 1; // FNM_NOMATCH
    }
  }
  while (*p == '*') p++;
  return *p ? 1 : 0;
}

char *realpath_fake(const char *path, char *resolved) {
  if (!resolved)
    resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}

// bionic strerror_r is the POSIX int-returning variant
int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

char *tmpnam_fake(char *buf) {
  static char internal[64];
  static int counter = 0;
  char *out = buf ? buf : internal;
  sprintf(out, "tmp_%d_%d", gettid_fake(), counter++);
  return out;
}

// ---------------------------------------------------------------------------
// math helpers
// ---------------------------------------------------------------------------

void sincos_fake(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

long double fmodl_fake(long double a, long double b) {
  return (long double)fmod((double)a, (double)b);
}

long double scalbnl_fake(long double x, int n) {
  return (long double)scalbn((double)x, n);
}

// ---------------------------------------------------------------------------
// process/user stubs
// ---------------------------------------------------------------------------

int getpid_fake(void) {
  return 1;
}

int getuid_fake(void) {
  return 0;
}

void *getpwuid_fake(int uid) {
  (void)uid;
  return NULL;
}

int gethostname_fake(char *name, size_t len) {
  snprintf(name, len, "switch");
  return 0;
}

int getrusage_fake(int who, void *usage) {
  (void)who;
  memset(usage, 0, 144); // sizeof(struct rusage) on bionic LP64
  return 0;
}

int system_fake(const char *cmd) {
  debugPrintf("system(%s) ignored\n", cmd ? cmd : "(null)");
  return -1;
}

FILE *popen_fake(const char *cmd, const char *mode) {
  (void)mode;
  debugPrintf("popen(%s) -> NULL\n", cmd ? cmd : "(null)");
  return NULL;
}

int pclose_fake(FILE *f) {
  (void)f;
  return -1;
}

int sched_yield_fake(void) {
  svcSleepThread(0);
  return 0;
}

// ---------------------------------------------------------------------------
// null-socket emulation: the engine REQUIRES a client UDP socket even in
// singleplayer (OpenSocketInternal Sys_Exit()s without one, bypassing
// -noip). SP traffic actually runs over Source's internal loopback buffers,
// not the wire, so the socket just idles -- BUT the engine still parses the
// loopback address, binds, learns its own port and connects to it. So the
// fakes have to behave like real (idle) sockets: bind/connect record the
// address, getsockname/getpeername return it, and inet_*/gethostbyname
// resolve dotted IPv4 + "localhost". Previously these failed, which broke
// the local server<->client handshake when launching a map directly.
// (swap for libnx bsd sockets later if LAN play is wanted)
// ---------------------------------------------------------------------------

#define FAKE_SOCK_BASE 0x40000000
#define FAKE_SOCK_MAX 32
#define BIONIC_AF_INET 2
#define BIONIC_INADDR_NONE 0xffffffffu

// the binaries test errno against bionic values; EBADF/EAGAIN/EINVAL/ENFILE
// match newlib already, but these two differ so spell them out
#define BIONIC_EAFNOSUPPORT 97
#define BIONIC_ENOTCONN 107

// bionic sockaddr_in: u16 family, u16 port (net order), u32 addr, 8 pad
struct bionic_sockaddr_in {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  uint8_t sin_zero[8];
};

typedef struct {
  uint8_t used;
  int nonblock;
  uint16_t port;       // host order
  uint32_t addr;       // network-byte-order packed (a|b<<8|c<<16|d<<24)
  uint16_t peer_port;
  uint32_t peer_addr;
} FakeSock;

static FakeSock fake_socks_tbl[FAKE_SOCK_MAX];

static int fake_sock_index(int fd) {
  if (fd < FAKE_SOCK_BASE || fd >= FAKE_SOCK_BASE + FAKE_SOCK_MAX)
    return -1;
  return fd - FAKE_SOCK_BASE;
}

static int is_fake_sock(int fd) {
  const int i = fake_sock_index(fd);
  return i >= 0 && fake_socks_tbl[i].used;
}

static void fake_sock_set_nonblock(int fd, int enabled) {
  const int i = fake_sock_index(fd);
  if (i >= 0 && fake_socks_tbl[i].used)
    fake_socks_tbl[i].nonblock = enabled;
}

static uint16_t net_bswap16(uint16_t v) {
  return (uint16_t)((v << 8) | (v >> 8));
}

static uint32_t make_ipv4_addr(unsigned a, unsigned b, unsigned c, unsigned d) {
  return (a & 0xffu) | ((b & 0xffu) << 8) | ((c & 0xffu) << 16) | ((d & 0xffu) << 24);
}

static int parse_ipv4_addr(const char *s, uint32_t *out) {
  if (!s || !*s)
    return 0;
  if (!strcmp(s, "localhost")) {
    *out = make_ipv4_addr(127, 0, 0, 1);
    return 1;
  }
  unsigned part[4];
  const char *p = s;
  for (int i = 0; i < 4; i++) {
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || v > 255)
      return 0;
    part[i] = (unsigned)v;
    if (i < 3) {
      if (*end != '.') return 0;
      p = end + 1;
    } else if (*end) {
      return 0;
    }
  }
  *out = make_ipv4_addr(part[0], part[1], part[2], part[3]);
  return 1;
}

static void format_ipv4_addr(uint32_t addr, char *out, size_t out_size) {
  snprintf(out, out_size, "%u.%u.%u.%u",
           (unsigned)(addr & 0xffu), (unsigned)((addr >> 8) & 0xffu),
           (unsigned)((addr >> 16) & 0xffu), (unsigned)((addr >> 24) & 0xffu));
}

static void fill_sockaddr(void *addr, int *addrlen, uint32_t ip, uint16_t port) {
  if (!addr || !addrlen || *addrlen < (int)sizeof(struct bionic_sockaddr_in))
    return;
  struct bionic_sockaddr_in *out = addr;
  memset(out, 0, sizeof(*out));
  out->sin_family = BIONIC_AF_INET;
  out->sin_port = net_bswap16(port);
  out->sin_addr = ip;
  *addrlen = sizeof(*out);
}

int socket_fake(int domain, int type, int protocol) {
  (void)domain; (void)type; (void)protocol;
  for (int i = 0; i < FAKE_SOCK_MAX; i++) {
    if (!fake_socks_tbl[i].used) {
      memset(&fake_socks_tbl[i], 0, sizeof(fake_socks_tbl[i]));
      fake_socks_tbl[i].used = 1;
      return FAKE_SOCK_BASE + i;
    }
  }
  errno = ENFILE;
  return -1;
}

int sock_ok_fake(int fd) { // shutdown and friends on a fake socket
  if (is_fake_sock(fd))
    return 0;
  errno = EBADF;
  return -1;
}

int sock_bind_fake(int fd, const void *addr, int addrlen) {
  const int i = fake_sock_index(fd);
  if (i < 0 || !fake_socks_tbl[i].used) { errno = EBADF; return -1; }
  if (addr && addrlen >= (int)sizeof(struct bionic_sockaddr_in)) {
    const struct bionic_sockaddr_in *in = addr;
    if (in->sin_family == BIONIC_AF_INET) {
      fake_socks_tbl[i].addr = in->sin_addr;
      fake_socks_tbl[i].port = net_bswap16(in->sin_port);
    }
  }
  return 0;
}

int sock_connect_fake(int fd, const void *addr, int addrlen) {
  const int i = fake_sock_index(fd);
  if (i < 0 || !fake_socks_tbl[i].used) { errno = EBADF; return -1; }
  if (addr && addrlen >= (int)sizeof(struct bionic_sockaddr_in)) {
    const struct bionic_sockaddr_in *in = addr;
    if (in->sin_family == BIONIC_AF_INET) {
      fake_socks_tbl[i].peer_addr = in->sin_addr;
      fake_socks_tbl[i].peer_port = net_bswap16(in->sin_port);
    }
  }
  return 0;
}

int sock_listen_fake(int fd, int backlog) {
  (void)backlog;
  return sock_ok_fake(fd);
}

int sock_setsockopt_fake(int fd, int level, int optname, const void *optval, int optlen) {
  (void)level; (void)optname; (void)optval; (void)optlen;
  return sock_ok_fake(fd);
}

long sock_send_fake(int fd, const void *buf, size_t len) {
  (void)buf;
  if (is_fake_sock(fd))
    return (long)len; // pretend it went out
  errno = EBADF;
  return -1;
}

long sock_sendto_fake(int fd, const void *buf, size_t len, int flags, const void *to, int tolen) {
  (void)flags; (void)to; (void)tolen;
  return sock_send_fake(fd, buf, len);
}

long sock_recv_fake(int fd, void *buf, size_t len, int flags) {
  (void)buf; (void)len; (void)flags;
  if (is_fake_sock(fd)) {
    errno = EAGAIN; // nonblocking socket with no data
    return -1;
  }
  errno = EBADF;
  return -1;
}

long sock_recvfrom_fake(int fd, void *buf, size_t len, int flags, void *from, int *fromlen) {
  (void)from; (void)fromlen;
  return sock_recv_fake(fd, buf, len, flags);
}

int sock_getsockopt_fake(int fd, int level, int optname, void *optval, int *optlen) {
  (void)level; (void)optname;
  if (!is_fake_sock(fd)) {
    errno = EBADF;
    return -1;
  }
  if (optval && optlen && *optlen >= 4)
    memset(optval, 0, 4); // no pending error, zero everything
  return 0;
}

int sock_getsockname_fake(int fd, void *addr, int *addrlen) {
  const int i = fake_sock_index(fd);
  if (i < 0 || !fake_socks_tbl[i].used) { errno = EBADF; return -1; }
  fill_sockaddr(addr, addrlen, fake_socks_tbl[i].addr, fake_socks_tbl[i].port);
  return 0;
}

int sock_getpeername_fake(int fd, void *addr, int *addrlen) {
  const int i = fake_sock_index(fd);
  if (i < 0 || !fake_socks_tbl[i].used) { errno = EBADF; return -1; }
  if (!fake_socks_tbl[i].peer_port && !fake_socks_tbl[i].peer_addr) {
    errno = BIONIC_ENOTCONN;
    return -1;
  }
  fill_sockaddr(addr, addrlen, fake_socks_tbl[i].peer_addr, fake_socks_tbl[i].peer_port);
  return 0;
}

int sock_accept_fake(int fd, void *addr, int *addrlen) {
  (void)addr; (void)addrlen;
  if (is_fake_sock(fd)) {
    errno = EAGAIN; // nothing to accept
    return -1;
  }
  errno = EBADF;
  return -1;
}

int close_fake(int fd) {
  const int i = fake_sock_index(fd);
  if (i >= 0 && fake_socks_tbl[i].used) {
    memset(&fake_socks_tbl[i], 0, sizeof(fake_socks_tbl[i]));
    return 0;
  }
  return close(fd);
}

int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res) {
  (void)node; (void)service; (void)hints;
  if (res)
    *res = NULL;
  return 8; // bionic EAI_NONAME (positive codes, unlike glibc)
}

void freeaddrinfo_fake(void *res) {
  (void)res;
}

void *gethostbyname_fake(const char *name) {
  static uint32_t addr;
  static char *addr_list[] = { (char *)&addr, NULL };
  static char *aliases[] = { NULL };
  static char host_name[64];
  static struct {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
  } host;

  uint32_t parsed;
  if (name && (parse_ipv4_addr(name, &parsed) || !strcmp(name, "switch"))) {
    addr = strcmp(name, "switch") ? parsed : make_ipv4_addr(127, 0, 0, 1);
    snprintf(host_name, sizeof(host_name), "%s", name);
    host.h_name = host_name;
    host.h_aliases = aliases;
    host.h_addrtype = BIONIC_AF_INET;
    host.h_length = 4;
    host.h_addr_list = addr_list;
    return &host;
  }
  return NULL;
}

unsigned int inet_addr_fake(const char *cp) {
  uint32_t addr;
  return parse_ipv4_addr(cp, &addr) ? addr : BIONIC_INADDR_NONE;
}

const char *inet_ntop_fake(int af, const void *src, char *dst, unsigned int size) {
  if (af != BIONIC_AF_INET || !src || !dst || size < 16) {
    errno = EINVAL;
    return NULL;
  }
  uint32_t addr;
  memcpy(&addr, src, sizeof(addr));
  format_ipv4_addr(addr, dst, size);
  return dst;
}

int inet_pton_fake(int af, const char *src, void *dst) {
  if (af != BIONIC_AF_INET) {
    errno = BIONIC_EAFNOSUPPORT;
    return -1;
  }
  uint32_t addr;
  if (!parse_ipv4_addr(src, &addr))
    return 0;
  if (dst)
    memcpy(dst, &addr, sizeof(addr));
  return 1;
}

int poll_fake(void *fds, unsigned long nfds, int timeout) {
  (void)fds; (void)nfds;
  if (timeout > 0)
    svcSleepThread((s64)timeout * 1000000ll);
  return 0;
}

int select_fake(int nfds, void *rd, void *wr, void *ex, struct timeval *timeout) {
  (void)nfds; (void)rd; (void)wr; (void)ex;
  if (timeout)
    svcSleepThread((s64)timeout->tv_sec * 1000000000ll + (s64)timeout->tv_usec * 1000ll);
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr); the engine's
// console output goes to stdout/stderr alongside the tier0 spew
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total);
    buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return c;
  return fputc(c, f);
}

int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f)) {
    debugPrintf("stdio: %s", s);
    return 0;
  }
  return fputs(s, f);
}

int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL)
    return 0;
  return fflush(f);
}

int fclose_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fclose(f);
}

int ferror_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return ferror(f);
}

int feof_fake(FILE *f) {
  if (is_fake_file(f))
    return 1;
  return feof(f);
}

int fileno_fake(FILE *f) {
  if (is_fake_file(f))
    return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}

int fscanf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f))
    return -1; // EOF
  va_list va;
  va_start(va, fmt);
  int ret = vfscanf(f, fmt, va);
  va_end(va);
  return ret;
}

int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f))
    return -1;
  return fseek(f, off, whence);
}

int fgetc_fake(FILE *f) {
  if (is_fake_file(f))
    return -1; // EOF
  return fgetc(f);
}

char *fgets_fake(char *s, int n, FILE *f) {
  if (is_fake_file(f))
    return NULL;
  return fgets(s, n, f);
}

int ungetc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return -1;
  return ungetc(c, f);
}

void rewind_fake(FILE *f) {
  if (is_fake_file(f))
    return;
  rewind(f);
}

FILE *freopen_fake(const char *path, const char *mode, FILE *f) {
  if (is_fake_file(f))
    return f; // pretend the redirect worked
  return freopen(path, mode, f);
}

int setvbuf_fake(FILE *f, char *buf, int mode, size_t size) {
  if (is_fake_file(f))
    return 0;
  return setvbuf(f, buf, mode, size);
}

// ---------------------------------------------------------------------------
// fopen with /proc synthesis: tier0/engine/GameUI read /proc/cpuinfo and
// /proc/meminfo for CPU/core detection and memory sizing. Serve plausible
// Switch values from memory (fmemopen) and let other /proc and /sys paths
// fail cleanly.
// ---------------------------------------------------------------------------

static const char fake_cpuinfo[] =
  "processor\t: 0\n"
  "model name\t: ARMv8 Processor rev 1 (v8l)\n"
  "BogoMIPS\t: 38.40\n"
  "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
  "CPU implementer\t: 0x41\n"
  "CPU architecture: 8\n"
  "CPU variant\t: 0x1\n"
  "CPU part\t: 0xd07\n"
  "CPU revision\t: 1\n"
  "\n"
  "processor\t: 1\n"
  "model name\t: ARMv8 Processor rev 1 (v8l)\n"
  "BogoMIPS\t: 38.40\n"
  "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
  "CPU implementer\t: 0x41\n"
  "CPU architecture: 8\n"
  "CPU variant\t: 0x1\n"
  "CPU part\t: 0xd07\n"
  "CPU revision\t: 1\n"
  "\n"
  "processor\t: 2\n"
  "model name\t: ARMv8 Processor rev 1 (v8l)\n"
  "BogoMIPS\t: 38.40\n"
  "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
  "CPU implementer\t: 0x41\n"
  "CPU architecture: 8\n"
  "CPU variant\t: 0x1\n"
  "CPU part\t: 0xd07\n"
  "CPU revision\t: 1\n"
  "\n"
  "Hardware\t: Nintendo Switch\n";

static const char fake_meminfo[] =
  "MemTotal:        3276800 kB\n"
  "MemFree:         2097152 kB\n"
  "MemAvailable:    2621440 kB\n"
  "Buffers:               0 kB\n"
  "Cached:                0 kB\n"
  "SwapTotal:             0 kB\n"
  "SwapFree:              0 kB\n";

static const char fake_self_status[] =
  "Name:\thl2_linux\n"
  "Umask:\t0022\n"
  "State:\tR (running)\n"
  "Tgid:\t1\n"
  "Ngid:\t0\n"
  "Pid:\t1\n"
  "PPid:\t0\n"
  "TracerPid:\t0\n"
  "Uid:\t0\t0\t0\t0\n"
  "Gid:\t0\t0\t0\t0\n"
  "FDSize:\t64\n"
  "VmPeak:\t 3145728 kB\n"
  "VmSize:\t 3145728 kB\n"
  "VmRSS:\t  524288 kB\n"
  "Threads:\t8\n";

// stage the synthetic /proc contents as real files so the fd-level open()
// path can serve them; called from main once the install root is current
void proc_files_init(const char *install_root) {
  char dir[280];
  snprintf(dir, sizeof(dir), "%s/.proc", install_root);
  mkdir(dir, 0777);

  const struct { const char *name; const char *data; size_t len; char *out; } files[] = {
    { "meminfo", fake_meminfo, sizeof(fake_meminfo) - 1, proc_meminfo_path },
    { "cpuinfo", fake_cpuinfo, sizeof(fake_cpuinfo) - 1, proc_cpuinfo_path },
    { "self_status", fake_self_status, sizeof(fake_self_status) - 1, proc_status_path },
  };
  for (unsigned int i = 0; i < sizeof(files) / sizeof(*files); i++) {
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", dir, files[i].name);
    FILE *f = fopen(path, "wb");
    if (f) {
      fwrite(files[i].data, 1, files[i].len, f);
      fclose(f);
      strcpy(files[i].out, path);
    } else {
      debugPrintf("proc_files_init: could not stage %s\n", path);
    }
  }
}

FILE *fopen_fake(const char *path, const char *mode) {
  if (path && path[0] == '/' && (path[1] == 'p' || path[1] == 's' || path[1] == 'd')) {
    if (!strcmp(path, "/proc/cpuinfo"))
      return fmemopen((void *)fake_cpuinfo, sizeof(fake_cpuinfo) - 1, "r");
    if (!strcmp(path, "/proc/meminfo"))
      return fmemopen((void *)fake_meminfo, sizeof(fake_meminfo) - 1, "r");
    if (!strcmp(path, "/proc/self/status"))
      return fmemopen((void *)fake_self_status, sizeof(fake_self_status) - 1, "r");
    if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5) || !strncmp(path, "/dev/", 5))
      return NULL;
  }
  FILE *f = fopen(path, mode);
  return f;
}

// ---------------------------------------------------------------------------
// pthread TLS keys, multiplexed over a single real newlib key
//
// devkitA64 pthread keys are backed by libnx TLS slots and there are only 16
// of those system-wide. Every Source module's statically-linked gabi++
// runtime creates one key in its init array (26 modules), and tier0's
// CThreadLocal* objects allocate more at runtime -- the 8th module's init
// died with "GAbi++:Can't allocate C++ runtime pthread_key_t" on first
// hardware boot. bionic allows 128 keys, so emulate that: one real key
// holds a per-thread value array for up to 128 fake keys.
// ---------------------------------------------------------------------------

#include <pthread.h>

#define FAKE_KEYS_MAX 128

static Mutex key_mutex;
static struct {
  int used;
  void (*dtor)(void *);
} key_table[FAKE_KEYS_MAX];
static pthread_key_t master_key;
static int master_key_ready;

typedef struct {
  void *values[FAKE_KEYS_MAX];
} KeyValues;

static void master_key_dtor(void *p) {
  KeyValues *kv = p;
  // POSIX semantics: rerun while destructors set new values, bounded
  for (int iter = 0; iter < 4; iter++) {
    int again = 0;
    for (int i = 0; i < FAKE_KEYS_MAX; i++) {
      void *v = kv->values[i];
      if (key_table[i].used && key_table[i].dtor && v) {
        kv->values[i] = NULL;
        key_table[i].dtor(v);
        again = 1;
      }
    }
    if (!again)
      break;
  }
  free(kv);
}

int pthread_key_create_fake(unsigned *key, void (*dtor)(void *)) {
  mutexLock(&key_mutex);
  if (!master_key_ready) {
    if (pthread_key_create(&master_key, master_key_dtor) != 0) {
      mutexUnlock(&key_mutex);
      debugPrintf("pthread_key_create_fake: master key allocation failed\n");
      return EAGAIN;
    }
    master_key_ready = 1;
  }
  for (unsigned i = 0; i < FAKE_KEYS_MAX; i++) {
    if (!key_table[i].used) {
      key_table[i].used = 1;
      key_table[i].dtor = dtor;
      *key = i + 1; // 1-based so a zeroed pthread_key_t is invalid
      mutexUnlock(&key_mutex);
      return 0;
    }
  }
  mutexUnlock(&key_mutex);
  debugPrintf("pthread_key_create_fake: out of keys\n");
  return EAGAIN;
}

int pthread_key_delete_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX)
    return EINVAL;
  mutexLock(&key_mutex);
  key_table[key - 1].used = 0;
  key_table[key - 1].dtor = NULL;
  mutexUnlock(&key_mutex);
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX || !master_key_ready)
    return NULL;
  KeyValues *kv = pthread_getspecific(master_key);
  return kv ? kv->values[key - 1] : NULL;
}

int pthread_setspecific_fake(unsigned key, const void *value) {
  if (key == 0 || key > FAKE_KEYS_MAX || !master_key_ready)
    return EINVAL;
  KeyValues *kv = pthread_getspecific(master_key);
  if (!kv) {
    kv = calloc(1, sizeof(*kv));
    if (!kv)
      return ENOMEM;
    pthread_setspecific(master_key, kv);
  }
  kv->values[key - 1] = (void *)value;
  return 0;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks and semaphores via pointer indirection
// (bionic types are plain structs the game allocates; we stash a pointer
// to the real object in their first bytes, like the mutex fakes)
// ---------------------------------------------------------------------------

typedef struct {
  RwLock lock;
} FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  for (;;) {
    FakeRwLock *cur = *(FakeRwLock *volatile *)storage;
    if (cur)
      return cur;
    FakeRwLock *l = calloc(1, sizeof(*l));
    rwlockInit(&l->lock);
    if (__sync_bool_compare_and_swap(storage, NULL, l))
      return l;
    free(l); // lost the init race
  }
}

int pthread_rwlock_rdlock_fake(void **rw) {
  rwlockReadLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_wrlock_fake(void **rw) {
  rwlockWriteLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  // libnx needs to know which way it was locked
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock))
    rwlockWriteUnlock(&l->lock);
  else
    rwlockReadUnlock(&l->lock);
  return 0;
}

// bionic sem_t is 16 bytes -- exactly sizeof(libnx Semaphore), so the real
// semaphore is constructed in place inside the game's storage and value
// semantics (memcpy, destroy-without-free) match bionic
_Static_assert(sizeof(Semaphore) <= 16, "Semaphore must fit bionic sem_t");

int sem_init_fake(void *s, int pshared, unsigned int value) {
  (void)pshared;
  semaphoreInit((Semaphore *)s, value);
  return 0;
}

int sem_destroy_fake(void *s) {
  (void)s; // nothing to release
  return 0;
}

int sem_post_fake(void *s) {
  semaphoreSignal((Semaphore *)s);
  return 0;
}

int sem_wait_fake(void *s) {
  semaphoreWait((Semaphore *)s);
  return 0;
}

int sem_trywait_fake(void *s) {
  if (semaphoreTryWait((Semaphore *)s))
    return 0;
  errno = EAGAIN;
  return -1;
}

int sem_timedwait_fake(void *s, const struct timespec *abstime) {
  Semaphore *fs_sem = (Semaphore *)s;
  // the abstime may be against CLOCK_REALTIME or CLOCK_MONOTONIC (bionic
  // sem_timedwait_monotonic_np); compute a relative deadline that works
  // for whichever produced it
  struct timespec rt;
  clock_gettime(CLOCK_REALTIME, &rt);
  const int64_t now_rt = (int64_t)rt.tv_sec * 1000000000ll + rt.tv_nsec;
  const int64_t now_mono = (int64_t)armTicksToNs(armGetSystemTick());
  const int64_t abs_ns = (int64_t)abstime->tv_sec * 1000000000ll + abstime->tv_nsec;
  const int64_t day = 24ll * 3600 * 1000000000ll;
  int64_t rel = abs_ns - now_rt;
  if (rel < -1000000ll || rel >= day) {
    const int64_t rel_mono = abs_ns - now_mono;
    if (rel_mono >= -1000000ll && rel_mono < day)
      rel = rel_mono;
  }
  const int64_t deadline = (int64_t)armTicksToNs(armGetSystemTick()) + (rel > 0 ? rel : 0);
  for (;;) {
    if (semaphoreTryWait(fs_sem))
      return 0;
    if ((int64_t)armTicksToNs(armGetSystemTick()) >= deadline) {
      errno = 110; // bionic ETIMEDOUT; the binaries test against bionic values
      return -1;
    }
    svcSleepThread(1000000ll); // 1 ms
  }
}

int sem_getvalue_fake(void *s, int *val) {
  *val = (int)((Semaphore *)s)->count;
  return 0;
}

int pthread_attr_getstacksize_fake(const void *attr, size_t *size) {
  (void)attr;
  *size = 1024 * 1024;
  return 0;
}

int pthread_getschedparam_fake(unsigned long thread, int *policy, void *param) {
  (void)thread;
  if (policy)
    *policy = 0;
  if (param)
    memset(param, 0, 8);
  return 0;
}
