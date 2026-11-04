#include "controller.h"
#include "controller_defaults.h"
#include "config.h"
#include "util.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

// Source's CCommand and ConCommand layouts, checked against the loaded engine.
typedef struct {
  int argc, argv0_size;
  char command[512], arguments[512];
  const char *argv[64];
} SourceCommand;

typedef struct {
  uintptr_t vtable, next;
  uint8_t registered, padding[7];
  const char *name, *help;
  int flags;
  uint32_t padding2;
  void (*callback)(const SourceCommand *);
  uintptr_t completion;
  uint8_t callback_flags;
} SourceConCommand;

_Static_assert(offsetof(SourceCommand, argv) == 1032, "CCommand argv layout");
_Static_assert(offsetof(SourceConCommand, callback) == 48, "ConCommand callback layout");
_Static_assert(offsetof(SourceConCommand, callback_flags) == 64, "ConCommand callback flags");
_Static_assert(sizeof(SourceConCommand) == 72, "ConCommand size");

static const struct {
  const char *name, *value;
} axis_defaults[] = {
  { "joy_advanced", "1" },
  { "joy_advaxisx", "3" },
  { "joy_advaxisy", "1" },
  { "joy_advaxisz", "0" },
  { "joy_advaxisr", "2" },
  { "joy_advaxisu", "4" },
  { "joy_advaxisv", "0" },
};

static struct {
  int ready, initialized, autoexec_depth;
  SourceConCommand *exec;
  void (*original_exec)(const SourceCommand *);
  int (*button_code)(const char *, int);
  const char *(*get_binding)(int);
  void (*set_binding)(int, const char *);
  void (*make_command)(SourceCommand *, int, const char **);
  const void *(*execute_command)(const SourceCommand *, int, int);
  int codes[CONTROLLER_BINDING_COUNT];
} controller;

static uintptr_t runtime_symbol(so_module *module, const char *name, size_t *size) {
  const uintptr_t address = so_find_addr_with_size(module, name, size);
  const uintptr_t base = (uintptr_t)module->load_base;
  if (!address || address < base || address - base >= module->load_size)
    return 0;
  return (uintptr_t)module->load_virtbase + address - base;
}

static int command_layout_matches(so_module *engine) {
  size_t size = 0;
  const uint32_t *code = (const uint32_t *)so_find_addr_with_size(
      engine, "_Z10Cmd_Exec_fRK8CCommand", &size);
  int argc_matches = 0, argv_matches = 0;
  if (!code)
    return 0;
  for (size_t i = 0; i < size / sizeof(*code); i++) {
    // ldr w8, [x19]; cmp w8, #2
    if (i + 1 < size / sizeof(*code) && code[i] == 0xb9400268 && code[i + 1] == 0x7100091f)
      argc_matches++;
    // ldr x20, [x19, #1040] (argv[1])
    if (code[i] == 0xf9420a74)
      argv_matches++;
  }
  return argc_matches == 1 && argv_matches == 1;
}

static int script_is(const char *filename, const char *stem) {
  const size_t length = strlen(stem);
  return !strncasecmp(filename, stem, length) &&
         (!filename[length] || !strcasecmp(filename + length, ".cfg"));
}

static void bind_defaults(void) {
  for (int i = 0; i < CONTROLLER_BINDING_COUNT; i++)
    controller.set_binding(controller.codes[i], controller_bindings[i].action);
}

static void execute_setting(const char *name, const char *value) {
  const char *argv[] = { name, value };
  SourceCommand command = {0};
  controller.make_command(&command, value ? 2 : 1, argv);
  // Use the registered client commands, including stripped episodic modules.
  controller.execute_command(&command, 1 /* src_command */, -1);
}

