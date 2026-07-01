/* imports.c -- .so import resolution for the HL2 Source modules
 *
 * Covers the host-provided import surface: SDL_* symbols (served by switch-sdl2)
 * plus the libc/zlib/net/dl symbols. Symbols the Source modules import from each
 * other (CreateInterface, tier0 API, ...) resolve module-to-module in so_resolve
 * and are NOT here. The table takes priority over module exports.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <wchar.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <time.h>
#include <dirent.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <zlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "dl_emu.h"
#include "video_player.h"
#include "thread_affinity.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __stack_chk_fail;
extern int __fpclassifyd(double);
extern int __isnanf(float);
// in devkitA64's libc.a but not declared in its malloc.h
extern size_t malloc_usable_size(void *ptr);

static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

// ---------------------------------------------------------------------------
// android log
// ---------------------------------------------------------------------------

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];

  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);

  debugPrintf("%s: %s\n", tag, string);
#endif
  return 0;
}

int __android_log_write(int prio, const char *tag, const char *text) {
  debugPrintf("%s: %s\n", tag, text);
  return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
#if DEBUG_LOG
  static char string[0x1000];
  vsnprintf(string, sizeof(string), fmt, va);
  debugPrintf("%s: %s\n", tag, string);
#endif
  return 0;
}

// ---------------------------------------------------------------------------
// pthread: bionic struct sizes differ from newlib's, so the synchronization
// objects are wrapped behind pointers stored in their first bytes
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// bionic pthread_mutex_t (40 bytes) / pthread_cond_t (48 bytes) emulation,
// constructed IN PLACE inside the game's storage.
//
// Source treats these as plain values: it memcpy's structs containing live
// mutexes (CUtlVector growth), destroys the originals, and memset()s
// objects back to zero. A pointer-in-storage scheme breaks all of that
// (destroying the original frees the mutex a copy still points to -- this
// deadlocked AddSearchPath on a mutex no thread held). newlib's mutex is
// 12 bytes and cond 16, so the real object plus an init-state word fits
// inside bionic's storage and value semantics survive.
// ---------------------------------------------------------------------------

// bionic pthread_mutexattr_t is an int holding the type in the low bits.
// Source's CThreadMutex constructs every mutex PTHREAD_MUTEX_RECURSIVE and
// relies on re-entry, so the type must be tracked.
#define BIONIC_PTHREAD_MUTEX_RECURSIVE 1

#define SYNC_STATE_RAW 0
#define SYNC_STATE_BUSY 0xA11C0911u
#define SYNC_STATE_READY 0x5EAD15EDu

typedef struct {
  pthread_mutex_t real;   // 12 bytes on devkitA64 newlib
  uint32_t recursive;
  volatile uint32_t state;
  uint32_t pad[5];        // up to bionic's 40
} InplaceMutex;

typedef struct {
  pthread_cond_t real;    // 16 bytes on devkitA64 newlib
  volatile uint32_t state;
  uint32_t pad[7];        // up to bionic's 48
} InplaceCond;

_Static_assert(sizeof(InplaceMutex) == 40, "InplaceMutex must match bionic pthread_mutex_t");
_Static_assert(sizeof(InplaceCond) == 48, "InplaceCond must match bionic pthread_cond_t");

int pthread_mutexattr_init_fake(int *attr) {
  *attr = 0;
  return 0;
}

int pthread_mutexattr_settype_fake(int *attr, int type) {
  *attr = type;
  return 0;
}

static void mutex_construct(InplaceMutex *m, int recursive) {
  pthread_mutexattr_t at;
  pthread_mutexattr_init(&at);
  if (recursive)
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&m->real, &at);
  pthread_mutexattr_destroy(&at);
  m->recursive = recursive;
}

// lazy in-place init for statically-initialized bionic mutexes: word 0 is
// 0 (NORMAL) or type<<14 (PTHREAD_RECURSIVE_MUTEX_INITIALIZER = 0x4000).
// claim via CAS on the state word so concurrent first use stays correct.
static pthread_mutex_t *mutex_inplace(InplaceMutex *m) {
  for (;;) {
    const uint32_t st = m->state;
    if (st == SYNC_STATE_READY)
      return &m->real;
    if (st == SYNC_STATE_RAW || st != SYNC_STATE_BUSY) {
      if (__sync_bool_compare_and_swap(&m->state, st, SYNC_STATE_BUSY)) {
        const int recursive = (*(uint32_t *)m & 0x4000) != 0;
        mutex_construct(m, recursive);
        __sync_synchronize();
        m->state = SYNC_STATE_READY;
        return &m->real;
      }
      continue;
    }
    svcSleepThread(10000); // another thread is constructing
  }
}

int pthread_mutex_init_fake(InplaceMutex *m, const int *mutexattr) {
  const int recursive = (mutexattr && (*mutexattr & 3) == BIONIC_PTHREAD_MUTEX_RECURSIVE);
  mutex_construct(m, recursive);
  __sync_synchronize();
  m->state = SYNC_STATE_READY;
  return 0;
}

int pthread_mutex_destroy_fake(InplaceMutex *m) {
  if (m && m->state == SYNC_STATE_READY) {
    pthread_mutex_destroy(&m->real);
    m->state = SYNC_STATE_RAW;
  }
  return 0;
}

int pthread_mutex_lock_fake(InplaceMutex *m) {
  return pthread_mutex_lock(mutex_inplace(m));
}

int pthread_mutex_trylock_fake(InplaceMutex *m) {
  return pthread_mutex_trylock(mutex_inplace(m));
}

int pthread_mutex_unlock_fake(InplaceMutex *m) {
  return pthread_mutex_unlock(mutex_inplace(m));
}

static pthread_cond_t *cond_inplace(InplaceCond *c) {
  for (;;) {
    const uint32_t st = c->state;
    if (st == SYNC_STATE_READY)
      return &c->real;
    if (st != SYNC_STATE_BUSY) {
      if (__sync_bool_compare_and_swap(&c->state, st, SYNC_STATE_BUSY)) {
        c->real = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
        pthread_cond_init(&c->real, NULL);
        __sync_synchronize();
        c->state = SYNC_STATE_READY;
        return &c->real;
      }
      continue;
    }
    svcSleepThread(10000);
  }
}

int pthread_cond_init_fake(InplaceCond *c, const int *condattr) {
  (void)condattr; // clock id handled by the timedwait rebase instead
  c->real = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
  pthread_cond_init(&c->real, NULL);
  __sync_synchronize();
  c->state = SYNC_STATE_READY;
  return 0;
}

int pthread_cond_broadcast_fake(InplaceCond *c) {
  return pthread_cond_broadcast(cond_inplace(c));
}

int pthread_cond_signal_fake(InplaceCond *c) {
  return pthread_cond_signal(cond_inplace(c));
}

int pthread_cond_destroy_fake(InplaceCond *c) {
  if (c && c->state == SYNC_STATE_READY) {
    pthread_cond_destroy(&c->real);
    c->state = SYNC_STATE_RAW;
  }
  return 0;
}

int pthread_cond_wait_fake(InplaceCond *c, InplaceMutex *m) {
  return pthread_cond_wait(cond_inplace(c), mutex_inplace(m));
}

// bionic condvars may carry CLOCK_MONOTONIC abstimes (pthread_condattr_
// setclock); newlib always interprets them as CLOCK_REALTIME. Detect which
// clock the abstime was built against and rebase it onto REALTIME.
static int64_t rebase_abstime_ns(const struct timespec *t) {
  struct timespec rt;
  clock_gettime(CLOCK_REALTIME, &rt);
  const int64_t now_rt = (int64_t)rt.tv_sec * 1000000000ll + rt.tv_nsec;
  const int64_t now_mono = (int64_t)armTicksToNs(armGetSystemTick());
  const int64_t abs_ns = (int64_t)t->tv_sec * 1000000000ll + t->tv_nsec;
  const int64_t day = 24ll * 3600 * 1000000000ll;

  int64_t rel = abs_ns - now_rt;
  if (rel < -1000000ll || rel >= day) {
    const int64_t rel_mono = abs_ns - now_mono;
    if (rel_mono >= -1000000ll && rel_mono < day)
      rel = rel_mono;
  }
  if (rel < 0)
    rel = 0;
  return now_rt + rel;
}

// the Source binaries compare against bionic errno values; newlib's differ
// for the ones that drive tier0's threadtools state machine
// (CThreadSyncObject::Wait loops on ret != 110) -- translate at the boundary
#define BIONIC_ETIMEDOUT 110

int pthread_cond_timedwait_fake(InplaceCond *c, InplaceMutex *m, const struct timespec *t) {
  pthread_cond_t *rc = cond_inplace(c);
  pthread_mutex_t *rm = mutex_inplace(m);
  if (!t)
    return pthread_cond_wait(rc, rm);
  const int64_t abs_rt = rebase_abstime_ns(t);
  struct timespec ts = { .tv_sec = abs_rt / 1000000000ll, .tv_nsec = abs_rt % 1000000000ll };
  const int r = pthread_cond_timedwait(rc, rm, &ts);
  return r == ETIMEDOUT ? BIONIC_ETIMEDOUT : r;
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

// every game-created thread needs the fake bionic TLS block installed
// before any engine code runs on it (see libc_shim.c)
typedef struct {
  void *(*func)(void *);
  void *arg;
} FakeThreadStart;

static void *fake_thread_trampoline(void *arg) {
  FakeThreadStart start = *(FakeThreadStart *)arg;
  free(arg);
  fake_tls_install();
  thread_affinity_pin_source_thread();
  return start.func(start.arg);
}

// bionic pthread_t is 8 bytes (long); newlib's may be narrower, so the
// value is widened explicitly through a uint64_t slot
int pthread_create_fake(uint64_t *thread, const void *attr, void *(*entry)(void *), void *arg) {
  (void)attr;

  FakeThreadStart *start = malloc(sizeof(*start));
  if (!start)
    return -1;
  start->func = entry;
  start->arg = arg;

  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 1024 * 1024);

  pthread_t t = 0;
  const int ret = pthread_create(&t, &at, fake_thread_trampoline, start);
  pthread_attr_destroy(&at);
  if (ret != 0) {
    free(start);
    return ret;
  }
  if (thread)
    *thread = (uint64_t)t;
  return 0;
}

int pthread_join_fake(uint64_t t, void **retval) {
  return pthread_join((pthread_t)t, retval);
}

int pthread_detach_fake(uint64_t t) {
  return pthread_detach((pthread_t)t);
}

uint64_t pthread_self_fake(void) {
  return (uint64_t)pthread_self();
}

static int sched_get_priority_max_fake(int policy) {
  (void)policy;
  return 0;
}

// the audio callback fires on an SDL-created thread, which needs the fake bionic
// TLS installed before engine code runs on it (engine reads the stack-guard slot)
static SDL_AudioCallback game_audio_callback;
static void *game_audio_userdata;

static void audio_callback_trampoline(void *userdata, Uint8 *stream, int len) {
  static int affinity_set;
  (void)userdata;
  fake_tls_install();
  if (__atomic_exchange_n(&affinity_set, 1, __ATOMIC_RELAXED) == 0)
    thread_affinity_pin_background_thread();
  game_audio_callback(game_audio_userdata, stream, len);
}

static SDL_AudioDeviceID SDL_OpenAudioDevice_fake(const char *device, int iscapture,
    const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes) {
  SDL_AudioSpec spec = *desired;
  if (spec.callback) {
    game_audio_callback = spec.callback;
    game_audio_userdata = spec.userdata;
    spec.callback = audio_callback_trampoline;
    spec.userdata = NULL;
  }
  return SDL_OpenAudioDevice(device, iscapture, &spec, obtained, allowed_changes);
}

volatile unsigned sdl_swap_count; // frame counter watched by the clock manager

static void apply_display_pass(SDL_Window *window); // defined below

// switch-sdl2 reports the desktop mode as 1920x1080 regardless of dock state, so
// going fullscreen inflates the window to 1080p while the engine keeps rendering
// at our -w/-h (the bottom-left-corner symptom). Report our render size for the
// display/window queries; the compositor scales the final buffer to the panel.
static int SDL_GetDesktopDisplayMode_fake(int idx, SDL_DisplayMode *mode) {
  const int r = SDL_GetDesktopDisplayMode(idx, mode);
  if (r == 0 && mode) { mode->w = screen_width; mode->h = screen_height; }
  return r;
}

static int SDL_GetCurrentDisplayMode_fake(int idx, SDL_DisplayMode *mode) {
  const int r = SDL_GetCurrentDisplayMode(idx, mode);
  if (r == 0 && mode) { mode->w = screen_width; mode->h = screen_height; }
  return r;
}

static int SDL_GetDisplayBounds_fake(int idx, SDL_Rect *rect) {
  const int r = SDL_GetDisplayBounds(idx, rect);
  if (r == 0 && rect) { rect->x = 0; rect->y = 0; rect->w = screen_width; rect->h = screen_height; }
  return r;
}

static void SDL_SetWindowSize_fake(SDL_Window *window, int w, int h) {
  (void)w; (void)h;
  SDL_SetWindowSize(window, screen_width, screen_height);
}

static int SDL_SetWindowDisplayMode_fake(SDL_Window *window, const SDL_DisplayMode *mode) {
  SDL_DisplayMode m;
  if (mode) {
    m = *mode;
    m.w = screen_width;
    m.h = screen_height;
    return SDL_SetWindowDisplayMode(window, &m);
  }
  return SDL_SetWindowDisplayMode(window, NULL);
}

// no-op: the Switch window always fills the screen and the compositor scales the
// presented buffer; letting SDL process FULLSCREEN_DESKTOP would resize the
// drawable to 1920x1080 behind the engine's back (the bottom-left-corner bug)
static int SDL_SetWindowFullscreen_fake(SDL_Window *window, Uint32 flags) {
  (void)window; (void)flags;
  return 0;
}

// switch-sdl2 has no WM backend; the real call returns false and the engine
// fatal-errors during video playback. Only the bool result is checked.
static SDL_bool SDL_GetWindowWMInfo_fake(SDL_Window *window, SDL_SysWMinfo *info) {
  (void)window;
  if (info)
    info->subsystem = SDL_SYSWM_UNKNOWN;
  return SDL_TRUE;
}

static void SDL_GL_SwapWindow_fake(SDL_Window *window) {
  ++sdl_swap_count;
  apply_display_pass(window);
  SDL_GL_SwapWindow(window);
}

// Source handles touch itself; drop SDL touch-to-mouse duplicates.
static int SDL_PollEvent_fake(SDL_Event *event) {
  for (;;) {
    const int r = SDL_PollEvent(event);
    if (!r)
      return 0;
    switch (event->type) {
      case SDL_MOUSEMOTION:
        if (event->motion.which == SDL_TOUCH_MOUSEID) continue;
        return r;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        if (event->button.which == SDL_TOUCH_MOUSEID) continue;
        return r;
      case SDL_MOUSEWHEEL:
        if (event->wheel.which == SDL_TOUCH_MOUSEID) continue;
        return r;
      default:
        return r;
    }
  }
}

static SDL_bool SDL_SetHint_fake(const char *name, const char *value) {
  if (name && !strcmp(name, "SDL_TOUCH_MOUSE_EVENTS"))
    value = "0";
  return SDL_SetHint(name, value);
}

// Feed libnx swkbd text back through SDL events.
static void osk_push_key(SDL_Scancode sc, SDL_Keycode kc) {
  SDL_Event e;
  SDL_zero(e);
  e.key.keysym.scancode = sc;
  e.key.keysym.sym = kc;
  e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED;  SDL_PushEvent(&e);
  e.type = SDL_KEYUP;   e.key.state = SDL_RELEASED; SDL_PushEvent(&e);
}

static int osk_utf8_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xe) return 3;
  if ((c >> 3) == 0x1e) return 4;
  return 1;
}

static void osk_inject(const char *s) {
  for (int i = 0; i < 64; i++)
    osk_push_key(SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE);

  for (size_t i = 0; s[i];) {
    const int cl = osk_utf8_len((unsigned char)s[i]);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_TEXTINPUT;
    int k = 0;
    for (; k < cl && s[i + k]; k++)
      e.text.text[k] = s[i + k];
    e.text.text[k] = '\0';
    SDL_PushEvent(&e);
    i += k ? k : 1;
  }
}

static void SDL_StartTextInput_fake(void) {
  static volatile bool busy = false;
  if (busy) return;
  busy = true;

  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0))) { busy = false; return; }
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetStringLenMax(&kbd, 255);

  char out[256];
  out[0] = '\0';
  const Result rc = swkbdShow(&kbd, out, sizeof(out));
  swkbdClose(&kbd);
  busy = false;

  if (R_SUCCEEDED(rc))
    osk_inject(out);
}

static SDL_GLContext SDL_GL_CreateContext_fake(SDL_Window *window) {
  // Pin whichever Source thread owns GL before the context and driver state
  // become hot in that core's caches.
  thread_affinity_pin_render_thread();
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  static int startup_videos_done;
  if (ctx && !startup_videos_done) {
    startup_videos_done = 1;
    play_startup_videos(window, ctx);
  }
  return ctx;
}

// Switch presents through a linear default FB; keep Source's sRGB bytes intact.
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9 /* GL_FRAMEBUFFER_SRGB_EXT (sRGB_write_control) */
#endif

