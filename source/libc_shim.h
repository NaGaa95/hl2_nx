/* libc_shim.h -- bionic-compatible libc wrappers
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>

// fake bionic __sF (stdin/stdout/stderr) and wrappers that absorb it
extern uint8_t fake_sF[3][0x100];

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
int fputs_fake(const char *s, FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int ferror_fake(FILE *f);
int feof_fake(FILE *f);
int fileno_fake(FILE *f);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);
int fscanf_fake(FILE *f, const char *fmt, ...);
int fseek_fake(FILE *f, long off, int whence);
int fgetc_fake(FILE *f);
char *fgets_fake(char *s, int n, FILE *f);
int ungetc_fake(int c, FILE *f);
FILE *fopen_fake(const char *path, const char *mode);
FILE *freopen_fake(const char *path, const char *mode, FILE *f);
void rewind_fake(FILE *f);
int setvbuf_fake(FILE *f, char *buf, int mode, size_t size);

// fortify
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen);
char *__strchr_chk_fake(const char *s, int c, size_t slen);
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen);
size_t __strlen_chk_fake(const char *s, size_t slen);
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
void __FD_SET_chk_fake(int fd, void *set, size_t set_size);

// misc bionic
int __system_property_get_fake(const char *name, char *value);
unsigned long getauxval_fake(unsigned long type);
int gettid_fake(void);
void android_set_abort_message_fake(const char *msg);
int __register_atfork_fake(void);
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso);
void __assert2_fake(const char *file, int line, const char *func, const char *expr);

// fd/file
int open_fake(const char *path, int flags, ...);
int access_fake(const char *path, int mode);
char *getcwd_fake(char *buf, size_t size);
int fcntl_fake(int fd, int cmd, ...);
int ioctl_fake(int fd, unsigned long req, ...);

// bionic struct stat conversion
struct bionic_stat; // opaque here
int stat_fake(const char *path, void *st);
int fstat_fake(int fd, void *st);
int lstat_fake(const char *path, void *st);
int statfs64_fake(const char *path, void *buf);

// dirent
void *readdir_fake(void *dirp);
int scandir_fake(const char *dir, void ***namelist,
                 int (*filter)(const void *),
                 int (*compar)(const void **, const void **));
int alphasort_fake(const void **a, const void **b);

// time
int clock_gettime_fake(int clk, struct timespec *ts);
time_t timegm_fake(struct tm *tm);

// memory
int posix_memalign_fake(void **out, size_t align, size_t size);
void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, long long off);
int munmap_fake(void *addr, size_t len);
void *mallinfo_fake(void *out);

// strings/util
char *basename_fake(const char *path);
void *memrchr_fake(const void *s, int c, size_t n);
int fnmatch_fake(const char *pattern, const char *string, int flags);
char *realpath_fake(const char *path, char *resolved);
int strerror_r_fake(int err, char *buf, size_t len);
char *tmpnam_fake(char *buf);

// math helpers
void sincos_fake(double x, double *s, double *c);
void sincosf_fake(float x, float *s, float *c);
long double fmodl_fake(long double a, long double b);
long double scalbnl_fake(long double x, int n);

// process/user stubs
int getpid_fake(void);
int getuid_fake(void);
void *getpwuid_fake(int uid);
int gethostname_fake(char *name, size_t len);
int getrusage_fake(int who, void *usage);
int system_fake(const char *cmd);
FILE *popen_fake(const char *cmd, const char *mode);
int pclose_fake(FILE *f);

// scheduling
int sched_yield_fake(void);

// null-socket emulation: idle UDP sockets that bind/connect/resolve so the
// SP loopback server<->client handshake completes; never carry real data
int socket_fake(int domain, int type, int protocol);
int sock_ok_fake(int fd);
int sock_bind_fake(int fd, const void *addr, int addrlen);
int sock_connect_fake(int fd, const void *addr, int addrlen);
int sock_listen_fake(int fd, int backlog);
int sock_setsockopt_fake(int fd, int level, int optname, const void *optval, int optlen);
long sock_send_fake(int fd, const void *buf, size_t len);
long sock_sendto_fake(int fd, const void *buf, size_t len, int flags, const void *to, int tolen);
long sock_recv_fake(int fd, void *buf, size_t len, int flags);
long sock_recvfrom_fake(int fd, void *buf, size_t len, int flags, void *from, int *fromlen);
int sock_getsockopt_fake(int fd, int level, int optname, void *optval, int *optlen);
int sock_getsockname_fake(int fd, void *addr, int *addrlen);
int sock_getpeername_fake(int fd, void *addr, int *addrlen);
int sock_accept_fake(int fd, void *addr, int *addrlen);
int close_fake(int fd);
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res);
void freeaddrinfo_fake(void *res);
void *gethostbyname_fake(const char *name);
unsigned int inet_addr_fake(const char *cp);
const char *inet_ntop_fake(int af, const void *src, char *dst, unsigned int size);
int inet_pton_fake(int af, const char *src, void *dst);
int poll_fake(void *fds, unsigned long nfds, int timeout);
int select_fake(int nfds, void *rd, void *wr, void *ex, struct timeval *timeout);

// pthread TLS keys multiplexed over one real newlib key (libnx has only 16
// TLS slots; the game needs bionic's 128)
int pthread_key_create_fake(unsigned *key, void (*dtor)(void *));
int pthread_key_delete_fake(unsigned key);
void *pthread_getspecific_fake(unsigned key);
int pthread_setspecific_fake(unsigned key, const void *value);

// pthread odds and ends with bionic-incompatible types
int pthread_rwlock_rdlock_fake(void **rw);
int pthread_rwlock_wrlock_fake(void **rw);
int pthread_rwlock_unlock_fake(void **rw);
int sem_init_fake(void *s, int pshared, unsigned int value);
int sem_destroy_fake(void *s);
int sem_post_fake(void *s);
int sem_wait_fake(void *s);
int sem_trywait_fake(void *s);
int sem_timedwait_fake(void *s, const struct timespec *abstime);
int sem_getvalue_fake(void *s, int *val);
int pthread_attr_getstacksize_fake(const void *attr, size_t *size);
int pthread_getschedparam_fake(unsigned long thread, int *policy, void *param);

// per-thread fake bionic TLS (TPIDR_EL0): stack-guard slot etc.
void fake_tls_install(void);

// stage synthetic /proc files (meminfo, cpuinfo, self/status) so the engine's
// raw open() reads work; the engine Sys_Error()s without them
void proc_files_init(const char *install_root);

#endif
