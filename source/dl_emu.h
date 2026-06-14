/* dl_emu.h -- dlopen/dlsym/dlclose/dlerror/dladdr over the so_util loader
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __DL_EMU_H__
#define __DL_EMU_H__

void *dlopen_fake(const char *filename, int flags);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);
char *dlerror_fake(void);

// bionic Dl_info layout
typedef struct {
  const char *dli_fname;
  void *dli_fbase;
  const char *dli_sname;
  void *dli_saddr;
} DlInfoFake;

int dladdr_fake(const void *addr, DlInfoFake *info);

#endif