static void (*p_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint,
                                   GLint, GLint, GLbitfield, GLenum);
static void (*p_glBlitFramebufferEXT)(GLint, GLint, GLint, GLint, GLint, GLint,
                                      GLint, GLint, GLbitfield, GLenum);

static void blit_to_default_no_srgb(void (*blit)(GLint, GLint, GLint, GLint,
                                                 GLint, GLint, GLint, GLint,
                                                 GLbitfield, GLenum),
                                    GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                    GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                    GLbitfield mask, GLenum filter) {
  GLint drawfb = -1;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawfb);
  if (drawfb != 0) {
    blit(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    return;
  }

  const GLboolean wasSRGB = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  if (wasSRGB)
    glDisable(GL_FRAMEBUFFER_SRGB);

  blit(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);

  if (wasSRGB)
    glEnable(GL_FRAMEBUFFER_SRGB);
}

static void glBlitFramebuffer_fake(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                   GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                   GLbitfield mask, GLenum filter) {
  blit_to_default_no_srgb(p_glBlitFramebuffer, sx0, sy0, sx1, sy1,
                          dx0, dy0, dx1, dy1, mask, filter);
}

static void glBlitFramebufferEXT_fake(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                      GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                      GLbitfield mask, GLenum filter) {
  blit_to_default_no_srgb(p_glBlitFramebufferEXT, sx0, sy0, sx1, sy1,
                          dx0, dy0, dx1, dy1, mask, filter);
}

