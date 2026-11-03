/* Android guests often bypass atexit() and call libnx's exit path directly. */
#include <switch.h>
#include "saltynx.h"

extern void NX_NORETURN __real___libnx_exit(int rc);

void NX_NORETURN __wrap___libnx_exit(int rc) {
    saltynx_stop();
    __real___libnx_exit(rc);
}