static void initialize_defaults(void) {
  if (controller.initialized)
    return;
  controller.initialized = 1;

  const char *bindings[CONTROLLER_BINDING_COUNT];
  for (int i = 0; i < CONTROLLER_BINDING_COUNT; i++)
    bindings[i] = controller.get_binding(controller.codes[i]);
  const ControllerProfile profile = controller_classify(bindings);

  if (profile == CONTROLLER_PROFILE_CUSTOM)
    return;
  bind_defaults();
  if (profile == CONTROLLER_PROFILE_EMPTY) {
    // New profiles also need the Source/SDL stick axes and vertical look.
    // Existing profiles retain their axis settings and sensitivity.
    for (size_t i = 0; i < sizeof(axis_defaults) / sizeof(*axis_defaults); i++)
      execute_setting(axis_defaults[i].name, axis_defaults[i].value);
    execute_setting("+jlook", NULL);
    execute_setting("joyadvancedupdate", NULL);
  }
}

static void controller_exec(const SourceCommand *args) {
  const char *filename = args && args->argc == 2 ? args->argv[1] : NULL;
  if (!filename) {
    controller.original_exec(args);
    return;
  }

  if (script_is(filename, "switch_controller")) {
    initialize_defaults();
    bind_defaults();
    return;
  }

  // The engine can queue these after autoexec on its first joystick update.
  // Explicit executions from inside autoexec are still the user's choice.
  if (!controller.autoexec_depth &&
      (script_is(filename, "360controller") || script_is(filename, "360controller-linux")))
    return;

  const int autoexec = script_is(filename, "autoexec");
  if (autoexec || script_is(filename, "joystick"))
    initialize_defaults();
  if (autoexec)
    controller.autoexec_depth++;
  controller.original_exec(args);
  if (autoexec)
    controller.autoexec_depth--;

  // Apply only after all saved binds have been read, before valve.rc/autoexec.
  if (script_is(filename, "config") || script_is(filename, "config_default"))
    initialize_defaults();
}

void controller_prepare(so_module *engine, so_module *inputsystem) {
  memset(&controller, 0, sizeof(controller));
  if (!engine || !inputsystem ||
      (strcmp(config.gamedir, "hl2") && strcmp(config.gamedir, "episodic") && strcmp(config.gamedir, "ep2")))
    return;
  if (!command_layout_matches(engine))
    return;

  size_t size = 0;
  controller.exec = (void *)runtime_symbol(engine, "_ZL12exec_command", &size);
  if (size != sizeof(SourceConCommand))
    return;
  controller.original_exec = (void *)runtime_symbol(engine, "_Z10Cmd_Exec_fRK8CCommand", NULL);
  controller.get_binding = (void *)runtime_symbol(engine, "_Z17Key_BindingForKey12ButtonCode_t", NULL);
  controller.set_binding = (void *)runtime_symbol(engine, "_Z14Key_SetBinding12ButtonCode_tPKc", NULL);
  controller.button_code = (void *)runtime_symbol(inputsystem, "_Z29ButtonCode_StringToButtonCodePKcb", NULL);
  controller.make_command = (void *)runtime_symbol(engine, "_ZN8CCommandC1EiPPKc", NULL);
  controller.execute_command = (void *)runtime_symbol(engine, "_Z18Cmd_ExecuteCommandRK8CCommand12cmd_source_ti", NULL);
  if (!controller.exec || !controller.original_exec || !controller.get_binding ||
      !controller.set_binding || !controller.button_code || !controller.make_command ||
      !controller.execute_command)
    return;
  controller.ready = 1;
}

void controller_install(void) {
  if (!controller.ready || !controller.exec->name || strcmp(controller.exec->name, "exec") ||
      controller.exec->callback != controller.original_exec ||
      (controller.exec->callback_flags & 6) != 2)
    return;

  for (int i = 0; i < CONTROLLER_BINDING_COUNT; i++) {
    const int code = controller.button_code(controller_bindings[i].button, 1);
    if (code <= 0 || code >= 512)
      return;
    for (int j = 0; j < i; j++)
      if (controller.codes[j] == code)
        return;
    controller.codes[i] = code;
  }

  // Replace only the initialized command callback, preserving the original
  // engine function and its normal script parsing/execution behavior.
  controller.exec->callback = controller_exec;
  debugPrintf("Controller defaults: installed HL2 preset\n");
}
