/* gyro_math_test.c -- host unit test for the pure gyro conversion.
 * Build+run on the host: gcc -I. tests/gyro_math_test.c source/gyro_math.c -lm -o gyro_math_test && ./gyro_math_test
 */
#include "../source/gyro_math.h"
#include <math.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 1e-3f)

/* GYRO_BASE_SCALE is 10.0; tighten/accel default off in these unless set. */
int main(void) {
  int dx, dy;

  /* zero input -> zero output, accumulator untouched */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0f, 0.0f, 0.0f, 100, 100, 0, 0, 0, &dx, &dy);
    CHECK(dx == 0); CHECK(dy == 0);
    CHECK(NEAR(a.rem_x, 0.0f)); CHECK(NEAR(a.rem_y, 0.0f)); }

  /* base scale: sum 1.0 at 100% -> 10 counts */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 1.0f, 100, 100, 0, 0, 0, &dx, &dy);
    CHECK(dx == 10); CHECK(dy == 0); CHECK(NEAR(a.rem_x, 0.0f)); }

  /* sub-count motion accumulates across frames (0.06 * 10 = 0.6 per frame) */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.06f, 0.0f, 1.0f, 100, 100, 0, 0, 0, &dx, &dy);
    CHECK(dx == 0); CHECK(NEAR(a.rem_x, 0.6f));
    gyro_math_step(&a, 0.06f, 0.0f, 1.0f, 100, 100, 0, 0, 0, &dx, &dy);   /* 1.2 */
    CHECK(dx == 1); CHECK(NEAR(a.rem_x, 0.2f)); }

  /* invert_y flips vertical only */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0f, 1.0f, 1.0f, 100, 100, 0, 0, 0, &dx, &dy);
    CHECK(dy == 10); }
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0f, 1.0f, 1.0f, 100, 100, 1, 0, 0, &dx, &dy);
    CHECK(dy == -10); }

  /* per-axis sensitivity is independent: X at 200% -> 20, Y at 50% -> 5 */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 1.0f, 1.0f, 200, 50, 0, 0, 0, &dx, &dy);
    CHECK(dx == 20); CHECK(dy == 5); }

  /* X sensitivity scales the horizontal axis */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 1.0f, 200, 100, 0, 0, 0, &dx, &dy);
    CHECK(dx == 20);
    a.rem_x = 0.0f;
    gyro_math_step(&a, 1.0f, 0.0f, 1.0f, 50, 100, 0, 0, 0, &dx, &dy);
    CHECK(dx == 5); }

  /* negative input, with accumulation preserving sign */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, -1.0f, 0.0f, 1.0f, 100, 100, 0, 0, 0, &dx, &dy);
    CHECK(dx == -10); CHECK(NEAR(a.rem_x, 0.0f)); }
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, -0.06f, 0.0f, 1.0f, 100, 100, 0, 0, 0, &dx, &dy);  /* -0.6 */
    CHECK(dx == 0); CHECK(NEAR(a.rem_x, -0.6f));
    gyro_math_step(&a, -0.06f, 0.0f, 1.0f, 100, 100, 0, 0, 0, &dx, &dy);  /* -1.2 */
    CHECK(dx == -1); CHECK(NEAR(a.rem_x, -0.2f)); }

  /* tightening (threshold 0.08): zero at rest, full above threshold, tapered below */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 0.0f, 100, 100, 0, 1, 0, &dx, &dy);
    CHECK(dx == 0); }                                    /* speed 0 -> fully tapered */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 0.16f, 100, 100, 0, 1, 0, &dx, &dy);
    CHECK(dx == 10); }                                   /* speed >= threshold -> full */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 0.02f, 100, 100, 0, 1, 0, &dx, &dy);
    CHECK(dx == 1); }                                    /* smoothstep(0.25)=0.15625 -> 1.5625 */

  /* acceleration (low 0.5, high 5.0, max 2x): ramps the multiplier by speed */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 0.5f, 100, 100, 0, 0, 1, &dx, &dy);
    CHECK(dx == 10); }                                   /* at low -> 1x */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 5.0f, 100, 100, 0, 0, 1, &dx, &dy);
    CHECK(dx == 20); }                                   /* at high -> 2x */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 2.75f, 100, 100, 0, 0, 1, &dx, &dy);
    CHECK(dx == 15); }                                   /* midpoint -> 1.5x */

  if (failures == 0) { printf("ALL PASS\n"); return 0; }
  printf("%d CHECK(s) FAILED\n", failures);
  return 1;
}
