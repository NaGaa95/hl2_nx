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

/* Convert one frame of angular velocity into an integer relative-mouse delta.
 *
 *   ang_yaw     : angular velocity mapped to horizontal look
 *   ang_pitch   : angular velocity mapped to vertical look
 *   sensitivity : integer percent; 100 == base scale
 *   invert_y    : non-zero flips the vertical delta
 *   out_dx/dy   : integer mouse deltas (may be 0)
 *
 * The fractional part is accumulated in *acc across calls.
 */
void gyro_math_step(GyroAccum *acc,
                    float ang_yaw, float ang_pitch,
                    int sensitivity, int invert_y,
                    int *out_dx, int *out_dy);

#endif
