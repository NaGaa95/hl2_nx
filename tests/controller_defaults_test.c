#include "../source/controller_defaults.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char *bindings[CONTROLLER_BINDING_COUNT] = {0};
  assert(controller_classify(bindings) == CONTROLLER_PROFILE_EMPTY);
  for (int i = 0; i < CONTROLLER_BINDING_COUNT; i++)
    bindings[i] = "";
  assert(controller_classify(bindings) == CONTROLLER_PROFILE_EMPTY);

  for (int i = 0; i < CONTROLLER_BINDING_COUNT; i++)
    bindings[i] = controller_bindings[i].legacy;
  assert(controller_classify(bindings) == CONTROLLER_PROFILE_LEGACY);
  bindings[6] = "impulse 50";
  assert(controller_classify(bindings) == CONTROLLER_PROFILE_LEGACY);

  // A single custom button protects the entire saved profile.
  for (int i = 0; i < CONTROLLER_BINDING_COUNT; i++) {
    const char *old = bindings[i];
    bindings[i] = "say custom";
    assert(controller_classify(bindings) == CONTROLLER_PROFILE_CUSTOM);
    if (i != 6) {
      bindings[i] = "";
      assert(controller_classify(bindings) == CONTROLLER_PROFILE_CUSTOM);
    }
    bindings[i] = old;
  }

  const char *expected[] = {
    "+jump", "+reload", "+use", "impulse 100", "+speed", "phys_swap",
    "impulse 50", "pause", "toggle_duck", "+zoom", "slot1", "slot2",
    "slot3", "slot4", "+attack2", "+attack",
  };
  for (int i = 0; i < CONTROLLER_BINDING_COUNT; i++) {
    assert(!strcmp(controller_bindings[i].action, expected[i]));
    bindings[i] = expected[i];
  }
  assert(controller_classify(bindings) == CONTROLLER_PROFILE_CUSTOM);
  puts("ALL PASS: controller defaults and custom-profile detection");
  return 0;
}
