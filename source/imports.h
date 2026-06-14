/* imports.h -- .so import resolution
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stddef.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

// GL proc-address lookup with the present-blit hook layered in (see imports.c);
// dl_emu.c routes the engine's eglGetProcAddress through this.
void *eglGetProcAddress_fake(const char *name);

#endif
