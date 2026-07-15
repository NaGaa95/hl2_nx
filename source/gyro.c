/* gyro.c -- Switch gyroscope -> relative mouse-look injection.
 *
 * Reads the active controller's angular velocity via libnx SixAxis once per
 * frame, converts it to a relative-mouse delta, and pushes an SDL_MOUSEMOTION
 * into the shared SDL queue the engine already polls. Gated to in-game only
 * via SDL relative-mouse mode so the menu cursor is never disturbed.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <SDL2/SDL.h>
#include <stdbool.h>

#include "gyro.h"
#include "gyro_math.h"
#include "config.h"
#include "util.h"

static bool g_pad_ready = false;
static PadState g_pad;
static HidSixAxisSensorHandle g_handles[2];
static int g_handle_count = 0;
static u32 g_style_active = 0;   /* style set the current handles were acquired for */
static GyroAccum g_accum;

static void acquire_handles(u32 style) {
  for (int i = 0; i < g_handle_count; i++)
    hidStopSixAxisSensor(g_handles[i]);
  g_handle_count = 0;
  g_style_active = 0;

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

void gyro_frame(void) {
  if (!config.gyro_enabled)
    return;
  if (SDL_GetRelativeMouseMode() != SDL_TRUE)
    return;

  if (!g_pad_ready) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    g_accum.rem_x = 0.0f;
    g_accum.rem_y = 0.0f;
    g_pad_ready = true;
  }

  padUpdate(&g_pad);
  const u32 style = padGetStyleSet(&g_pad);
  if (style != g_style_active)
    acquire_handles(style);
  if (g_handle_count == 0)
    return;

  const HidSixAxisSensorHandle h = g_handles[g_handle_count == 2 ? 1 : 0];
  HidSixAxisSensorState st = {0};
  if (hidGetSixAxisSensorStates(h, &st, 1) < 1)
    return;

  /* Local-space mapping. Signs are finalized on-device (see plan verify step):
   *   yaw   <- rotation about the controller's vertical axis   (angular_velocity.y)
   *   pitch <- rotation about the controller's left-right axis (angular_velocity.x) */
  const float ang_yaw   = -st.angular_velocity.y;
  const float ang_pitch =  st.angular_velocity.x;

  int dx = 0, dy = 0;
  gyro_math_step(&g_accum, ang_yaw, ang_pitch,
                 config.gyro_sensitivity, config.gyro_invert_y, &dx, &dy);
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
