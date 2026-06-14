/* so_util.h -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define SO_MAX_SEGMENTS 8

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;
  char name[64];

  // entire LOAD zone
  void *load_base, *load_virtbase;
  size_t load_size;
  void *load_memrv; // VirtmemReservation *

  // copy of the unmodified program headers (link-time vaddrs),
  // needed for dl_iterate_phdr / exception unwinding
  Elf64_Phdr phdr[SO_MAX_SEGMENTS * 2];
  int phnum;

  // temporary file image
  void *so_base;
  size_t so_size;

  Elf64_Ehdr *elf_hdr;
  Elf64_Phdr *prog_hdr;
  Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms;
  int num_syms;
  char *shstrtab;
  char *dynstrtab;

  // .init_array location (runtime address), captured at load time so
  // initializers can run after the temp file image is freed
  void (**init_array)(void);
  int num_init;
} so_module;

void hook_arm64(uintptr_t addr, uintptr_t dst);

void so_flush_caches(so_module *mod);
void so_free_temp(so_module *mod);
int so_load(so_module *mod, const char *filename, void *base, size_t max_size);
int so_relocate(so_module *mod);
int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
void so_execute_init_array(so_module *mod);
// pre-finalize only: address inside the RW mirror, for hooking/patching
uintptr_t so_find_addr(so_module *mod, const char *symbol);
uintptr_t so_find_addr_rx(so_module *mod, const char *symbol);
// returns 0 instead of aborting when the symbol is missing
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol);
// exported (defined, non-local) symbols only; the dlsym() backend
uintptr_t so_lookup_export(so_module *mod, const char *name);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name);
void so_finalize(so_module *mod);
int so_unload(so_module *mod);

// loaded module list access for the dl* emulation
so_module *so_first(void);
so_module *so_find_module(const char *basename);
int so_is_module(const void *handle);
// search every loaded module in load order; for dlsym(RTLD_DEFAULT)
uintptr_t so_lookup_export_all(const char *name);

// dl_iterate_phdr() replacement operating on all loaded modules
int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

#endif
