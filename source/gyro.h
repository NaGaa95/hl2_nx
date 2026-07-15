/* gyro.h -- Switch gyroscope aiming: per-frame entry point.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __GYRO_H__
#define __GYRO_H__

/* Read the gyro once and, if enabled and in-game, push an SDL_MOUSEMOTION.
 * Safe to call every frame; cheap no-op when disabled. */
void gyro_frame(void);

#endif
