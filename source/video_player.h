/* video_player.h -- fullscreen startup video playback (ffmpeg)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __VIDEO_PLAYER_H__
#define __VIDEO_PLAYER_H__

#include <SDL2/SDL.h>

// Play the game's startup videos (the Valve logo, etc.) fullscreen, blocking
// until done. Decoding runs on a dedicated worker thread (the GL context is
// temporarily handed to it) because ffmpeg must not run on the engine's
// TLS-hijacked main thread. Pass the window and its GL context (currently
// current on the calling/main thread); on return the context is current on the
// caller again. Silently does nothing if disabled / no playable file.
void play_startup_videos(SDL_Window *win, SDL_GLContext ctx);

#endif