void *eglGetProcAddress_fake(const char *name) {
  if (name && !strcmp(name, "glBlitFramebuffer")) {
    if (!p_glBlitFramebuffer)
      p_glBlitFramebuffer = (void *)eglGetProcAddress(name);
    return (void *)&glBlitFramebuffer_fake;
  }
  if (name && !strcmp(name, "glBlitFramebufferEXT")) {
    if (!p_glBlitFramebufferEXT)
      p_glBlitFramebufferEXT = (void *)eglGetProcAddress(name);
    return (void *)&glBlitFramebufferEXT_fake;
  }
  return (void *)eglGetProcAddress(name);
}

// switch-sdl2 ignores SDL_SetWindowGammaRamp; apply non-identity ramps here.
static Uint16 g_gamma_r[256], g_gamma_g[256], g_gamma_b[256];
static volatile int g_gamma_have_ramp;
static volatile int g_gamma_ramp_dirty;
static volatile int g_gamma_identity = 1;

static int SDL_SetWindowGammaRamp_fake(SDL_Window *window, const Uint16 *r, const Uint16 *g, const Uint16 *b) {
  (void)window;
  if (r && g && b) {
    memcpy(g_gamma_r, r, sizeof(g_gamma_r));
    memcpy(g_gamma_g, g, sizeof(g_gamma_g));
    memcpy(g_gamma_b, b, sizeof(g_gamma_b));
    g_gamma_have_ramp = 1;
    g_gamma_ramp_dirty = 1;
    int ident = 1;
    for (int i = 0; i < 256; i++) {
      if (abs((g_gamma_r[i] >> 8) - i) > 1 ||
          abs((g_gamma_g[i] >> 8) - i) > 1 ||
          abs((g_gamma_b[i] >> 8) - i) > 1) { ident = 0; break; }
    }
    g_gamma_identity = ident;
  }
  return 0;
}

