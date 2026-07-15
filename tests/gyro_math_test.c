/* gyro_math_test.c -- host unit test for the pure gyro conversion.
 * Build+run on the host: gcc -I. tests/gyro_math_test.c source/gyro_math.c -lm -o gyro_math_test && ./gyro_math_test
 */
#include "../source/gyro_math.h"
#include <math.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 1e-4f)

int main(void) {
  int dx, dy;

  /* zero input -> zero output, accumulator untouched */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0f, 0.0f, 100, 0, &dx, &dy);
    CHECK(dx == 0); CHECK(dy == 0);
    CHECK(NEAR(a.rem_x, 0.0f)); CHECK(NEAR(a.rem_y, 0.0f)); }

  /* base scale: yaw 1.0 at 100% -> 8 counts */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 100, 0, &dx, &dy);
    CHECK(dx == 8); CHECK(dy == 0); CHECK(NEAR(a.rem_x, 0.0f)); }

  /* sub-count motion accumulates across frames */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0625f, 0.0f, 100, 0, &dx, &dy);   /* 0.0625*8 = 0.5 */
    CHECK(dx == 0); CHECK(NEAR(a.rem_x, 0.5f));
    gyro_math_step(&a, 0.0625f, 0.0f, 100, 0, &dx, &dy);   /* 0.5 + 0.5 = 1.0 */
    CHECK(dx == 1); CHECK(NEAR(a.rem_x, 0.0f)); }

  /* invert_y flips vertical only */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 0.0f, 1.0f, 100, 0, &dx, &dy);
    CHECK(dy == 8);
    a.rem_y = 0.0f;
    gyro_math_step(&a, 0.0f, 1.0f, 100, 1, &dx, &dy);
    CHECK(dy == -8); }

  /* sensitivity scales output */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, 1.0f, 0.0f, 200, 0, &dx, &dy);
    CHECK(dx == 16);
    a.rem_x = 0.0f;
    gyro_math_step(&a, 1.0f, 0.0f, 50, 0, &dx, &dy);
    CHECK(dx == 4); }

  /* negative input, with accumulation preserving sign */
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, -1.0f, 0.0f, 100, 0, &dx, &dy);
    CHECK(dx == -8); CHECK(NEAR(a.rem_x, 0.0f)); }
  { GyroAccum a = { 0.0f, 0.0f };
    gyro_math_step(&a, -0.0625f, 0.0f, 100, 0, &dx, &dy);  /* -0.5 */
    CHECK(dx == 0); CHECK(NEAR(a.rem_x, -0.5f));
    gyro_math_step(&a, -0.0625f, 0.0f, 100, 0, &dx, &dy);  /* -1.0 */
    CHECK(dx == -1); CHECK(NEAR(a.rem_x, 0.0f)); }

  if (failures == 0) { printf("ALL PASS\n"); return 0; }
  printf("%d CHECK(s) FAILED\n", failures);
  return 1;
}
