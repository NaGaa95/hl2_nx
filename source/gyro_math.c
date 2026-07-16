/* gyro_math.c -- pure gyro conversion (no libnx/SDL deps; host-testable).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "gyro_math.h"

/* Overall gain: mouse counts per unit of the frame's summed per-sample angular
 * velocity at 100%. Re-tuned for multi-sample integration (each hardware sample
 * contributes once, so this is much smaller than the old per-frame scale).
 * Tune on-device via the per-axis sensitivity. */
#define GYRO_BASE_SCALE 10.0f

/* Tightening ("anti-drift"): below this average angular speed the output is
 * smoothly tapered toward zero to swallow sensor noise / hand tremor. In raw
 * libnx angular-velocity units -- tune on-device. */
#define GYRO_TIGHTEN_THRESHOLD 0.08f

/* Acceleration: an extra multiplier ramped linearly from 1x at LOW speed to
 * MAX at HIGH speed. In raw libnx angular-velocity units -- tune on-device. */
#define GYRO_ACCEL_LOW  0.5f
#define GYRO_ACCEL_HIGH 5.0f
#define GYRO_ACCEL_MAX  2.0f

/* smoothstep on [0,1], clamped */
static float smoothstep01(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

void gyro_math_step(GyroAccum *acc,
                    float sum_x, float sum_y, float speed,
                    int sens_x, int sens_y, int invert_y,
                    int tighten, int accel,
                    int *out_dx, int *out_dy) {
  float mult = 1.0f;

  if (tighten)
    mult *= smoothstep01(speed / GYRO_TIGHTEN_THRESHOLD);

  if (accel) {
    float a;
    if (speed <= GYRO_ACCEL_LOW)
      a = 1.0f;
    else if (speed >= GYRO_ACCEL_HIGH)
      a = GYRO_ACCEL_MAX;
    else
      a = 1.0f + (GYRO_ACCEL_MAX - 1.0f) *
          (speed - GYRO_ACCEL_LOW) / (GYRO_ACCEL_HIGH - GYRO_ACCEL_LOW);
    mult *= a;
  }

  const float kx = GYRO_BASE_SCALE * mult * (float)sens_x / 100.0f;
  const float ky = GYRO_BASE_SCALE * mult * (float)sens_y / 100.0f
                   * (invert_y ? -1.0f : 1.0f);

  const float fx = acc->rem_x + sum_x * kx;
  const float fy = acc->rem_y + sum_y * ky;

  const int dx = (int)fx;   /* truncates toward zero */
  const int dy = (int)fy;

  acc->rem_x = fx - (float)dx;   /* |remainder| < 1, sign preserved */
  acc->rem_y = fy - (float)dy;

  *out_dx = dx;
  *out_dy = dy;
}
