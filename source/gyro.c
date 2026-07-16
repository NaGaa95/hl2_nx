/* gyro.c -- Switch gyroscope -> relative mouse-look injection.
 *
 * Each frame, sums every new libnx SixAxis sample (framerate-independent: one
 * contribution per hardware sample), maps it to look rates in the configured
 * gyro space, converts to a relative-mouse delta, and pushes an SDL_MOUSEMOTION
 * into the shared SDL queue the engine already polls. Gated to in-game only via
 * SDL relative-mouse mode so the menu cursor is never disturbed.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <SDL2/SDL.h>
#include <math.h>

#include "gyro.h"
#include "gyro_math.h"
#include "config.h"
#include "util.h"

static HidSixAxisSensorHandle g_handles[2];
static int g_handle_count = 0;
static u32 g_style_active = 0;   /* style set the current handles were acquired for */
static u64 g_last_sample = 0;    /* newest SixAxis sample already consumed */
static GyroAccum g_accum;

static void stop_handles(void) {
  for (int i = 0; i < g_handle_count; i++)
    hidStopSixAxisSensor(g_handles[i]);
  g_handle_count = 0;
  g_style_active = 0;
  g_last_sample = 0;   /* a freshly started sensor restarts its sample counter */
}

static void acquire_handles(u32 style) {
  stop_handles();

  Result rc;
  if (style & HidNpadStyleTag_NpadHandheld) {
    rc = hidGetSixAxisSensorHandles(&g_handles[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    if (R_SUCCEEDED(rc))
      g_handle_count = 1;
  } else if (style & HidNpadStyleTag_NpadFullKey) {
    rc = hidGetSixAxisSensorHandles(&g_handles[0], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    if (R_SUCCEEDED(rc))
      g_handle_count = 1;
  } else if (style & HidNpadStyleTag_NpadJoyDual) {
    rc = hidGetSixAxisSensorHandles(&g_handles[0], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    if (R_SUCCEEDED(rc))
      g_handle_count = 2;   /* [0]=left, [1]=right; aim reads the right one */
  } else {
    debugPrintf("gyro: no supported controller style (0x%x)\n", style);
    g_style_active = style;   /* record attempted style so we don't re-log every frame */
    return;
  }

  if (g_handle_count == 0) {
    debugPrintf("gyro: failed to acquire SixAxis handles (style 0x%x)\n", style);
    g_style_active = style;   /* record attempted style so we don't re-log every frame */
    return;
  }

  for (int i = 0; i < g_handle_count; i++)
    hidStartSixAxisSensor(g_handles[i]);
  g_style_active = style;
}

/* Map one sample's angular velocity to (yaw, pitch) look rates per gyro space.
 * Controller axes: x = pitch (right), y = yaw (up), z = roll (forward). Signs
 * verified on-device so the un-inverted default is the natural direction. */
static void sample_to_look(const HidSixAxisSensorState *st, int space,
                           float *out_yaw, float *out_pitch) {
  const float ax = st->angular_velocity.x;
  const float ay = st->angular_velocity.y;
  const float az = st->angular_velocity.z;

  if (space == GYRO_SPACE_LOCAL) {
    *out_yaw   = -ay;
    *out_pitch = -ax;
    return;
  }

  /* world / player space: reorient using gravity from the accelerometer
   * (valid while not accelerating hard; ~1g points along device-down at rest) */
  float gx = st->acceleration.x;
  float gy = st->acceleration.y;
  float gz = st->acceleration.z;
  const float gl = sqrtf(gx * gx + gy * gy + gz * gz);
  if (gl > 0.0001f) { gx /= gl; gy /= gl; gz /= gl; }

  if (space == GYRO_SPACE_WORLD)
    *out_yaw = -(ax * gx + ay * gy + az * gz);   /* rotation about the true vertical */
  else /* GYRO_SPACE_PLAYER */
    *out_yaw = -(ay * gy + az * gz);             /* blend yaw + roll by gravity */

  *out_pitch = -ax;
}

void gyro_frame(void) {
  if (!config.gyro_enabled) {
    if (g_handle_count > 0)
      stop_handles();   /* release the sensor when the feature is turned off */
    return;
  }
  if (SDL_GetRelativeMouseMode() != SDL_TRUE)
    return;

  /* Detect the active controller's style WITHOUT reconfiguring global HID
   * (SDL owns that): read the style set directly, handheld first, else P1. */
  u32 style = hidGetNpadStyleSet(HidNpadIdType_Handheld);
  if (!style)
    style = hidGetNpadStyleSet(HidNpadIdType_No1);
  if (style != g_style_active)
    acquire_handles(style);
  if (g_handle_count == 0)
    return;

  const HidSixAxisSensorHandle h = g_handles[g_handle_count == 2 ? 1 : 0];
  HidSixAxisSensorState states[16];
  const int n = (int)hidGetSixAxisSensorStates(h, states, 16);   /* newest first */
  if (n < 1)
    return;

  /* Sum every sample not yet consumed. One contribution per hardware sample
   * makes the total turn framerate-independent (more samples per frame at lower
   * fps) and captures fast flicks the single-latest read would drop. */
  const int space = config.gyro_space;
  float sum_x = 0.0f, sum_y = 0.0f;
  int used = 0;
  u64 newest = g_last_sample;

  for (int i = 0; i < n; i++) {
    if (states[i].sampling_number <= g_last_sample)
      continue;
    if (states[i].sampling_number > newest)
      newest = states[i].sampling_number;

    float yaw, pitch;
    sample_to_look(&states[i], space, &yaw, &pitch);
    sum_x += yaw;
    sum_y += pitch;
    used++;
  }
  g_last_sample = newest;
  if (used == 0)
    return;

  /* average per-sample angular speed, for tightening / acceleration shaping */
  const float speed = sqrtf(sum_x * sum_x + sum_y * sum_y) / (float)used;

  int dx = 0, dy = 0;
  gyro_math_step(&g_accum, sum_x, sum_y, speed,
                 config.gyro_sensitivity_x, config.gyro_sensitivity_y,
                 config.gyro_invert_x, config.gyro_invert_y,
                 config.gyro_tighten, config.gyro_accel,
                 &dx, &dy);
  if (dx == 0 && dy == 0)
    return;

  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_MOUSEMOTION;
  e.motion.timestamp = SDL_GetTicks();
  e.motion.which = 0;              /* not SDL_TOUCH_MOUSEID, so SDL_PollEvent_fake keeps it */
  e.motion.state = 0;
  e.motion.xrel = dx;
  e.motion.yrel = dy;
  SDL_PushEvent(&e);
}