static GLuint gamma_prog, gamma_vao, gamma_scene_tex, gamma_lut_tex;
static GLint gamma_u_scene, gamma_u_lut;
static int gamma_tex_w, gamma_tex_h, gamma_inited;

static GLuint gamma_compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    debugPrintf("gamma: shader compile failed: %s\n", log);
  }
  return s;
}

static int gamma_init(void) {
  static const char *vs =
    "#version 300 es\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "  vec2 p = vec2(gl_VertexID==1 ? 3.0 : -1.0, gl_VertexID==2 ? 3.0 : -1.0);\n"
    "  vUV = p*0.5 + 0.5;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
  static const char *fs =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 vUV; out vec4 o;\n"
    "uniform sampler2D uScene; uniform sampler2D uLut;\n"
    "float h(vec2 p){ return fract(sin(dot(p, vec2(12.9898,78.233))) * 43758.5453); }\n"
    "void main(){\n"
    "  vec3 c = texture(uScene, vUV).rgb;\n"
    "  c = vec3(texture(uLut, vec2(c.r, 0.5)).r,\n"
    "           texture(uLut, vec2(c.g, 0.5)).g,\n"
    "           texture(uLut, vec2(c.b, 0.5)).b);\n"
    "  float n1 = h(gl_FragCoord.xy), n2 = h(gl_FragCoord.xy + vec2(13.0, 7.0));\n"
    "  c += vec3((n1 + n2 - 1.0) * (1.0/255.0));\n"
    "  o = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";
  gamma_prog = glCreateProgram();
  GLuint v = gamma_compile(GL_VERTEX_SHADER, vs);
  GLuint f = gamma_compile(GL_FRAGMENT_SHADER, fs);
  glAttachShader(gamma_prog, v);
  glAttachShader(gamma_prog, f);
  glLinkProgram(gamma_prog);
  GLint ok = 0;
  glGetProgramiv(gamma_prog, GL_LINK_STATUS, &ok);
  glDeleteShader(v);
  glDeleteShader(f);
  if (!ok) {
    debugPrintf("gamma: program link failed\n");
    return 0;
  }
  gamma_u_scene = glGetUniformLocation(gamma_prog, "uScene");
  gamma_u_lut = glGetUniformLocation(gamma_prog, "uLut");
  glGenVertexArrays(1, &gamma_vao);
  glGenTextures(1, &gamma_scene_tex);
  glGenTextures(1, &gamma_lut_tex);
  for (int i = 0; i < 2; i++) {
    glBindTexture(GL_TEXTURE_2D, i ? gamma_scene_tex : gamma_lut_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  uint8_t idlut[256 * 4];
  for (int i = 0; i < 256; i++) {
    idlut[i * 4 + 0] = idlut[i * 4 + 1] = idlut[i * 4 + 2] = (uint8_t)i;
    idlut[i * 4 + 3] = 255;
  }
  glBindTexture(GL_TEXTURE_2D, gamma_lut_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, idlut);
  gamma_inited = 1;
  return 1;
}

static void gamma_upload_lut(void) {
  uint8_t lut[256 * 4];
  for (int i = 0; i < 256; i++) {
    lut[i * 4 + 0] = (uint8_t)(g_gamma_r[i] >> 8);
    lut[i * 4 + 1] = (uint8_t)(g_gamma_g[i] >> 8);
    lut[i * 4 + 2] = (uint8_t)(g_gamma_b[i] >> 8);
    lut[i * 4 + 3] = 255;
  }
  glBindTexture(GL_TEXTURE_2D, gamma_lut_tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, lut);
}

static void apply_display_pass(SDL_Window *window) {
  if (g_gamma_identity)
    return;
  if (!gamma_inited && !gamma_init())
    return;

  int w = 0, h = 0;
  SDL_GL_GetDrawableSize(window, &w, &h);
  if (w <= 0 || h <= 0) { w = screen_width; h = screen_height; }

  GLint prevProg = 0, prevVAO = 0, prevActive = 0, prevFBO = 0, prevTex0 = 0, prevTex1 = 0, vp[4];
  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
  glGetIntegerv(GL_VIEWPORT, vp);
  const GLboolean bDepth = glIsEnabled(GL_DEPTH_TEST), bBlend = glIsEnabled(GL_BLEND),
                  bCull = glIsEnabled(GL_CULL_FACE), bScissor = glIsEnabled(GL_SCISSOR_TEST),
                  bStencil = glIsEnabled(GL_STENCIL_TEST);
  const GLboolean bSRGB = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  GLboolean cmask[4];
  glGetBooleanv(GL_COLOR_WRITEMASK, cmask);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex1);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);

  glBindTexture(GL_TEXTURE_2D, gamma_scene_tex);
  if (w != gamma_tex_w || h != gamma_tex_h) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    gamma_tex_w = w; gamma_tex_h = h;
  }
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

  if (g_gamma_have_ramp && g_gamma_ramp_dirty) { gamma_upload_lut(); g_gamma_ramp_dirty = 0; }

  glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, w, h);

  glBindVertexArray(gamma_vao);
  glUseProgram(gamma_prog);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gamma_scene_tex); glUniform1i(gamma_u_scene, 0);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gamma_lut_tex); glUniform1i(gamma_u_lut, 1);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, prevTex1);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, prevTex0);
  glActiveTexture(prevActive);
  glBindVertexArray(prevVAO);
  glUseProgram(prevProg);
  glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  if (bDepth) glEnable(GL_DEPTH_TEST);
  if (bBlend) glEnable(GL_BLEND);
  if (bCull) glEnable(GL_CULL_FACE);
  if (bScissor) glEnable(GL_SCISSOR_TEST);
  if (bStencil) glEnable(GL_STENCIL_TEST);
  if (bSRGB) glEnable(GL_FRAMEBUFFER_SRGB);
  glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
}

