#define _POSIX_C_SOURCE 200809L
#include "archive_cache.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK_SIZE (1024u * 1024u)
#define CACHE_BLOCKS 64
#define CACHE_FILES 128
#define CACHE_STREAMS 256
#define CACHE_PATH 512

typedef struct {
  char path[CACHE_PATH];
  struct stat identity;
  uint64_t generation;
  int disabled;
} CachedFile;
typedef struct { FILE *file; unsigned identity; uint64_t generation; } CachedStream;
typedef struct {
  unsigned char *data;
  unsigned identity;
  uint64_t generation, used;
  off_t offset;
  int valid, busy;
} CachedBlock;

static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static CachedFile files[CACHE_FILES];
static CachedStream streams[CACHE_STREAMS];
static CachedBlock blocks[CACHE_BLOCKS];
static ArchiveCacheStats counters;
static unsigned file_count;
static int allocation_limited;
static uint64_t sequence;

static int numbered_archive(const char *path) {
  if (!path) return 0;
  size_t length = strlen(path);
  if (length < 8) return 0;
  const char *tail = path + length - 8;
  return tail[0] == '_' && tail[1] >= '0' && tail[1] <= '9' &&
    tail[2] >= '0' && tail[2] <= '9' && tail[3] >= '0' && tail[3] <= '9' &&
    tail[4] == '.' && (tail[5] | 32) == 'v' && (tail[6] | 32) == 'p' && (tail[7] | 32) == 'k';
}

// Switch paths can use sdmc:/..., /..., or a path relative to the game root.
// Normalize spelling without querying the SD card or relying on path hashes.
static int path_key(const char *path, char out[CACHE_PATH]) {
  char absolute[CACHE_PATH], cwd[CACHE_PATH];
  const char *colon = strchr(path, ':');
  int length;
  if (colon) {
    length = snprintf(absolute, sizeof(absolute), "%s", path);
  } else {
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    const char *device = strchr(cwd, ':');
    if (path[0] == '/' && device)
      length = snprintf(absolute, sizeof(absolute), "%.*s%s", (int)(device - cwd + 1), cwd, path);
    else if (path[0] == '/')
      length = snprintf(absolute, sizeof(absolute), "%s", path);
    else
      length = snprintf(absolute, sizeof(absolute), "%s/%s", cwd, path);
  }
  if (length < 0 || length >= (int)sizeof(absolute)) return 0;
  for (char *p = absolute; *p; p++) {
    if (*p == '\\') *p = '/';
    if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
  }
  char *root = strchr(absolute, ':');
  size_t used = root ? (size_t)(root - absolute + 1) : 0;
  if (absolute[used] != '/') return 0;
  memcpy(out, absolute, used);
  out[used++] = '/';
  size_t component[256], depth = 0;
  const char *p = absolute + used;
  while (*p) {
    while (*p == '/') p++;
    const char *start = p;
    while (*p && *p != '/') p++;
    const size_t n = (size_t)(p - start);
    if (!n || (n == 1 && start[0] == '.')) continue;
    if (n == 2 && start[0] == '.' && start[1] == '.') {
      if (depth) used = component[--depth];
      continue;
    }
    if (depth == 256 || used + n + 1 >= CACHE_PATH) return 0;
    component[depth++] = used;
    if (out[used - 1] != '/') out[used++] = '/';
    memcpy(out + used, start, n);
    used += n;
  }
  out[used] = 0;
  return 1;
}

static unsigned find_file(const char *path) {
  unsigned index = 0;
  while (index < file_count && strcmp(files[index].path, path)) index++;
  return index;
}

static void invalidate_file(unsigned identity) {
  files[identity].generation++;
  for (unsigned i = 0; i < CACHE_BLOCKS; i++)
    if (blocks[i].identity == identity) blocks[i].valid = 0;
}

