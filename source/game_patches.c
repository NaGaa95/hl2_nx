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

#define CPUINFO_FLAGS_OFFSET    4
#define CPUINFO_MIN_SIZE        8

#define CPU_FEATURE_SSE  (1u << 3)
#define CPU_FEATURE_SSE2 (1u << 4)

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

  size_t function_size = 0;
  uint32_t *function = (uint32_t *)so_find_addr_with_size(gameui, symbol, &function_size);
  if (!function || function_size < sizeof(force_fastswitch)) {
    debugPrintf("GameUI patch: save dialog symbol unavailable\n");
    return 0;
  }

  uint32_t *match = NULL;
  const size_t instruction_count = function_size / sizeof(uint32_t);
  const size_t pattern_count = sizeof(force_fastswitch) / sizeof(force_fastswitch[0]);

  for (size_t i = 0; i + pattern_count <= instruction_count; i++) {
    if (memcmp(function + i, force_fastswitch, sizeof(force_fastswitch)) != 0)
      continue;
    if (match) {
      debugPrintf("GameUI patch: ambiguous save dialog instruction pattern\n");
      return 0;
    }
    match = function + i;
  }

  if (!match) {
    debugPrintf("GameUI patch: save dialog instruction pattern unavailable\n");
    return 0;
  }

  *match = skip_fastswitch;
  debugPrintf("GameUI patch: preserving hud_fastswitch across save/load\n");
  return 1;
}

static int patch_arm64_cpu_features(so_module *tier0) {
  typedef const void *(*get_cpu_information_fn)(void);

  uintptr_t address = so_lookup_export(tier0, "GetCPUInformation");
  if (!address) {
    debugPrintf("CPU patch: GetCPUInformation unavailable\n");
    return 0;
  }

  uint8_t *cpu_info = (uint8_t *)((get_cpu_information_fn)address)();
  if (!cpu_info) {
    debugPrintf("CPU patch: GetCPUInformation returned NULL\n");
    return 0;
  }

  int cpu_info_size = 0;
  memcpy(&cpu_info_size, cpu_info, sizeof(cpu_info_size));
  if (cpu_info_size < CPUINFO_MIN_SIZE) {
    debugPrintf("CPU patch: unexpected CPUInformation size %d\n", cpu_info_size);
    return 0;
  }

  /*
   * The Android ARM64 modules contain NEON implementations behind Source's
   * legacy SSE/SSE2 capability switches. tier0 cannot infer those switches
   * from ARM /proc/cpuinfo, so advertise only the two verified dispatch bits.
   */
  cpu_info[CPUINFO_FLAGS_OFFSET] =
      cpu_info[CPUINFO_FLAGS_OFFSET] | CPU_FEATURE_SSE | CPU_FEATURE_SSE2;
  debugPrintf("CPU patch: enabling NEON-backed SSE/SSE2 dispatch\n");
  return 1;
}

void apply_game_patches(void) {
  so_module *gameui = so_find_module("libGameUI.so");
  if (gameui)
    patch_save_dialog_fastswitch(gameui);
}

void apply_post_init_game_patches(void) {
  so_module *tier0 = so_find_module("libtier0.so");
  if (tier0)
    patch_arm64_cpu_features(tier0);
}
