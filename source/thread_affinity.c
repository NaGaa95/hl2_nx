/* thread_affinity.c -- Nintendo Switch three-core scheduling policy
 *
 * Source's main/render work is latency-sensitive and benefits from staying on
 * one core. Its worker threads are distributed over the other two application
 * cores, avoiding needless migration while retaining parallel execution.
 */

#include <stdint.h>
#include <switch.h>

#include "thread_affinity.h"

#define MAX_MANAGED_CORES 3

static uint32_t worker_mask;
static unsigned int worker_sequence;
static int main_core = -1;
static int background_core = -1;
static int affinity_enabled;

static int count_cores(uint32_t mask) {
  int count = 0;
  while (mask) {
    count += mask & 1u;
    mask >>= 1;
  }
  return count;
}

static int first_core(uint32_t mask) {
  for (int core = 0; core < 32; core++)
    if (mask & (1u << core))
      return core;
  return -1;
}

static int last_core(uint32_t mask) {
  for (int core = 31; core >= 0; core--)
    if (mask & (1u << core))
      return core;
  return -1;
}

static int core_at(uint32_t mask, unsigned int index) {
  const int count = count_cores(mask);
  if (!count)
    return -1;

  index %= (unsigned int)count;
  for (int core = 0; core < 32; core++) {
    if (!(mask & (1u << core)))
      continue;
    if (index-- == 0)
      return core;
  }
  return -1;
}

static Result pin_current_thread(int core) {
  if (!affinity_enabled || core < 0)
    return 0;
  return svcSetThreadCoreMask(threadGetCurHandle(), core, 1u << core);
}

void thread_affinity_init(void) {
  uint64_t process_mask = 0;
  uint32_t managed_mask = 0;
  worker_mask = 0;
  worker_sequence = 0;
  main_core = -1;
  background_core = -1;
  affinity_enabled = 0;

  if (R_FAILED(svcGetInfo(
          &process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
    return;

  /*
   * Source is told that three processors are available. If Horizon exposes a
   * larger mask, keep the highest core free rather than oversubscribing what
   * the engine believes is a three-core machine.
   */
  int selected = 0;
  for (int core = 0; core < 32 && selected < MAX_MANAGED_CORES; core++) {
    if (!(process_mask & (1ull << core)))
      continue;
    managed_mask |= 1u << core;
    selected++;
  }

  if (selected < 2)
    return;

  main_core = last_core(managed_mask);
  worker_mask = managed_mask & ~(1u << main_core);
  background_core = first_core(worker_mask);
  affinity_enabled = 1;

  if (R_FAILED(pin_current_thread(main_core)))
    affinity_enabled = 0;
}

void thread_affinity_pin_source_thread(void) {
  if (!affinity_enabled)
    return;

  const unsigned int sequence =
      __atomic_fetch_add(&worker_sequence, 1u, __ATOMIC_RELAXED);
  pin_current_thread(core_at(worker_mask, sequence));
}

void thread_affinity_pin_background_thread(void) {
  pin_current_thread(background_core);
}

void thread_affinity_pin_render_thread(void) {
  pin_current_thread(main_core);
}