// SDL_Haptic is missing in switch-sdl2; drive libnx rumble directly.
static HidVibrationDeviceHandle g_vib_handheld[2];
static HidVibrationDeviceHandle g_vib_player1[2];
static int g_vib_handheld_ok, g_vib_player1_ok, g_vib_init_done;
static int g_haptic_sentinel;

static void vib_init(void) {
  if (g_vib_init_done) return;
  g_vib_init_done = 1;
  g_vib_handheld_ok = R_SUCCEEDED(hidInitializeVibrationDevices(
      g_vib_handheld, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld));
  g_vib_player1_ok = R_SUCCEEDED(hidInitializeVibrationDevices(
      g_vib_player1, 2, HidNpadIdType_No1, HidNpadStyleSet_NpadFullCtrl));
}

static void vib_send(float amp) {
  if (amp < 0.0f) amp = 0.0f;
  else if (amp > 1.0f) amp = 1.0f;
  HidVibrationValue v;
  v.amp_low = amp;  v.freq_low = 160.0f;
  v.amp_high = amp; v.freq_high = 320.0f;
  HidVibrationValue vals[2] = { v, v };
  if (g_vib_handheld_ok && (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld))
    hidSendVibrationValues(g_vib_handheld, vals, 2);
  else if (g_vib_player1_ok)
    hidSendVibrationValues(g_vib_player1, vals, 2);
}

static SDL_Haptic *SDL_HapticOpenFromJoystick_fake(SDL_Joystick *joystick) {
  (void)joystick;
  vib_init();
  return (SDL_Haptic *)&g_haptic_sentinel;
}
static int SDL_HapticRumbleInit_fake(SDL_Haptic *h) { (void)h; return 0; }
static int SDL_HapticRumblePlay_fake(SDL_Haptic *h, float strength, Uint32 length) {
  (void)h; (void)length;
  vib_send(strength);
  return 0;
}
static int SDL_HapticRumbleStop_fake(SDL_Haptic *h) { (void)h; vib_send(0.0f); return 0; }
static void SDL_HapticClose_fake(SDL_Haptic *h) { (void)h; vib_send(0.0f); }

#define MAX_EVENT_WATCHES 8

static struct {
  SDL_EventFilter func;
  void *userdata;
} event_watches[MAX_EVENT_WATCHES];

static int event_watch_trampoline(void *userdata, SDL_Event *event) {
  const intptr_t slot = (intptr_t)userdata;
  fake_tls_install();
  return event_watches[slot].func(event_watches[slot].userdata, event);
}

static void SDL_AddEventWatch_fake(SDL_EventFilter filter, void *userdata) {
  for (intptr_t i = 0; i < MAX_EVENT_WATCHES; i++) {
    if (!event_watches[i].func) {
      event_watches[i].func = filter;
      event_watches[i].userdata = userdata;
      SDL_AddEventWatch(event_watch_trampoline, (void *)i);
      return;
    }
  }
  debugPrintf("SDL_AddEventWatch: out of slots\n");
}

