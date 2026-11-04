#define _POSIX_C_SOURCE 200809L
#include "archive_cache.h"
#include "archive_io.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLOCK 1048576u
static unsigned char output[BLOCK], scratch[BLOCK];
static unsigned disk_reads, allocations;
static int budget = -1, seek_failure;
static pthread_mutex_t pause_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static FILE *paused_file;
static int waiting, resume_read;

void *__real_malloc(size_t);
size_t __real_fread(void *, size_t, size_t, FILE *);
int __real_fseeko(FILE *, off_t, int);

void *__wrap_malloc(size_t size) {
  if (size == BLOCK) {
    if (!budget) { errno = ENOMEM; return NULL; }
    if (budget > 0) budget--;
    allocations++;
  }
  return __real_malloc(size);
}

size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file) {
  __atomic_fetch_add(&disk_reads, 1, __ATOMIC_RELAXED);
  if (size == SIZE_MAX) return 7; // Verify overflow arguments are forwarded unchanged.
  pthread_mutex_lock(&pause_mutex);
  if (file == paused_file) {
    waiting = 1;
    pthread_cond_broadcast(&pause_cond);
    while (!resume_read) pthread_cond_wait(&pause_cond, &pause_mutex);
    paused_file = NULL;
  }
  pthread_mutex_unlock(&pause_mutex);
  return __real_fread(data, size, count, file);
}

int __wrap_fseeko(FILE *file, off_t offset, int whence) {
  if (seek_failure) { seek_failure = 0; errno = ESPIPE; return -1; }
  return __real_fseeko(file, offset, whence);
}

static unsigned char pattern(unsigned block, unsigned byte, unsigned salt) {
  return (unsigned char)(block * 37u + byte * 13u + salt);
}

static void fixture(const char *path, unsigned blocks, unsigned salt) {
  FILE *file = fopen(path, "wb"); assert(file);
  for (unsigned b = 0; b < blocks; b++) {
    for (unsigned i = 0; i < BLOCK; i++) scratch[i] = pattern(b, i, salt);
    assert(fwrite(scratch, 1, BLOCK, file) == BLOCK);
  }
  assert(fwrite("seventeen-tail-xx", 1, 17, file) == 17);
  assert(!fclose(file));
}

static FILE *open_archive(const char *path) {
  FILE *file = fopen(path, "rb"); assert(file);
  int configured = archive_stdio_setup(file, path, "rb"); assert(configured);
  errno = EACCES;
  archive_cache_open(file, path, "rb", configured);
  assert(errno == EACCES);
  return file;
}

static void close_archive(FILE *file) { archive_cache_forget(file); assert(!fclose(file)); }

static void check_bytes(const unsigned char *data, unsigned block, unsigned salt) {
  for (unsigned i = 0; i < BLOCK; i++) assert(data[i] == pattern(block, i, salt));
}

static void get(FILE *file, unsigned block, unsigned salt, unsigned char *data) {
  assert(!fseeko(file, (off_t)block * BLOCK, SEEK_SET));
  errno = ENOSPC;
  assert(archive_cache_read(data, 1, BLOCK, file) == BLOCK);
  assert(errno == ENOSPC);
  assert(ftello(file) == (off_t)(block + 1) * BLOCK);
  assert(!ferror(file) && !feof(file));
  check_bytes(data, block, salt);
}

static void *delayed_reader(void *arg) { get(arg, 1, 9, scratch); return NULL; }

