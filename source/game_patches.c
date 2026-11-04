/* game_patches.c -- compatibility fixes for the original Android modules
 *
 * The Android GameUI contains console-specific behavior that can conflict
 * with user settings on Switch. Keep these changes small and validate the
 * surrounding instructions before modifying a loaded module.
 */

#include <stdint.h>
#include <string.h>

#include "game_patches.h"
#include "so_util.h"
#include "util.h"

static uint32_t *find_unique_pattern(so_module *module, const char *symbol,
                                     const uint32_t *pattern, size_t count) {
  size_t function_size = 0;
  uint32_t *function = (uint32_t *)so_find_addr_with_size(module, symbol, &function_size);
  if (!function || function_size < count * sizeof(*pattern))
    return NULL;

  uint32_t *match = NULL;
  const size_t instruction_count = function_size / sizeof(*function);
  for (size_t i = 0; i + count <= instruction_count; i++) {
    if (memcmp(function + i, pattern, count * sizeof(*pattern)) != 0)
      continue;
    if (match)
      return NULL;
    match = function + i;
  }
  return match;
}

static int patch_save_dialog_fastswitch(so_module *gameui) {
  static const char symbol[] =
      "_ZN19CBaseSaveGameDialog16OnKeyCodePressedE12ButtonCode_t";

  // This is the body of:
  //   if (hud_fastswitch.IsValid() && hud_fastswitch.GetInt() != 2)
  //       hud_fastswitch.SetValue(2);
  //
  // The first instruction branches past the body only when the ConVar is
  // invalid. Replacing it with an unconditional branch preserves the user's
  // value when a save/load action is confirmed with controller A.
  static const uint32_t force_fastswitch[] = {
    0x36000140, // tbz w0, #0, +0x28
    0xf9400be8, // ldr x8, [sp, #16]
    0xb9405908, // ldr w8, [x8, #88]
    0x7100091f, // cmp w8, #2
    0x540000c0, // b.eq +0x18
    0xf94007e0, // ldr x0, [sp, #8]
    0x52800041, // mov w1, #2
    0xf9400008, // ldr x8, [x0]
    0xf9400908, // ldr x8, [x8, #16]
    0xd63f0100, // blr x8
  };
  static const uint32_t skip_fastswitch = 0x1400000a; // b +0x28

  const size_t pattern_count = sizeof(force_fastswitch) / sizeof(force_fastswitch[0]);
  uint32_t *match = find_unique_pattern(gameui, symbol, force_fastswitch, pattern_count);
  if (!match) {
    debugPrintf("GameUI patch: save dialog instruction pattern unavailable or ambiguous\n");
    return 0;
  }

  *match = skip_fastswitch;
  debugPrintf("GameUI patch: preserving hud_fastswitch across save/load\n");
  return 1;
}

static int patch_video_gamma_button(so_module *gameui) {
  // Android disables this control because it has no display gamma ramp.
  // The Switch SDL shim implements the ramp with a final rendering pass.
  static const uint32_t reset[] = {
    0xf9415260, // ldr x0, [x19, #672] (m_pGammaButton)
    0x2a1f03e1, // mov w1, wzr
    0xf9400008, // ldr x8, [x0]
    0xf940dd08, // ldr x8, [x8, #440] (SetEnabled)
    0xd63f0100, // blr x8
  };
  static const uint32_t layout[] = {
    0xf9400008, // ldr x8, [x0]
    0xf940dd02, // ldr x2, [x8, #440] (SetEnabled)
    0x2a1f03e1, // mov w1, wzr
    0xf9400bf3, // ldr x19, [sp, #16]
    0xa8c27bfd, // ldp x29, x30, [sp], #32
    0xd61f0040, // br x2
  };
  uint32_t *on_reset = find_unique_pattern(gameui,
      "_ZN16COptionsSubVideo11OnResetDataEv", reset, sizeof(reset) / sizeof(*reset));
  uint32_t *on_layout = find_unique_pattern(gameui,
      "_ZN16COptionsSubVideo13PerformLayoutEv", layout, sizeof(layout) / sizeof(*layout));
  if (!on_reset || !on_layout) {
    debugPrintf("GameUI patch: gamma button instruction patterns unavailable or ambiguous\n");
    return 0;
  }

  // Validate both sites before enabling the control; layout must not undo reset.
  on_reset[1] = on_layout[2] = 0x52800021; // mov w1, #1
  debugPrintf("GameUI patch: enabling display gamma control\n");
  return 1;
}

void apply_game_patches(void) {
  so_module *gameui = so_find_module("libGameUI.so");
  if (gameui) {
    patch_save_dialog_fastswitch(gameui);
    patch_video_gamma_button(gameui);
  }
}