static void SDL_DelEventWatch_fake(SDL_EventFilter filter, void *userdata) {
  for (intptr_t i = 0; i < MAX_EVENT_WATCHES; i++) {
    if (event_watches[i].func == filter && event_watches[i].userdata == userdata) {
      SDL_DelEventWatch(event_watch_trampoline, (void *)i);
      event_watches[i].func = NULL;
      event_watches[i].userdata = NULL;
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- C++ ABI / sanitizers / TLS ---
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__sF", (uintptr_t)&fake_sF },
  { "__errno", (uintptr_t)&__errno },
  { "__assert2", (uintptr_t)&__assert2_fake },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_write", (uintptr_t)&__android_log_write },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "gettid", (uintptr_t)&gettid_fake },

  // --- fortify ---
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },

  // --- v1.17.0025-only symbols (unused by v1.16; one binary loads either) ---
  { "__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__recvfrom_chk", (uintptr_t)&__recvfrom_chk_fake },
  { "__snprintf_chk", (uintptr_t)&__snprintf_chk_fake },
  { "__sprintf_chk", (uintptr_t)&__sprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "_ctype_", (uintptr_t)&bionic_ctype },
  { "__google_potentially_blocking_region_begin", (uintptr_t)&__google_potentially_blocking_region_begin_fake },
  { "__google_potentially_blocking_region_end", (uintptr_t)&__google_potentially_blocking_region_end_fake },
  { "isblank", (uintptr_t)&isblank },
  { "ldexp", (uintptr_t)&ldexp },
  { "wcsftime", (uintptr_t)&wcsftime },

  // --- dynamic loader ---
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },
  { "dladdr", (uintptr_t)&dladdr_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },

  // --- math ---
  { "__fpclassifyd", (uintptr_t)&__fpclassifyd },
  { "__isnanf", (uintptr_t)&__isnanf },
  { "abs", (uintptr_t)&abs },
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin },
  { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "exp", (uintptr_t)&exp },
  { "exp2", (uintptr_t)&exp2 },
  { "exp2f", (uintptr_t)&exp2f },
  { "expf", (uintptr_t)&expf },
  { "floor", (uintptr_t)&floor },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "fmodl", (uintptr_t)&fmodl_fake },
  { "frexp", (uintptr_t)&frexp },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "lrintf", (uintptr_t)&lrintf },
  { "modf", (uintptr_t)&modf },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "scalbn", (uintptr_t)&scalbn },
  { "scalbnl", (uintptr_t)&scalbnl_fake },
  { "sin", (uintptr_t)&sin },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "sinf", (uintptr_t)&sinf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },

  // --- ctype ---
  { "isalnum", (uintptr_t)&isalnum },
  { "iscntrl", (uintptr_t)&iscntrl },
  { "isgraph", (uintptr_t)&isgraph },
  { "isprint", (uintptr_t)&isprint },
  { "ispunct", (uintptr_t)&ispunct },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },

  // --- stdlib / memory ---
  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&exit },
  { "_exit", (uintptr_t)&exit },
  { "atof", (uintptr_t)&atof },
  { "atoi", (uintptr_t)&atoi },
  { "atol", (uintptr_t)&atol },
  { "atoll", (uintptr_t)&atoll },
  { "bsearch", (uintptr_t)&bsearch },
  { "calloc", (uintptr_t)&calloc },
  { "free", (uintptr_t)&free },
  { "malloc", (uintptr_t)&malloc },
  { "realloc", (uintptr_t)&realloc },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "malloc_usable_size", (uintptr_t)&malloc_usable_size },
  { "mallinfo", (uintptr_t)&mallinfo_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },
  { "getenv", (uintptr_t)&getenv },
  { "setenv", (uintptr_t)&setenv },
  { "unsetenv", (uintptr_t)&unsetenv },
  { "setlocale", (uintptr_t)&setlocale },
  { "system", (uintptr_t)&system_fake },

  // --- strings ---
  { "basename", (uintptr_t)&basename_fake },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memrchr", (uintptr_t)&memrchr_fake },
  { "memset", (uintptr_t)&memset },
  { "stpcpy", (uintptr_t)&stpcpy },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strcspn", (uintptr_t)&strcspn },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&strrchr },
  { "strspn", (uintptr_t)&strspn },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtok", (uintptr_t)&strtok },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "strtol", (uintptr_t)&strtol },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "fnmatch", (uintptr_t)&fnmatch_fake },
  { "wcscmp", (uintptr_t)&wcscmp },
  { "wcscpy", (uintptr_t)&wcscpy },

  // --- printf family ---
  { "printf", (uintptr_t)&debugPrintf },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "swprintf", (uintptr_t)&swprintf },
  { "swscanf", (uintptr_t)&swscanf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vswprintf", (uintptr_t)&vswprintf },

  // --- stdio (fake __sF aware) ---
  { "fclose", (uintptr_t)&fclose_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "feof", (uintptr_t)&feof_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgetc", (uintptr_t)&fgetc_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "freopen", (uintptr_t)&freopen_fake },
  { "fscanf", (uintptr_t)&fscanf_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "rewind", (uintptr_t)&rewind_fake },
  { "setvbuf", (uintptr_t)&setvbuf_fake },
  { "tmpnam", (uintptr_t)&tmpnam_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "popen", (uintptr_t)&popen_fake },
  { "pclose", (uintptr_t)&pclose_fake },

  // --- fd / fs ---
  { "access", (uintptr_t)&access_fake },
  { "chdir", (uintptr_t)&chdir },
  { "chmod", (uintptr_t)&ret0 },
  { "close", (uintptr_t)&close_fake },
  { "dup", (uintptr_t)&retm1 },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "getcwd", (uintptr_t)&getcwd_fake },
  { "ioctl", (uintptr_t)&ioctl_fake },
  { "isatty", (uintptr_t)&isatty },
  { "lseek", (uintptr_t)&lseek },
  { "lstat", (uintptr_t)&lstat_fake },
  { "mkdir", (uintptr_t)&mkdir },
  { "open", (uintptr_t)&open_fake },
  { "read", (uintptr_t)&read },
  { "realpath", (uintptr_t)&realpath_fake },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "stat", (uintptr_t)&stat_fake },
  { "statfs64", (uintptr_t)&statfs64_fake },
  { "unlink", (uintptr_t)&unlink },
  { "utimensat", (uintptr_t)&ret0 },
  { "write", (uintptr_t)&write },

  // --- dirent ---
  { "alphasort", (uintptr_t)&alphasort_fake },
  { "closedir", (uintptr_t)&closedir },
  { "opendir", (uintptr_t)&opendir },
  { "readdir", (uintptr_t)&readdir_fake },
  { "readdir64", (uintptr_t)&readdir_fake },
  { "scandir", (uintptr_t)&scandir_fake },

  // --- time ---
  { "asctime", (uintptr_t)&asctime },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "ctime", (uintptr_t)&ctime },
  { "ctime_r", (uintptr_t)&ctime_r },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "sleep", (uintptr_t)&sleep },
  { "strftime", (uintptr_t)&strftime },
  { "time", (uintptr_t)&time },
  { "timegm", (uintptr_t)&timegm_fake },
  { "tzset", (uintptr_t)&tzset },
  { "usleep", (uintptr_t)&usleep },

  // --- process / signals ---
  { "alarm", (uintptr_t)&ret0 },
  { "execlp", (uintptr_t)&retm1 },
  { "fork", (uintptr_t)&retm1 },
  { "geteuid", (uintptr_t)&getuid_fake },
  { "getpid", (uintptr_t)&getpid_fake },
  { "getpwuid", (uintptr_t)&getpwuid_fake },
  { "getrusage", (uintptr_t)&getrusage_fake },
  { "getuid", (uintptr_t)&getuid_fake },
  { "raise", (uintptr_t)&ret0 },
  { "sigaction", (uintptr_t)&ret0 },
  { "signal", (uintptr_t)&ret0 },
  { "pthread_sigmask", (uintptr_t)&ret0 },
  { "pthread_kill", (uintptr_t)&ret0 },

  // --- setjmp: bionic jmp_buf (32 longs) is larger than newlib's, and
  // sigsetjmp(buf, save) has the same register ABI as setjmp(buf) ---
  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },
  { "sigsetjmp", (uintptr_t)&setjmp },
  { "siglongjmp", (uintptr_t)&longjmp },

  // --- null-socket emulation (idle UDP sockets; SP uses loopback) ---
  { "accept", (uintptr_t)&sock_accept_fake },
  { "bind", (uintptr_t)&sock_bind_fake },
  { "connect", (uintptr_t)&sock_connect_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },
  { "gethostname", (uintptr_t)&gethostname_fake },
  { "getpeername", (uintptr_t)&sock_getpeername_fake },
  { "getsockname", (uintptr_t)&sock_getsockname_fake },
  { "getsockopt", (uintptr_t)&sock_getsockopt_fake },
  { "if_nametoindex", (uintptr_t)&ret0 },
  { "inet_addr", (uintptr_t)&inet_addr_fake },
  { "inet_ntop", (uintptr_t)&inet_ntop_fake },
  { "inet_pton", (uintptr_t)&inet_pton_fake },
  { "listen", (uintptr_t)&sock_listen_fake },
  { "poll", (uintptr_t)&poll_fake },
  { "recv", (uintptr_t)&sock_recv_fake },
  { "recvfrom", (uintptr_t)&sock_recvfrom_fake },
  { "select", (uintptr_t)&select_fake },
  { "send", (uintptr_t)&sock_send_fake },
  { "sendto", (uintptr_t)&sock_sendto_fake },
  { "setsockopt", (uintptr_t)&sock_setsockopt_fake },
  { "socket", (uintptr_t)&socket_fake },
  { "socketpair", (uintptr_t)&retm1 },

  // --- zlib (switch portlib) ---
  { "adler32", (uintptr_t)&adler32 },
  { "crc32", (uintptr_t)&crc32 },
  { "deflate", (uintptr_t)&deflate },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "deflateReset", (uintptr_t)&deflateReset },
  { "gzclose", (uintptr_t)&gzclose },
  { "gzopen", (uintptr_t)&gzopen },
  { "gzwrite", (uintptr_t)&gzwrite },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "inflateInit_", (uintptr_t)&inflateInit_ },
  { "inflateReset", (uintptr_t)&inflateReset },
  { "inflateReset2", (uintptr_t)&inflateReset2 },
  { "zlibVersion", (uintptr_t)&zlibVersion },

  // --- pthread ---
  { "pthread_attr_destroy", (uintptr_t)&ret0 },
  { "pthread_attr_init", (uintptr_t)&ret0 },
  { "pthread_attr_setdetachstate", (uintptr_t)&ret0 },
  { "pthread_attr_setstacksize", (uintptr_t)&ret0 },
  { "pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_fake },
  { "pthread_setschedparam", (uintptr_t)&ret0 },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific_fake },
  { "pthread_join", (uintptr_t)&pthread_join_fake },
  { "pthread_key_create", (uintptr_t)&pthread_key_create_fake },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_self", (uintptr_t)&pthread_self_fake },
  { "pthread_setname_np", (uintptr_t)&ret0 },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific_fake },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_get_priority_min", (uintptr_t)&retm1 },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_timedwait", (uintptr_t)&sem_timedwait_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },

  // --- SDL2: served natively by switch-sdl2 ---
  { "SDL_AddEventWatch", (uintptr_t)&SDL_AddEventWatch_fake },
  { "SDL_CloseAudioDevice", (uintptr_t)&SDL_CloseAudioDevice },
  { "SDL_CreateSystemCursor", (uintptr_t)&SDL_CreateSystemCursor },
  { "SDL_CreateWindow", (uintptr_t)&SDL_CreateWindow },
  { "SDL_DelEventWatch", (uintptr_t)&SDL_DelEventWatch_fake },
  { "SDL_DestroyWindow", (uintptr_t)&SDL_DestroyWindow },
  { "SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface },
  { "SDL_GL_CreateContext", (uintptr_t)&SDL_GL_CreateContext_fake },
  { "SDL_GL_DeleteContext", (uintptr_t)&SDL_GL_DeleteContext },
  { "SDL_GL_LoadLibrary", (uintptr_t)&SDL_GL_LoadLibrary },
  { "SDL_GL_MakeCurrent", (uintptr_t)&SDL_GL_MakeCurrent },
  { "SDL_GL_SetAttribute", (uintptr_t)&SDL_GL_SetAttribute },
  { "SDL_GL_SetSwapInterval", (uintptr_t)&SDL_GL_SetSwapInterval },
  { "SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow_fake },
  { "SDL_GL_UnloadLibrary", (uintptr_t)&SDL_GL_UnloadLibrary },
  { "SDL_GameControllerClose", (uintptr_t)&SDL_GameControllerClose },
  { "SDL_GameControllerGetJoystick", (uintptr_t)&SDL_GameControllerGetJoystick },
  { "SDL_GameControllerOpen", (uintptr_t)&SDL_GameControllerOpen },
  { "SDL_GetClipboardText", (uintptr_t)&SDL_GetClipboardText },
  { "SDL_GetCurrentAudioDriver", (uintptr_t)&SDL_GetCurrentAudioDriver },
  { "SDL_GetCurrentDisplayMode", (uintptr_t)&SDL_GetCurrentDisplayMode_fake },
  { "SDL_GetCurrentVideoDriver", (uintptr_t)&SDL_GetCurrentVideoDriver },
  { "SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode_fake },
  { "SDL_GetDisplayBounds", (uintptr_t)&SDL_GetDisplayBounds_fake },
  { "SDL_GetError", (uintptr_t)&SDL_GetError },
  { "SDL_GetKeyName", (uintptr_t)&SDL_GetKeyName },
  { "SDL_GetMouseState", (uintptr_t)&SDL_GetMouseState },
  { "SDL_GetNumVideoDisplays", (uintptr_t)&SDL_GetNumVideoDisplays },
  { "SDL_GetRevision", (uintptr_t)&SDL_GetRevision },
  { "SDL_GetVersion", (uintptr_t)&SDL_GetVersion },
  { "SDL_GetWindowGrab", (uintptr_t)&SDL_GetWindowGrab },
  { "SDL_GetWindowID", (uintptr_t)&SDL_GetWindowID },
  { "SDL_GetWindowSize", (uintptr_t)&SDL_GetWindowSize },
  { "SDL_GetWindowWMInfo", (uintptr_t)&SDL_GetWindowWMInfo_fake },
  { "SDL_HapticClose", (uintptr_t)&SDL_HapticClose_fake },
  { "SDL_HapticOpenFromJoystick", (uintptr_t)&SDL_HapticOpenFromJoystick_fake },
  { "SDL_HapticRumbleInit", (uintptr_t)&SDL_HapticRumbleInit_fake },
  { "SDL_HapticRumblePlay", (uintptr_t)&SDL_HapticRumblePlay_fake },
  { "SDL_HapticRumbleStop", (uintptr_t)&SDL_HapticRumbleStop_fake },
  { "SDL_HasClipboardText", (uintptr_t)&SDL_HasClipboardText },
  { "SDL_HideWindow", (uintptr_t)&SDL_HideWindow },
  { "SDL_Init", (uintptr_t)&SDL_Init },
  { "SDL_InitSubSystem", (uintptr_t)&SDL_InitSubSystem },
  { "SDL_IsGameController", (uintptr_t)&SDL_IsGameController },
  { "SDL_JoystickClose", (uintptr_t)&SDL_JoystickClose },
  { "SDL_JoystickGetDeviceGUID", (uintptr_t)&SDL_JoystickGetDeviceGUID },
  { "SDL_JoystickGetGUIDString", (uintptr_t)&SDL_JoystickGetGUIDString },
  { "SDL_JoystickInstanceID", (uintptr_t)&SDL_JoystickInstanceID },
  { "SDL_JoystickNameForIndex", (uintptr_t)&SDL_JoystickNameForIndex },
  { "SDL_JoystickOpen", (uintptr_t)&SDL_JoystickOpen },
  { "SDL_LoadBMP_RW", (uintptr_t)&SDL_LoadBMP_RW },
  { "SDL_NumJoysticks", (uintptr_t)&SDL_NumJoysticks },
  { "SDL_OpenAudioDevice", (uintptr_t)&SDL_OpenAudioDevice_fake },
  { "SDL_OpenURL", (uintptr_t)&SDL_OpenURL },
  { "SDL_PauseAudioDevice", (uintptr_t)&SDL_PauseAudioDevice },
  { "SDL_PollEvent", (uintptr_t)&SDL_PollEvent_fake },
  { "SDL_PushEvent", (uintptr_t)&SDL_PushEvent },
  { "SDL_QuitSubSystem", (uintptr_t)&SDL_QuitSubSystem },
  { "SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile },
  { "SDL_RaiseWindow", (uintptr_t)&SDL_RaiseWindow },
  { "SDL_SetClipboardText", (uintptr_t)&SDL_SetClipboardText },
  { "SDL_SetCursor", (uintptr_t)&SDL_SetCursor },
  { "SDL_SetHint", (uintptr_t)&SDL_SetHint_fake },
  { "SDL_SetRelativeMouseMode", (uintptr_t)&SDL_SetRelativeMouseMode },
  { "SDL_SetWindowBordered", (uintptr_t)&SDL_SetWindowBordered },
  { "SDL_SetWindowDisplayMode", (uintptr_t)&SDL_SetWindowDisplayMode_fake },
  { "SDL_SetWindowFullscreen", (uintptr_t)&SDL_SetWindowFullscreen_fake },
  { "SDL_SetWindowGammaRamp", (uintptr_t)&SDL_SetWindowGammaRamp_fake },
  { "SDL_SetWindowGrab", (uintptr_t)&SDL_SetWindowGrab },
  { "SDL_SetWindowIcon", (uintptr_t)&SDL_SetWindowIcon },
  { "SDL_SetWindowPosition", (uintptr_t)&SDL_SetWindowPosition },
  { "SDL_SetWindowSize", (uintptr_t)&SDL_SetWindowSize_fake },
  { "SDL_SetWindowTitle", (uintptr_t)&SDL_SetWindowTitle },
  { "SDL_ShowCursor", (uintptr_t)&SDL_ShowCursor },
  { "SDL_ShowMessageBox", (uintptr_t)&SDL_ShowMessageBox },
  { "SDL_ShowSimpleMessageBox", (uintptr_t)&SDL_ShowSimpleMessageBox },
  { "SDL_ShowWindow", (uintptr_t)&SDL_ShowWindow },
  { "SDL_StartTextInput", (uintptr_t)&SDL_StartTextInput_fake },
  { "SDL_WaitEventTimeout", (uintptr_t)&SDL_WaitEventTimeout },
  { "SDL_WarpMouseInWindow", (uintptr_t)&SDL_WarpMouseInWindow },
  { "SDL_WasInit", (uintptr_t)&SDL_WasInit },
  { "SDL_free", (uintptr_t)&SDL_free },
  { "SDL_memset", (uintptr_t)&SDL_memset },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);
