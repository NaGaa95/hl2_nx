/* main.c -- Half-Life 2 Android (nillerusr Source port, v1.16.29) loader.
 *
 * Preloads the 25 Source ARM64 modules, mirrors the Android env/command-line
 * setup, and calls LauncherMainAndroid (which runs the whole game).
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen (loader architecture)
 * Distributed under the MIT license. See LICENSE for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "libc_shim.h"
#include "jni_fake.h"
#include "picker.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

// dependency order: imports resolve once all are loaded, but init_array runs
// in this order, so dependencies must come first
static const char *const module_names[] = {
  "libtier0.so",
  "libvstdlib.so",
  "libsteam_api.so",
  "libtogl.so",
  "libfilesystem_stdio.so",
  "libinputsystem.so",
  "libdatacache.so",
  "libscenefilecache.so",
  "libsoundemittersystem.so",
  "libvideo_services.so",
  "libvaudio_minimp3.so",
  "libvaudio_opus.so",
  "libvphysics.so",
  "libstudiorender.so",
  "libmaterialsystem.so",
  "libshaderapidx9.so",
  "libstdshader_dx9.so",
  "libvgui2.so",
  "libvguimatsurface.so",
  "libengine.so",
  "libGameUI.so",
  "libServerBrowser.so",
  "libclient.so",
  "libserver.so",
  "liblauncher.so",
};
#define NUM_MODULES (sizeof(module_names) / sizeof(*module_names))

static so_module modules[NUM_MODULES];

// split the process memory: the game's malloc resolves to newlib's, so the
// newlib heap gets everything except the SO_MEMORY_MB pool for the .so modules
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t so_size = (size_t)SO_MEMORY_MB * 1024 * 1024;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = size > so_size ? size - so_size : size / 2;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

// render at the real panel size so the engine's GL viewport fills switch-sdl2's
// framebuffer; config screen_width/height override, -1 means auto by dock state
static void set_screen_size(void) {
  if (config.screen_width > 0 && config.screen_height > 0) {
    screen_width = config.screen_width;
    screen_height = config.screen_height;
  } else if (appletGetOperationMode() == AppletOperationMode_Console) {
    screen_width = 1920;
    screen_height = 1080;
  } else {
    screen_width = 1280;
    screen_height = 720;
  }
}

static int is_episodic_game(void) {
  return !strcmp(config.gamedir, "episodic") || !strcmp(config.gamedir, "ep2");
}

static int is_regular_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int file_contains_text(const char *path, const char *needle) {
  unsigned char buf[8192];
  size_t needle_len = strlen(needle);
  size_t keep = 0;
  FILE *f;

  if (needle_len == 0 || needle_len >= sizeof(buf))
    return 0;

  f = fopen(path, "rb");
  if (!f)
    return 0;

  for (;;) {
    size_t got = fread(buf + keep, 1, sizeof(buf) - keep, f);
    size_t total = keep + got;

    if (total >= needle_len) {
      for (size_t i = 0; i <= total - needle_len; i++) {
        if (!memcmp(buf + i, needle, needle_len)) {
          fclose(f);
          return 1;
        }
      }
    }

    if (got == 0)
      break;

    keep = total < needle_len - 1 ? total : needle_len - 1;
    memmove(buf, buf + total - keep, keep);
  }

  fclose(f);
  return 0;
}

static const char *episodic_lib_dir(void) {
  static int checked = -1;

  if (checked < 0) {
    checked = is_regular_file("lib/episodic/libclient.so") &&
              file_contains_text("lib/episodic/libserver.so", "sk_zombie_soldier_health");
  }

  if (checked)
    return "lib/episodic";

  return NULL;
}

static const char *module_load_path(const char *name, char *path, size_t size) {
  if (is_episodic_game() &&
      (!strcmp(name, "libclient.so") || !strcmp(name, "libserver.so"))) {
    const char *dir = episodic_lib_dir();
    if (!dir)
      fatal_error("Episode One/Two need\n%s/lib/episodic/libclient.so\n%s/lib/episodic/libserver.so",
                  config.install_root, config.install_root);
    snprintf(path, size, "%s/%s", dir, name);
    return path;
  }

  snprintf(path, size, "lib/%s", name);
  return path;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.\nRun with a full title override, not applet mode.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  const char *files[] = {
    "lib/liblauncher.so",
    "lib/libengine.so",
    "lib/libclient.so",
    "lib/libserver.so",
    "lib/libtogl.so",
    "assets/extras_dir.vpk",
    "files/dejavusans.ttf",
    "hl2/gameinfo.txt",
    "hl2/steam.inf",
    "hl2/hl2_pak_dir.vpk",
    "hl2/hl2_misc_dir.vpk",
    "platform/platform_misc_dir.vpk",
  };
  struct stat st;
  const unsigned int numfiles = sizeof(files) / sizeof(*files);
  for (unsigned int i = 0; i < numfiles; ++i) {
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s/%s\nCheck your data files.", config.install_root, files[i]);
  }
}

static void load_modules(void) {
  char path[512];
  void *base = heap_so_base;
  size_t remaining = heap_so_limit;

  // pass 1: load everything so cross-module resolution sees all exports
  for (unsigned int i = 0; i < NUM_MODULES; i++) {
    const int res = so_load(&modules[i],
                            module_load_path(module_names[i], path, sizeof(path)),
                            base, remaining);
    if (res < 0)
      fatal_error("Could not load\n%s\n(so_load: %d)", path, res);
    base = (void *)ALIGN_MEM((uintptr_t)base + modules[i].load_size, 0x1000);
    remaining = heap_so_limit - ((uintptr_t)base - (uintptr_t)heap_so_base);
  }

  // pass 2: relocate + resolve now that every export is visible
  for (unsigned int i = 0; i < NUM_MODULES; i++) {
    so_relocate(&modules[i]);
    so_resolve(&modules[i], dynlib_functions, dynlib_numfunctions, 1);
  }

  // pass 3: map as code, then run initializers in dependency order
  for (unsigned int i = 0; i < NUM_MODULES; i++) {
    so_finalize(&modules[i]);
    so_flush_caches(&modules[i]);
  }
  for (unsigned int i = 0; i < NUM_MODULES; i++) {
    so_execute_init_array(&modules[i]);
    so_free_temp(&modules[i]);
  }
}

static void append_arg(char *dst, size_t dst_size, const char *arg) {
  size_t len;
  size_t arg_len;

  if (!dst_size || !arg || !arg[0])
    return;

  len = strlen(dst);
  if (len >= dst_size - 1)
    return;

  if (len > 0) {
    dst[len++] = ' ';
    dst[len] = '\0';
  }

  arg_len = strlen(arg);
  if (arg_len >= dst_size - len)
    arg_len = dst_size - len - 1;
  memcpy(dst + len, arg, arg_len);
  dst[len + arg_len] = '\0';
}

static void append_int_arg(char *dst, size_t dst_size, int value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  append_arg(dst, dst_size, buf);
}

static int option_arg_takes_value(const char *arg) {
  return !strcmp(arg, "+joystick") ||
         !strcmp(arg, "joystick") ||
         !strcmp(arg, "+touch_enable") ||
         !strcmp(arg, "touch_enable") ||
         !strcmp(arg, "+cl_showfps") ||
         !strcmp(arg, "cl_showfps") ||
         !strcmp(arg, "-language") ||
         !strcmp(arg, "-audiolanguage");
}

static int option_arg_is_managed(const char *arg) {
  return !strcmp(arg, "-console") || option_arg_takes_value(arg);
}

static void append_user_args(char *dst, size_t dst_size, const char *args) {
  char buf[256];
  char *tok;
  int skip_next = 0;

  if (!args || !args[0])
    return;

  snprintf(buf, sizeof(buf), "%s", args);
  tok = strtok(buf, " \t\r\n");
  while (tok) {
    if (skip_next) {
      skip_next = 0;
    } else if (option_arg_is_managed(tok)) {
      skip_next = option_arg_takes_value(tok);
    } else {
      append_arg(dst, dst_size, tok);
    }
    tok = strtok(NULL, " \t\r\n");
  }
}

static const char *language_env_name(const char *lang) {
  static const struct {
    const char *name;
    const char *env;
  } langs[] = {
    { "english", "en_US" },
    { "german", "de_DE" },
    { "french", "fr_FR" },
    { "italian", "it_IT" },
    { "spanish", "es_ES" },
    { "koreana", "ko_KR" },
    { "schinese", "zh_CN" },
    { "tchinese", "zh_TW" },
    { "russian", "ru_RU" },
    { "thai", "th_TH" },
    { "japanese", "ja_JP" },
    { "portuguese", "pt_PT" },
    { "polish", "pl_PL" },
    { "danish", "da_DK" },
    { "dutch", "nl_NL" },
    { "finnish", "fi_FI" },
    { "norwegian", "no_NO" },
    { "swedish", "sv_SE" },
    { "romanian", "ro_RO" },
    { "turkish", "tr_TR" },
    { "hungarian", "hu_HU" },
    { "czech", "cs_CZ" },
    { "brazilian", "pt_BR" },
    { "bulgarian", "bg_BG" },
    { "greek", "el_GR" },
    { "ukrainian", "uk_UA" },
  };

  for (unsigned i = 0; i < sizeof(langs) / sizeof(*langs); i++) {
    if (!strcmp(lang, langs[i].name))
      return langs[i].env;
  }
  return "en_US";
}

static void setup_game_environment(so_module *launcher) {
  static char extras_path[384];
  static char lib_path[384];
  static char mod_lib_path[384];
  static char cmdline[640];
  const int episodic_game = is_episodic_game();
  const char *mod_lib_dir = episodic_game ? episodic_lib_dir() : NULL;

  snprintf(extras_path, sizeof(extras_path), "%s/assets/extras_dir.vpk", config.install_root);
  snprintf(lib_path, sizeof(lib_path), "%s/lib", config.install_root);

  setenv("APP_DATA_PATH", config.install_root, 1);
  setenv("APP_LIB_PATH", lib_path, 1);
  if (mod_lib_dir) {
    snprintf(mod_lib_path, sizeof(mod_lib_path), "%s/%s", config.install_root, mod_lib_dir);
    setenv("APP_MOD_LIB", mod_lib_path, 1);
  } else {
    unsetenv("APP_MOD_LIB");
  }
  setenv("VALVE_GAME_PATH", config.install_root, 1);
  setenv("EXTRAS_VPK_PATH", extras_path, 1);
  setenv("LANG", language_env_name(config.lang), 1);
  setenv("HOME", config.install_root, 1);
  setenv("LIBGL_USEVBO", "0", 1);
  setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1);

  cmdline[0] = '\0';
  append_arg(cmdline, sizeof(cmdline), "-game");
  append_arg(cmdline, sizeof(cmdline), config.gamedir);
  append_arg(cmdline, sizeof(cmdline), "-w");
  append_int_arg(cmdline, sizeof(cmdline), screen_width);
  append_arg(cmdline, sizeof(cmdline), "-h");
  append_int_arg(cmdline, sizeof(cmdline), screen_height);
  append_user_args(cmdline, sizeof(cmdline), config.args);
  if (config.console)
    append_arg(cmdline, sizeof(cmdline), "-console");
  append_arg(cmdline, sizeof(cmdline), "+cl_showfps");
  append_arg(cmdline, sizeof(cmdline), config.show_fps ? "1" : "0");
  append_arg(cmdline, sizeof(cmdline), "+joystick");
  append_arg(cmdline, sizeof(cmdline), config.gamepad ? "1" : "0");
  append_arg(cmdline, sizeof(cmdline), "+touch_enable");
  append_arg(cmdline, sizeof(cmdline), config.touch_hud ? "1" : "0");
  append_arg(cmdline, sizeof(cmdline), "-language");
  append_arg(cmdline, sizeof(cmdline), config.lang);
  append_arg(cmdline, sizeof(cmdline), "-audiolanguage");
  append_arg(cmdline, sizeof(cmdline), config.lang);
  if (episodic_game) {
    append_arg(cmdline, sizeof(cmdline), "+hl2_episodic");
    append_arg(cmdline, sizeof(cmdline), "1");
  }
#if DEBUG_LOG
  strncat(cmdline, " -dev 2", sizeof(cmdline) - strlen(cmdline) - 1);
#endif

  debugPrintf("command line: %s\n", cmdline);

  void (* setArgs)(void *env, void *cls, const char *jstr) =
      (void *)so_find_addr_rx(launcher, "Java_com_valvesoftware_ValveActivity2_setArgs");
  setArgs(fake_env, NULL, cmdline);
}

// FastLoad helps loading but floors GPU clocks, so switch it off once frames flow.
extern volatile unsigned sdl_swap_count;
extern volatile int g_video_playing;

#define CLK_WIN_TICKS 12
#define CLK_BOOST_AT   5
#define CLK_DROP_AT   12

static void *clock_thread(void *arg) {
  (void)arg;
  unsigned ring[CLK_WIN_TICKS] = { 0 };
  unsigned win_sum = 0;
  unsigned last_swap = sdl_swap_count;
  int ri = 0;
  int boosted = 1;

  for (;;) {
    svcSleepThread(250000000ll);
    if (g_video_playing) {
      last_swap = sdl_swap_count;
      continue;
    }

    const unsigned cur = sdl_swap_count;
    win_sum -= ring[ri];
    ring[ri] = cur - last_swap;
    win_sum += ring[ri];
    ri = (ri + 1) % CLK_WIN_TICKS;
    last_swap = cur;

    if (boosted) {
      if (win_sum > CLK_DROP_AT) { cpu_boost(0); boosted = 0; }
    } else {
      if (win_sum <= CLK_BOOST_AT) { cpu_boost(1); boosted = 1; }
    }
  }
  return NULL;
}

static void start_clock_thread(void) {
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 128 * 1024);
  pthread_create(&t, &at, clock_thread, NULL);
  pthread_attr_destroy(&at);
}

int main(void) {
  cpu_boost(1);

  check_syscalls();

  if (read_config(CONFIG_NAME) < 0) {
    chdir(DEFAULT_INSTALL_ROOT);
    if (read_config(CONFIG_NAME) < 0)
      write_config(CONFIG_NAME);
  }
  chdir(config.install_root);

  int picker = picker_run();
  if (picker == 0) {
    cpu_boost(0);
    return 0;
  }

  check_data();
  set_screen_size();

  fake_tls_install();
  jni_init();
  proc_files_init(config.install_root);

  load_modules();

  so_module *launcher = so_find_module("liblauncher.so");
  if (!launcher)
    fatal_error("liblauncher.so did not load");

  setup_game_environment(launcher);

  int (* LauncherMainAndroid)(int argc, char **argv) =
      (void *)so_find_addr_rx(launcher, "LauncherMainAndroid");

  start_clock_thread();

  static char *fake_argv[] = { "hl2_linux", NULL };
  const int ret = LauncherMainAndroid(1, fake_argv);
  debugPrintf("LauncherMainAndroid returned %d\n", ret);

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);

  return 0;
}
