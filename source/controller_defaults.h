#ifndef HL2_CONTROLLER_DEFAULTS_H
#define HL2_CONTROLLER_DEFAULTS_H

#define CONTROLLER_BINDING_COUNT 16

typedef struct {
  const char *button;
  const char *legacy;
  const char *action;
} ControllerBinding;

typedef enum {
  CONTROLLER_PROFILE_EMPTY,
  CONTROLLER_PROFILE_LEGACY,
  CONTROLLER_PROFILE_CUSTOM,
} ControllerProfile;

extern const ControllerBinding controller_bindings[CONTROLLER_BINDING_COUNT];
ControllerProfile controller_classify(const char *const bindings[CONTROLLER_BINDING_COUNT]);

#endif
