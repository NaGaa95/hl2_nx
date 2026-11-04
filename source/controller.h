#ifndef HL2_CONTROLLER_H
#define HL2_CONTROLLER_H

#include "so_util.h"

// Resolve local symbols while the original ELF metadata is available.
void controller_prepare(so_module *engine, so_module *inputsystem);
// Install the callback after the modules' static constructors have run.
void controller_install(void);

#endif
