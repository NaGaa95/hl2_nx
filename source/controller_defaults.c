#include "controller_defaults.h"
#include <string.h>

// Source uses SDL's positional names: Switch B/A/Y/X become A/B/X/Y_BUTTON.
const ControllerBinding controller_bindings[CONTROLLER_BINDING_COUNT] = {
  { "A_BUTTON",    "+jump",         "+jump" },
  { "B_BUTTON",    "toggle_duck",   "+reload" },
  { "X_BUTTON",    "+use",          "+use" },
  { "Y_BUTTON",    "togglescores",  "impulse 100" },
  { "L_SHOULDER",  "+jump",         "+speed" },
  { "R_SHOULDER",  "+use",          "phys_swap" },
  { "BACK",        NULL,            "impulse 50" },
  { "START",       "pause",         "pause" },
  { "STICK1",      "voicemenu 0 0", "toggle_duck" },
  { "STICK2",      "+duck",         "+zoom" },
  { "UP",          "slot1",         "slot1" },
  { "RIGHT",       "slot2",         "slot2" },
  { "DOWN",        "slot3",         "slot3" },
  { "LEFT",        "slot4",         "slot4" },
  { "L_TRIGGER",   "+attack2",      "+attack2" },
  { "R_TRIGGER",   "+attack",       "+attack" },
};

ControllerProfile controller_classify(const char *const bindings[CONTROLLER_BINDING_COUNT]) {
  int empty = 1, legacy = 1;
  for (int i = 0; i < CONTROLLER_BINDING_COUNT; i++) {
    const char *value = bindings[i] ? bindings[i] : "";
    const char *old = controller_bindings[i].legacy;
    if (*value)
      empty = 0;
    if (old) {
      if (strcmp(value, old))
        legacy = 0;
    } else if (*value && strcmp(value, controller_bindings[i].action)) {
      // Some stock profiles retain the base preset's squad-order binding.
      legacy = 0;
    }
  }
  return empty ? CONTROLLER_PROFILE_EMPTY :
         legacy ? CONTROLLER_PROFILE_LEGACY : CONTROLLER_PROFILE_CUSTOM;
}