int main(int argc, char **argv) {
  assert(argc == 2);
  alarm(30);
  char directory[] = "/tmp/hl2-archive-cache-XXXXXX";
  assert(mkdtemp(directory));
  char a_path[512], b_path[512], alias[512];
  assert(snprintf(a_path, sizeof(a_path), "%s/a_000.vpk", directory) > 0);
  assert(snprintf(b_path, sizeof(b_path), "%s/b_000.vpk", directory) > 0);
  assert(snprintf(alias, sizeof(alias), "%s//./a_000.vpk", directory) > 0);
  fixture(a_path, 70, 3); fixture(b_path, 3, 9);
  if (!strcmp(argv[1], "oom")) budget = 0;
  if (!strcmp(argv[1], "limited")) budget = 2;
  FILE *a = open_archive(a_path);
  get(a, 0, 3, output);
  unsigned before = disk_reads;
  get(a, 0, 3, output);
  ArchiveCacheStats stats;
  archive_cache_stats(&stats);
  if (!strcmp(argv[1], "oom")) {
    assert(disk_reads == before + 1 && stats.allocated_blocks == 0 && stats.allocation_failures == 1);
    assert(stats.hits == 0 && stats.misses == 2);
    puts("PASS allocation failure preserves normal archive reads and errno");
    goto done;
  }
  assert(disk_reads == before && stats.hits == 1);
  get(a, 1, 3, output); get(a, 2, 3, output);
  if (!strcmp(argv[1], "limited")) {
    archive_cache_stats(&stats);
    assert(stats.allocated_blocks == 2 && stats.allocation_failures == 1);
    before = disk_reads; get(a, 2, 3, output); assert(disk_reads == before);
    puts("PASS partial allocation keeps a bounded smaller cache using LRU reuse");
    goto done;
  }

  // Cached fread returns elements, preserves errno and advances the real stream.
  assert(!fseeko(a, 0, SEEK_SET));
  before = disk_reads; errno = EDOM;
  assert(archive_cache_read(output, 4, BLOCK / 4, a) == BLOCK / 4);
  assert(disk_reads == before && errno == EDOM && ftello(a) == BLOCK);
  check_bytes(output, 0, 3);
  FILE *b = open_archive(b_path); get(b, 0, 9, output); close_archive(b);
  for (unsigned i = 0; i < 64; i++) get(a, i, 3, output);
  archive_cache_stats(&stats); assert(stats.allocated_blocks == 64 && allocations == 64);
  get(a, 0, 3, output); // promote block 0, making block 1 oldest
  get(a, 64, 3, output);
  before = disk_reads; get(a, 0, 3, output); assert(disk_reads == before);
  get(a, 1, 3, output); assert(disk_reads == before + 1);
  close_archive(a); a = open_archive(alias);
  before = disk_reads; get(a, 0, 3, output); assert(disk_reads == before);
  puts("PASS exact bytes, element counts, positions, file separation, normalized reopen and 64-block LRU");

  // Partial and zero-length reads retain stdio behavior; incomplete blocks are not cached.
  assert(!fseeko(a, 70LL * BLOCK, SEEK_SET));
  assert(archive_cache_read(output, 1, BLOCK, a) == 17 && feof(a));
  before = disk_reads;
  assert(!fseeko(a, 70LL * BLOCK, SEEK_SET));
  assert(archive_cache_read(output, 1, BLOCK, a) == 17 && disk_reads == before + 1);
  assert(!fseeko(a, 7, SEEK_SET));
  assert(archive_cache_read(output, 1, 19, a) == 19);
  assert(output[0] == pattern(0, 7, 3));
  off_t position = ftello(a);
  assert(archive_cache_read(output, 1, 0, a) == 0 && ftello(a) == position);
  assert(archive_cache_read(output, SIZE_MAX, 2, a) == 7);
  assert(!fseeko(a, 0, SEEK_SET));
  seek_failure = 1;
  assert(archive_cache_read(output, 1, BLOCK, a) == BLOCK);
  check_bytes(output, 0, 3);
  puts("PASS partial/unaligned/zero reads, overflow forwarding, EOF and failed cache-hit seek fallback");

  // Non-archive streams remain ordinary stdio.
  FILE *plain = fopen(a_path, "rb"); assert(plain);
  assert(!archive_stdio_setup(plain, "config.cfg", "rb"));
  archive_cache_open(plain, "config.cfg", "rb", 0);
  before = disk_reads;
  assert(archive_cache_read(output, 1, BLOCK, plain) == BLOCK && disk_reads == before + 1);
  fclose(plain);

  // SD reads must run outside the global cache mutex. Duplicate in-flight misses
  // can read normally while another stream uses an existing cached block.
  b = open_archive(b_path); FILE *second = open_archive(b_path);
  get(b, 0, 9, output);
  pthread_mutex_lock(&pause_mutex); paused_file = b; waiting = resume_read = 0; pthread_mutex_unlock(&pause_mutex);
  pthread_t thread; assert(!pthread_create(&thread, NULL, delayed_reader, b));
  pthread_mutex_lock(&pause_mutex);
  while (!waiting) pthread_cond_wait(&pause_cond, &pause_mutex);
  pthread_mutex_unlock(&pause_mutex);
  before = disk_reads; get(second, 0, 9, output); assert(disk_reads == before);
  get(second, 1, 9, output); // same block as the pending miss; no partial cache entry may escape
  archive_cache_invalidate(b_path); // invalidation during a fill must prevent its publication
  pthread_mutex_lock(&pause_mutex); resume_read = 1; pthread_cond_broadcast(&pause_cond); pthread_mutex_unlock(&pause_mutex);
  pthread_join(thread, NULL);
  before = disk_reads; get(second, 1, 9, output); assert(disk_reads == before + 1);
  close_archive(b); close_archive(second);
  puts("PASS parallel cache hits, duplicate in-flight misses and invalidation during a fill");

  close_archive(a);
  fixture(a_path, 2, 41); // same path, changed identity/size; old cached data must be rejected
  a = open_archive(a_path); get(a, 0, 41, output);
  before = disk_reads; get(a, 0, 41, output); assert(disk_reads == before);
  close_archive(a); fixture(a_path, 2, 51); // Same size, new timestamps.
  a = open_archive(a_path); get(a, 0, 51, output);
  archive_cache_invalidate(alias);
  FILE *writer = fopen(a_path, "r+b"); assert(writer);
  unsigned char changed = 233;
  assert(fwrite(&changed, 1, 1, writer) == 1 && !fclose(writer));
  assert(!fseeko(a, 0, SEEK_SET)); before = disk_reads;
  assert(archive_cache_read(output, 1, BLOCK, a) == BLOCK && output[0] == changed && disk_reads == before + 1);
  puts("PASS changed-size/same-size archive replacement and writable-path invalidation");
done:
  archive_cache_stats(&stats);
  assert(stats.allocated_blocks <= 64);
  printf("cache requests=%llu hits=%llu misses=%llu evictions=%llu allocated_mib=%u allocation_failures=%llu\n",
    (unsigned long long)stats.requests, (unsigned long long)stats.hits, (unsigned long long)stats.misses,
    (unsigned long long)stats.evictions, stats.allocated_blocks, (unsigned long long)stats.allocation_failures);
  close_archive(a);
  assert(!unlink(a_path) && !unlink(b_path) && !rmdir(directory));
  return 0;
}
