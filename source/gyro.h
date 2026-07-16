/* gyro.h -- Switch gyroscope aiming: per-frame entry point.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __GYRO_H__
#define __GYRO_H__

/* config.gyro_space values: how raw angular velocity maps to look.
 *   LOCAL  -- axes fixed to the controller (default; no gravity needed)
 *   WORLD  -- yaw about the true vertical, using gravity (tilt-independent)
 *   PLAYER -- world-up yaw blended from yaw+roll (best feel; needs tuning) */
enum {
  GYRO_SPACE_LOCAL  = 0,
  GYRO_SPACE_WORLD  = 1,
  GYRO_SPACE_PLAYER = 2,
};

/* Read the gyro once and, if enabled and in-game, push an SDL_MOUSEMOTION.
 * Safe to call every frame; cheap no-op when disabled. */
void gyro_frame(void);

#endif