static int same_file(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
    a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime &&
    a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

void archive_cache_open(FILE *file, const char *path, const char *mode, int unbuffered) {
  if (!file || !unbuffered || !mode || mode[0] != 'r' || strchr(mode, '+') || !numbered_archive(path)) return;
  const int saved_errno = errno;
  char key[CACHE_PATH];
  struct stat identity = {0};
  if (!path_key(path, key) || fstat(fileno(file), &identity) || !S_ISREG(identity.st_mode)) {
    errno = saved_errno;
    return;
  }
  pthread_mutex_lock(&cache_mutex);
  unsigned index = find_file(key);
  if (index < CACHE_FILES) {
    if (index == file_count) {
      strcpy(files[index].path, key);
      files[index].identity = identity;
      files[index].generation = 1;
      file_count++;
    } else if (!same_file(&identity, &files[index].identity)) {
      invalidate_file(index);
      files[index].identity = identity;
    }
    for (unsigned i = 0; i < CACHE_STREAMS; i++) {
      if (!streams[i].file || streams[i].file == file) {
        streams[i] = (CachedStream){file, index, files[index].generation};
        break;
      }
    }
  }
  pthread_mutex_unlock(&cache_mutex);
  errno = saved_errno;
}

void archive_cache_forget(FILE *file) {
  const int saved_errno = errno;
  pthread_mutex_lock(&cache_mutex);
  for (unsigned i = 0; i < CACHE_STREAMS; i++)
    if (streams[i].file == file) streams[i].file = NULL;
  pthread_mutex_unlock(&cache_mutex);
  errno = saved_errno;
}

void archive_cache_invalidate(const char *path) {
  if (!numbered_archive(path)) return;
  const int saved_errno = errno;
  char key[CACHE_PATH];
  if (path_key(path, key)) {
    pthread_mutex_lock(&cache_mutex);
    unsigned index = find_file(key);
    if (index < CACHE_FILES) {
      if (index == file_count) {
        strcpy(files[index].path, key);
        file_count++;
      }
      invalidate_file(index);
      // A writable handle may remain open. Use normal I/O for this path for
      // the rest of the process rather than caching potentially changing data.
      files[index].disabled = 1;
    }
    pthread_mutex_unlock(&cache_mutex);
  }
  errno = saved_errno;
}

size_t archive_cache_read(void *data, size_t size, size_t count, FILE *file) {
  if (!size || count > SIZE_MAX / size || size * count != BLOCK_SIZE)
    return fread(data, size, count, file);
  const int initial_errno = errno;
  CachedStream stream = {0};
  pthread_mutex_lock(&cache_mutex);
  for (unsigned i = 0; i < CACHE_STREAMS; i++) {
    if (streams[i].file == file) { stream = streams[i]; break; }
  }
  pthread_mutex_unlock(&cache_mutex);
  if (!stream.file) { errno = initial_errno; return fread(data, size, count, file); }

  flockfile(file);
  off_t offset = ftello(file);
  int eligible = offset >= 0 && (uint64_t)offset <= INT64_MAX - BLOCK_SIZE &&
    !(offset % BLOCK_SIZE) && !ferror(file) && !feof(file);
#ifdef __SWITCH__
  // Preserve a caller's pending pushback rather than replacing it with cached bytes.
  if (file->_r || file->_ub._base) eligible = 0;
#endif
  pthread_mutex_lock(&cache_mutex);
  CachedFile *identity = &files[stream.identity];
  if (!eligible || identity->disabled || identity->generation != stream.generation) {
    counters.bypasses++;
    pthread_mutex_unlock(&cache_mutex);
    errno = initial_errno;
    size_t result = fread(data, size, count, file);
    int saved_errno = errno;
    funlockfile(file);
    errno = saved_errno;
    return result;
  }

  counters.requests++;
  int existing_fill = 0;
  for (unsigned i = 0; i < CACHE_BLOCKS; i++) {
    CachedBlock *block = &blocks[i];
    if (block->identity != stream.identity || block->generation != stream.generation || block->offset != offset) continue;
    if (block->busy) existing_fill = 1;
    if (!block->valid || block->busy) continue;
    if (!fseeko(file, offset + BLOCK_SIZE, SEEK_SET)) {
      memcpy(data, block->data, BLOCK_SIZE);
      block->used = ++sequence;
      counters.hits++;
      pthread_mutex_unlock(&cache_mutex);
      funlockfile(file);
      errno = initial_errno;
      return count;
    }
    block->valid = 0;
    break;
  }

  counters.misses++;
  CachedBlock *fill = NULL;
  if (!existing_fill) {
    for (unsigned i = 0; i < CACHE_BLOCKS; i++) {
      CachedBlock *b = &blocks[i];
      if (b->busy) continue;
      if (!b->data && allocation_limited) continue;
      if (!fill || (!b->valid && fill->valid) || (b->valid == fill->valid && b->used < fill->used)) fill = b;
    }
    if (fill && !fill->data) {
      fill->data = malloc(BLOCK_SIZE);
      if (fill->data) counters.allocated_blocks++;
      else {
        counters.allocation_failures++;
        allocation_limited = 1;
        fill = NULL;
        for (unsigned i = 0; i < CACHE_BLOCKS; i++)
          if (blocks[i].data && !blocks[i].busy && (!fill || blocks[i].used < fill->used)) fill = &blocks[i];
      }
    }
    if (fill) {
      if (fill->valid) counters.evictions++;
      *fill = (CachedBlock){fill->data, stream.identity, stream.generation, ++sequence, offset, 0, 1};
    }
  }
  pthread_mutex_unlock(&cache_mutex);
  errno = initial_errno;
  size_t result = fread(data, size, count, file);
  const int saved_errno = errno;
  int complete = result == count && !ferror(file) && !feof(file);
  pthread_mutex_lock(&cache_mutex);
  if (fill) {
    if (complete && !identity->disabled && identity->generation == stream.generation) {
      memcpy(fill->data, data, BLOCK_SIZE);
      fill->valid = 1;
    }
    fill->busy = 0;
  }
  pthread_mutex_unlock(&cache_mutex);
  funlockfile(file);
  errno = saved_errno;
  return result;
}

void archive_cache_stats(ArchiveCacheStats *stats) {
  const int saved_errno = errno;
  pthread_mutex_lock(&cache_mutex);
  *stats = counters;
  pthread_mutex_unlock(&cache_mutex);
  errno = saved_errno;
}
