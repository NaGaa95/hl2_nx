/* dl_emu.c -- dlopen/dlsym/dlclose/dlerror/dladdr over the so_util loader
 *
 * The engine loads modules through dlopen and resolves CreateInterface and the
 * GL entry points through dlsym. All 26 modules are preloaded by main.c; dlopen
 * hands out their so_module pointers as handles. Three pseudo-modules cover the
 * host libraries:
 *
 *  - libSDL2.so / libSDL2-2.0.so  -> the static import table (switch-sdl2)
 *  - libEGL.so                    -> mesa EGL (static table + eglGetProcAddress)
 *  - libGLESv3.so / libGLESv2.so  -> eglGetProcAddress (mesa exposes all gl*
 *                                    via EGL_KHR_get_all_proc_addresses)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>

#include "so_util.h"
#include "dl_emu.h"
#include "util.h"
#include "imports.h"

#define HANDLE_HOST ((void *)0x484f5354) // "HOST": import-table-backed pseudo
#define HANDLE_EGL  ((void *)0x45474c00) // "EGL"
#define HANDLE_GLES ((void *)0x474c4553) // "GLES"

static char dl_err[256];
static int dl_err_set = 0;

static void set_dl_error(const char *fmt, const char *arg) {
  snprintf(dl_err, sizeof(dl_err), fmt, arg);
  dl_err_set = 1;
}

// minimal static EGL table: what the launcher dlsyms by name from the
// libEGL.so handle. everything else goes through eglGetProcAddress.
static const DynLibFunction egl_funcs[] = {
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress_fake },
  { "eglInitialize", (uintptr_t)&eglInitialize },
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglQueryString", (uintptr_t)&eglQueryString },
  { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglCreateContext", (uintptr_t)&eglCreateContext },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
  { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers },
  { "eglSwapInterval", (uintptr_t)&eglSwapInterval },
  { "eglBindAPI", (uintptr_t)&eglBindAPI },
  { "eglTerminate", (uintptr_t)&eglTerminate },
  { "eglWaitClient", (uintptr_t)&eglWaitClient },
  { "eglWaitGL", (uintptr_t)&eglWaitGL },
  { "eglWaitNative", (uintptr_t)&eglWaitNative },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglQueryContext", (uintptr_t)&eglQueryContext },
  { "eglQuerySurface", (uintptr_t)&eglQuerySurface },
  { "eglReleaseThread", (uintptr_t)&eglReleaseThread },
};

// the names Source/SDL may ask for that map onto host pseudo-modules
typedef struct {
  const char *name;
  void *handle;
} PseudoMap;

static const PseudoMap pseudo_map[] = {
  { "libSDL2.so", HANDLE_HOST },
  { "libSDL2-2.0.so", HANDLE_HOST },
  { "libdl.so", HANDLE_HOST },
  { "liblog.so", HANDLE_HOST },
  { "libc.so", HANDLE_HOST },
  { "libm.so", HANDLE_HOST },
  { "libz.so", HANDLE_HOST },
  { "libEGL.so", HANDLE_EGL },
  { "libGLESv3.so", HANDLE_GLES },
  { "libGLESv2.so", HANDLE_GLES },
  { "libGLESv1_CM.so", HANDLE_GLES },
};

void *dlopen_fake(const char *filename, int flags) {
  (void)flags;

  if (!filename) // "the main program"
    return HANDLE_HOST;

  // Source builds absolute paths from APP_LIB_PATH; match by basename
  const char *base = strrchr(filename, '/');
  base = base ? base + 1 : filename;

  // normalized copy with a "lib" prefix ("engine.so" -> "libengine.so")
  char norm[128];
  if (strncmp(base, "lib", 3) != 0)
    snprintf(norm, sizeof(norm), "lib%s", base);
  else
    snprintf(norm, sizeof(norm), "%s", base);

  for (unsigned int i = 0; i < sizeof(pseudo_map) / sizeof(*pseudo_map); i++) {
    if (!strcmp(norm, pseudo_map[i].name)) {
      debugPrintf("dlopen(%s) -> pseudo %p\n", filename, pseudo_map[i].handle);
      return pseudo_map[i].handle;
    }
  }

  so_module *mod = so_find_module(norm);
  if (mod) {
    debugPrintf("dlopen(%s) -> %s\n", filename, mod->name);
    return mod;
  }

  debugPrintf("dlopen(%s): NOT FOUND\n", filename);
  set_dl_error("dlopen failed: %s not preloaded", base);
  return NULL;
}

void *dlsym_fake(void *handle, const char *symbol) {
  void *addr = NULL;

  if (!symbol) {
    set_dl_error("dlsym: null symbol%s", "");
    return NULL;
  }

  if (handle == HANDLE_HOST || handle == NULL) {
    // import table first (host SDL2/libc), then every loaded module
    DynLibFunction *f = so_find_import(dynlib_functions, dynlib_numfunctions, symbol);
    if (f)
      addr = (void *)f->func;
    if (!addr)
      addr = (void *)so_lookup_export_all(symbol);
  } else if (handle == HANDLE_EGL) {
    for (unsigned int i = 0; i < sizeof(egl_funcs) / sizeof(*egl_funcs); i++) {
      if (!strcmp(egl_funcs[i].symbol, symbol)) {
        addr = (void *)egl_funcs[i].func;
        break;
      }
    }
    if (!addr)
      addr = (void *)eglGetProcAddress_fake(symbol);
  } else if (handle == HANDLE_GLES) {
    // mesa implements EGL_KHR_get_all_proc_addresses: core gl* resolves too
    addr = (void *)eglGetProcAddress_fake(symbol);
  } else if (so_is_module(handle)) {
    addr = (void *)so_lookup_export((so_module *)handle, symbol);
  } else {
    debugPrintf("dlsym(%p, %s): bad handle\n", handle, symbol);
    set_dl_error("dlsym: invalid handle%s", "");
    return NULL;
  }

  if (!addr) {
    debugPrintf("dlsym(%p, %s): NOT FOUND\n", handle, symbol);
    set_dl_error("dlsym: undefined symbol %s", symbol);
  }

  return addr;
}

int dlclose_fake(void *handle) {
  (void)handle; // modules stay loaded for the lifetime of the process
  return 0;
}

char *dlerror_fake(void) {
  if (!dl_err_set)
    return NULL;
  dl_err_set = 0;
  return dl_err;
}

int dladdr_fake(const void *addr, DlInfoFake *info) {
  for (so_module *mod = so_first(); mod; mod = mod->next) {
    const uintptr_t base = (uintptr_t)mod->load_virtbase;
    if ((uintptr_t)addr < base || (uintptr_t)addr >= base + mod->load_size)
      continue;
    info->dli_fname = mod->name;
    info->dli_fbase = mod->load_virtbase;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    // nearest exported symbol at or below addr, for tier0 stack traces
    uintptr_t best = 0;
    int best_idx = -1;
    for (int i = 0; i < mod->num_syms; i++) {
      if (mod->syms[i].st_shndx == SHN_UNDEF)
        continue;
      const uintptr_t s = base + mod->syms[i].st_value;
      if (s <= (uintptr_t)addr && s > best) {
        best = s;
        best_idx = i;
      }
    }
    if (best_idx >= 0) {
      info->dli_sname = mod->dynstrtab + mod->syms[best_idx].st_name;
      info->dli_saddr = (void *)best;
    }
    return 1;
  }
  return 0;
}
