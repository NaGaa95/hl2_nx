/* gyro_math.h -- pure gyro angular-velocity -> relative-mouse conversion.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __GYRO_MATH_H__
#define __GYRO_MATH_H__

/* Carried sub-count remainder so slow motion is not lost between frames. */
typedef struct {
  float rem_x;
  float rem_y;
} GyroAccum;

/* Convert one frame's worth of summed angular velocity into an integer
 * relative-mouse delta.
 *
 *   sum_x/sum_y   : sum of the frame's per-sample look rates (already
 *                   framerate-independent: one contribution per hardware
 *                   sample). sum_x = horizontal, sum_y = vertical.
 *   speed         : average per-sample angular speed this frame, used by the
 *                   tightening and acceleration shaping (raw libnx units)
 *   sens_x/sens_y : integer percent per axis; 100 == base scale
 *   invert_y      : non-zero flips the vertical delta
 *   tighten       : non-zero tapers tiny movements toward zero (anti-drift)
 *   accel         : non-zero applies a speed-based acceleration multiplier
 *   out_dx/dy     : integer mouse deltas (may be 0)
 *
 * The fractional part is accumulated in *acc across calls.
 */
void gyro_math_step(GyroAccum *acc,
                    float sum_x, float sum_y, float speed,
                    int sens_x, int sens_y, int invert_y,
                    int tighten, int accel,
                    int *out_dx, int *out_dy);

#endif
