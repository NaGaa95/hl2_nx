/* video_player.c -- fullscreen startup video playback via ffmpeg
 *
 * The Android build ships no video codec module (the engine probes video_bink
 * / video_webm and finds neither), so the Valve intro logo never plays. Rather
 * than reverse the engine's IVideoSubSystem ABI, we decode the startup videos
 * ourselves with switch-ffmpeg (its Bink decoder is built in) and present them
 * with GLES + SDL audio, right after the GL context exists and before the
 * engine draws its first frame. The engine then continues straight to the menu.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "video_player.h"
#include "config.h"
#include "util.h"

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif

// set while the player owns the screen, so main.c's clock manager leaves the CPU
// boosted during the intro instead of treating these frames as gameplay
volatile int g_video_playing = 0;

// ---- GLES present: a fullscreen triangle sampling the decoded RGBA frame ----
static const char *VID_VSRC =
    "#version 300 es\n"
    "out vec2 v_uv;\n"
    "void main(){\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  v_uv = vec2(p.x, 1.0 - p.y);\n" // ffmpeg row 0 = top, GL tex 0 = bottom
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *VID_FSRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv; uniform sampler2D u_tex; out vec4 o;\n"
    "void main(){ o = texture(u_tex, v_uv); }\n"; // frame is already display-encoded

static GLuint vid_compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    debugPrintf("video: shader compile failed: %s\n", log);
  }
  return s;
}

typedef struct {
  GLuint prog, tex, vao;
  int tex_w, tex_h;
} VidGL;

static int vidgl_init(VidGL *g, int w, int h) {
  memset(g, 0, sizeof(*g));
  GLuint vs = vid_compile(GL_VERTEX_SHADER, VID_VSRC);
  GLuint fs = vid_compile(GL_FRAGMENT_SHADER, VID_FSRC);
  g->prog = glCreateProgram();
  glAttachShader(g->prog, vs);
  glAttachShader(g->prog, fs);
  glLinkProgram(g->prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(g->prog, GL_LINK_STATUS, &linked);
  if (!linked) { debugPrintf("video: program link failed\n"); return -1; }

  glGenTextures(1, &g->tex);
  glBindTexture(GL_TEXTURE_2D, g->tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  g->tex_w = w;
  g->tex_h = h;
  glGenVertexArrays(1, &g->vao);
  return 0;
}

static void vidgl_free(VidGL *g) {
  if (g->vao) glDeleteVertexArrays(1, &g->vao);
  if (g->tex) glDeleteTextures(1, &g->tex);
  if (g->prog) glDeleteProgram(g->prog);
  glUseProgram(0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

static void vidgl_present(VidGL *g, SDL_Window *win, const uint8_t *rgba) {
  int ww = 0, wh = 0;
  SDL_GetWindowSize(win, &ww, &wh);
  if (ww <= 0 || wh <= 0) { ww = screen_width; wh = screen_height; }

  // aspect-preserving letterbox of the video inside the window
  int vx = 0, vy = 0, vw = ww, vh = wh;
  if (g->tex_w > 0 && g->tex_h > 0) {
    double ar = (double)g->tex_w / (double)g->tex_h;
    double sar = (double)ww / (double)wh;
    if (sar > ar) { vh = wh; vw = (int)(wh * ar + 0.5); vx = (ww - vw) / 2; }
    else          { vw = ww; vh = (int)(ww / ar + 0.5); vy = (wh - vh) / 2; }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
#ifdef GL_FRAMEBUFFER_SRGB
  glDisable(GL_FRAMEBUFFER_SRGB); // frame bytes are already sRGB; copy verbatim
#endif

  glViewport(0, 0, ww, wh);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(vx, vy, vw, vh);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g->tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g->tex_w, g->tex_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

  glUseProgram(g->prog);
  glUniform1i(glGetUniformLocation(g->prog, "u_tex"), 0);
  glBindVertexArray(g->vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);

  SDL_GL_SwapWindow(win);
}

// Skip is disabled: playback runs on a worker thread, and SDL events may only
// be pumped from the main thread (the one that initialised video). The intro is
// short, so it simply plays to completion.
static int vid_poll_skip(void) { return 0; }

// ---- audio --------------------------------------------------------------
// Don't use SDL audio here: switch-sdl2's audren backend can't be cleanly
// re-opened after an open/close cycle, so opening an SDL device for the intro
// mutes the engine's own audio (opened ~15s later). We play the intro sound
// through libnx audout instead -- a separate service, torn down before the
// engine touches audio, so the two never overlap.
//
// The whole (short) audio track is pre-decoded to 48kHz S16 stereo in one pass
// over a private file handle, then handed to audout while the video renders.

#define AUDOUT_RATE 48000
#define AUDOUT_CH   2

static uint8_t *decode_all_audio(const char *path, int *streams_audio, size_t *out_size) {
  *out_size = 0;
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return NULL;
  if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return NULL; }
  int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (as < 0) { *streams_audio = 0; avformat_close_input(&fmt); return NULL; }
  *streams_audio = 1;

  const AVCodec *adec = avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id);
  AVCodecContext *actx = adec ? avcodec_alloc_context3(adec) : NULL;
  int aok = actx && avcodec_parameters_to_context(actx, fmt->streams[as]->codecpar) >= 0;
  if (aok) { actx->thread_count = 1; actx->thread_type = 0; } // see note in play_one
  if (!aok || avcodec_open2(actx, adec, NULL) < 0) {
    if (actx) avcodec_free_context(&actx);
    avformat_close_input(&fmt);
    return NULL;
  }

  AVChannelLayout out_layout;
  av_channel_layout_default(&out_layout, AUDOUT_CH);
  struct SwrContext *swr = NULL;
  if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, AUDOUT_RATE,
                          &actx->ch_layout, actx->sample_fmt, actx->sample_rate,
                          0, NULL) < 0 || swr_init(swr) < 0) {
    if (swr) swr_free(&swr);
    avcodec_free_context(&actx);
    avformat_close_input(&fmt);
    return NULL;
  }

  uint8_t *buf = NULL;
  size_t cap = 0, len = 0;
  const size_t CAP_MAX = 24u * 1024 * 1024; // ~2 min of audio; startup vids are short
  AVPacket *pkt = av_packet_alloc();
  AVFrame *fr = av_frame_alloc();
  int oom = 0;
  while (!oom && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == as && avcodec_send_packet(actx, pkt) == 0) {
      while (avcodec_receive_frame(actx, fr) == 0) {
        int outs = swr_get_out_samples(swr, fr->nb_samples);
        if (outs <= 0) continue;
        size_t need = (size_t)outs * AUDOUT_CH * 2;
        if (len + need > cap) {
          size_t ncap = (len + need) * 2 + 0x10000;
          if (ncap > CAP_MAX) { oom = 1; break; }
          uint8_t *nb = (uint8_t *)realloc(buf, ncap);
          if (!nb) { oom = 1; break; }
          buf = nb; cap = ncap;
        }
        uint8_t *op[1] = { buf + len };
        int n = swr_convert(swr, op, outs, (const uint8_t **)fr->extended_data, fr->nb_samples);
        if (n > 0) len += (size_t)n * AUDOUT_CH * 2;
      }
    }
    av_packet_unref(pkt);
  }

  av_frame_free(&fr);
  av_packet_free(&pkt);
  swr_free(&swr);
  avcodec_free_context(&actx);
  avformat_close_input(&fmt);

  if (len == 0) { free(buf); return NULL; }
  *out_size = len;
  return buf;
}

// audout playback of one pre-decoded PCM buffer
static int g_audout_on;
static void *g_audout_buf;
static AudioOutBuffer g_audout_aob;

static void audout_play(const uint8_t *pcm, size_t size) {
  if (!pcm || size == 0) return;
  if (R_FAILED(audoutInitialize())) { debugPrintf("video: audoutInitialize failed\n"); return; }
  if (R_FAILED(audoutStartAudioOut())) { audoutExit(); return; }
  const size_t bufsz = (size + 0xFFF) & ~(size_t)0xFFF; // audout needs 0x1000 align
  g_audout_buf = memalign(0x1000, bufsz);
  if (!g_audout_buf) { audoutStopAudioOut(); audoutExit(); return; }
  memcpy(g_audout_buf, pcm, size);
  if (bufsz > size) memset((uint8_t *)g_audout_buf + size, 0, bufsz - size);
  memset(&g_audout_aob, 0, sizeof(g_audout_aob));
  g_audout_aob.buffer = g_audout_buf;
  g_audout_aob.buffer_size = bufsz;
  g_audout_aob.data_size = size;
  g_audout_aob.data_offset = 0;
  if (R_FAILED(audoutAppendAudioOutBuffer(&g_audout_aob))) {
    audoutStopAudioOut(); audoutExit();
    free(g_audout_buf); g_audout_buf = NULL;
    return;
  }
  g_audout_on = 1;
}

static void audout_stop(void) {
  if (g_audout_on) {
    audoutStopAudioOut();
    audoutExit();
    g_audout_on = 0;
  }
  free(g_audout_buf);
  g_audout_buf = NULL;
}

// ---- play one file --------------------------------------------------------
static int play_one(SDL_Window *win, const char *path) {
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return -1;
  if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return -1; }

  int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (vs < 0) { avformat_close_input(&fmt); return -1; }
  int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

  const AVCodec *vdec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
  AVCodecContext *vctx = vdec ? avcodec_alloc_context3(vdec) : NULL;
  int vok = vctx && avcodec_parameters_to_context(vctx, fmt->streams[vs]->codecpar) >= 0;
  if (vok) {
    // must stay single-threaded: ffmpeg's decoder worker threads consume the
    // Switch's scarce TLS slots, which starves the engine's audio thread (created
    // later) and silences the game. Decode on this thread only.
    vctx->thread_count = 1;
    vctx->thread_type = 0;
  }
  if (!vok || avcodec_open2(vctx, vdec, NULL) < 0) {
    debugPrintf("video: no video decoder for %s\n", path);
    if (vctx) avcodec_free_context(&vctx);
    avformat_close_input(&fmt);
    return -1;
  }

  // pre-decode the whole audio track up front (its own file handle, so the
  // video format context below stays at the start). played via libnx audout,
  // never SDL -- see the audio section comment above.
  size_t audio_size = 0;
  int has_audio = 0;
  uint8_t *audio_pcm = (as >= 0) ? decode_all_audio(path, &has_audio, &audio_size) : NULL;

  struct SwsContext *sws =
      sws_getContext(vctx->width, vctx->height, vctx->pix_fmt,
                     vctx->width, vctx->height, AV_PIX_FMT_RGBA,
                     SWS_BILINEAR, NULL, NULL, NULL);
  VidGL gl;
  uint8_t *rgba = (uint8_t *)malloc((size_t)vctx->width * vctx->height * 4);
  if (!sws || !rgba || vidgl_init(&gl, vctx->width, vctx->height) < 0) {
    debugPrintf("video: GL/sws setup failed\n");
    free(rgba);
    free(audio_pcm);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&vctx);
    avformat_close_input(&fmt);
    return -1;
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *fr = av_frame_alloc();
  const AVRational vtb = fmt->streams[vs]->time_base;
  int skipped = 0;

  // start the audio and pace the video off the same instant for A/V sync
  audout_play(audio_pcm, audio_size);
  free(audio_pcm);
  audio_pcm = NULL;
  const Uint64 start_ms = SDL_GetTicks64();

  while (!skipped && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vs) {
      if (avcodec_send_packet(vctx, pkt) == 0) {
        while (avcodec_receive_frame(vctx, fr) == 0) {
          uint8_t *dst[1] = { rgba };
          int dst_stride[1] = { vctx->width * 4 };
          sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0,
                    vctx->height, dst, dst_stride);

          int64_t ts = fr->best_effort_timestamp;
          double pts = (ts == AV_NOPTS_VALUE ? 0.0 : (double)ts * av_q2d(vtb));

          // pace to PTS, capped so a bogus timestamp can't stall the boot
          Uint64 wait_start = SDL_GetTicks64();
          for (;;) {
            Uint64 elapsed = SDL_GetTicks64() - start_ms;
            if ((double)elapsed >= pts * 1000.0) break;
            if (SDL_GetTicks64() - wait_start > 1000) break;
            if (vid_poll_skip()) { skipped = 1; break; }
            SDL_Delay(2);
          }
          if (skipped) break;

          vidgl_present(&gl, win, rgba);
          if (vid_poll_skip()) { skipped = 1; break; }
        }
      }
    }
    av_packet_unref(pkt);
  }

  av_frame_free(&fr);
  av_packet_free(&pkt);
  vidgl_free(&gl);
  free(rgba);
  sws_freeContext(sws);
  audout_stop();
  avcodec_free_context(&vctx);
  avformat_close_input(&fmt);
  return skipped ? 1 : 0;
}

// resolve a startupvids entry (e.g. "media/valve.avi") to a file that actually
// exists, trying the codecs we can decode; play it if found
static int resolve_and_play(SDL_Window *win, const char *entry) {
  // strip any extension from the entry's basename
  char base[640];
  snprintf(base, sizeof(base), "%s/%s/%s", config.install_root, config.gamedir, entry);
  char *dot = strrchr(base, '.');
  char *slash = strrchr(base, '/');
  if (dot && (!slash || dot > slash)) *dot = '\0';

  static const char *const exts[] = { ".bik", ".webm", ".ogv", ".ogg",
                                      ".mp4", ".mov", ".avi", ".m4v" };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(exts) / sizeof(*exts); i++) {
    snprintf(path, sizeof(path), "%s%s", base, exts[i]);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
      return play_one(win, path);
  }
  return -1; // nothing playable for this entry
}

// the actual playback (runs on the worker thread, GL context already current)
static void run_startup_videos(SDL_Window *win) {
  av_log_set_level(AV_LOG_FATAL);

  char list[400];
  snprintf(list, sizeof(list), "%s/%s/media/startupvids.txt",
           config.install_root, config.gamedir);

  FILE *f = fopen(list, "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      // trim whitespace/CR
      char *s = line;
      while (*s == ' ' || *s == '\t') s++;
      char *e = s + strlen(s);
      while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
      if (*s == '\0' || *s == '/' || *s == '#') continue;
      if (resolve_and_play(win, s) == 1) break; // user skipped -> stop the rest
    }
    fclose(f);
  } else {
    // no list: just try the stock Valve logo
    resolve_and_play(win, "media/valve.avi");
  }
}

// The intro must not decode on the engine's main thread: that thread has the
// engine's bionic TLS hijacked onto TPIDR_EL0 (the stack-guard the port relies
// on), and ffmpeg's newlib code clobbers it, which mutes the game. So we hand
// the GL context to a dedicated newlib pthread, run everything there, and give
// the context back to the main thread before the engine continues.
typedef struct {
  SDL_Window *win;
  SDL_GLContext ctx;
} VidThreadArg;

static void *video_thread(void *p) {
  VidThreadArg *a = (VidThreadArg *)p;
  if (SDL_GL_MakeCurrent(a->win, a->ctx) != 0) {
    debugPrintf("video: worker SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
    return NULL;
  }
  run_startup_videos(a->win);
  SDL_GL_MakeCurrent(a->win, NULL); // release so the main thread can re-acquire
  return NULL;
}

void play_startup_videos(SDL_Window *win, SDL_GLContext ctx) {
  if (!win) return;

  g_video_playing = 1;
  SDL_GL_MakeCurrent(win, NULL); // release the context from the main thread

  VidThreadArg arg = { win, ctx };
  pthread_t t;
  if (pthread_create(&t, NULL, video_thread, &arg) == 0)
    pthread_join(t, NULL);
  else
    debugPrintf("video: worker thread create failed; skipping intro\n");

  // ALWAYS restore the context on the main thread, even if the intro failed,
  // so the engine keeps a working GL context
  if (SDL_GL_MakeCurrent(win, ctx) != 0)
    debugPrintf("video: restore SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
  g_video_playing = 0;
}
