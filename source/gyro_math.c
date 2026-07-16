/* gyro_math.c -- pure gyro conversion (no libnx/SDL deps; host-testable).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "gyro_math.h"

/* Counts of relative mouse motion per unit of angular velocity at 100%.
 * Tuned empirically on-device; per-axis sensitivity scales it. */
#define GYRO_BASE_SCALE 40.0f

void gyro_math_step(GyroAccum *acc,
                    float ang_yaw, float ang_pitch,
                    int sens_x, int sens_y, int invert_y,
                    int *out_dx, int *out_dy) {
  const float scale_x = GYRO_BASE_SCALE * (float)sens_x / 100.0f;
  const float scale_y = GYRO_BASE_SCALE * (float)sens_y / 100.0f;
  const float fx = acc->rem_x + ang_yaw * scale_x;
  const float fy = acc->rem_y + ang_pitch * scale_y * (invert_y ? -1.0f : 1.0f);

  const int dx = (int)fx;   /* truncates toward zero */
  const int dy = (int)fy;

  acc->rem_x = fx - (float)dx;   /* |remainder| < 1, sign preserved */
  acc->rem_y = fy - (float)dy;

  *out_dx = dx;
  *out_dy = dy;
}
