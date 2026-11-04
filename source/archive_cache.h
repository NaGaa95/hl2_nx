#ifndef HL2_ARCHIVE_CACHE_H
#define HL2_ARCHIVE_CACHE_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
  uint64_t requests, hits, misses, bypasses, evictions, allocation_failures;
  unsigned allocated_blocks;
} ArchiveCacheStats;

void archive_cache_open(FILE *file, const char *path, const char *mode, int unbuffered);
void archive_cache_forget(FILE *file);
void archive_cache_invalidate(const char *path);
size_t archive_cache_read(void *data, size_t size, size_t count, FILE *file);
void archive_cache_stats(ArchiveCacheStats *stats);

#endif
