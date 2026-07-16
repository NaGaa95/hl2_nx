/* gyro_math_test.c -- host unit test for the pure gyro conversion.
 * Build+run on the host: gcc -I. tests/gyro_math_test.c source/gyro_math.c -lm -o gyro_math_test && ./gyro_math_test
 */
#include "../source/gyro_math.h"
#include <math.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 1e-3f)

int main(void) {
  int dx, dy;

  /* zero input -> zero output, accumulator untouched */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0f, 0.0f, 100, 100, 0, &dx, &dy);
    CHECK(dx == 0); CHECK(dy == 0);
    CHECK(NEAR(a.rem_x, 0.0f)); CHECK(NEAR(a.rem_y, 0.0f)); }

  /* base scale: yaw 1.0 at 100% -> 40 counts (GYRO_BASE_SCALE = 40) */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 100, 100, 0, &dx, &dy);
    CHECK(dx == 40); CHECK(dy == 0); CHECK(NEAR(a.rem_x, 0.0f)); }

  /* sub-count motion accumulates across frames (0.02 * 40 = 0.8 per frame) */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.02f, 0.0f, 100, 100, 0, &dx, &dy);
    CHECK(dx == 0); CHECK(NEAR(a.rem_x, 0.8f));
    gyro_math_step(&a, 0.02f, 0.0f, 100, 100, 0, &dx, &dy);   /* 0.8 + 0.8 = 1.6 */
    CHECK(dx == 1); CHECK(NEAR(a.rem_x, 0.6f)); }

  /* invert_y flips vertical only */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0f, 1.0f, 100, 100, 0, &dx, &dy);
    CHECK(dy == 40); }
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0f, 1.0f, 100, 100, 1, &dx, &dy);
    CHECK(dy == -40); }

  /* per-axis sensitivity is independent: X at 200% -> 80, Y at 50% -> 20 */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 1.0f, 200, 50, 0, &dx, &dy);
    CHECK(dx == 80); CHECK(dy == 20); }

  /* X sensitivity scales the horizontal axis */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 200, 100, 0, &dx, &dy);
    CHECK(dx == 80);
    a.rem_x = 0.0f;
    gyro_math_step(&a, 1.0f, 0.0f, 50, 100, 0, &dx, &dy);
    CHECK(dx == 20); }

  /* negative input, with accumulation preserving sign */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, -1.0f, 0.0f, 100, 100, 0, &dx, &dy);
    CHECK(dx == -40); CHECK(NEAR(a.rem_x, 0.0f)); }
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, -0.02f, 0.0f, 100, 100, 0, &dx, &dy);  /* -0.8 */
    CHECK(dx == 0); CHECK(NEAR(a.rem_x, -0.8f));
    gyro_math_step(&a, -0.02f, 0.0f, 100, 100, 0, &dx, &dy);  /* -1.6 */
    CHECK(dx == -1); CHECK(NEAR(a.rem_x, -0.6f)); }

  if (failures == 0) { printf("ALL PASS\n"); return 0; }
  printf("%d CHECK(s) FAILED\n", failures);
  return 1;
}
