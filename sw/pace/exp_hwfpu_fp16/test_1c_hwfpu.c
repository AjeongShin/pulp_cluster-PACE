/*
 * HW-FPU exp() baseline for pace_exp_fp16: same 1024 FP16 inputs, libm's expf()
 * (real fadd.s/fmul.s/etc on the same fpnew unit, unfused) instead of PACE_H.
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../pace_exp_fp16/data.h"

#define read_mcycle(x) __asm__ volatile("csrr %0, mcycle" : "=r"(x))

// mcountinhibit.CY defaults to 1 (disabled) out of reset; clear it once to count.
#define enable_mcycle() __asm__ volatile("csrci 0x320, 0x1")

static float fp16_to_fp32(uint16_t h) {
  uint32_t sign = (uint32_t)(h >> 15) & 0x1u;
  uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
  uint32_t man  = (uint32_t)h & 0x3FFu;
  uint32_t f;
  if (exp == 0) {
    f = sign << 31;  // zero (no subnormal FP16 values in this data set)
  } else if (exp == 0x1F) {
    f = (sign << 31) | (0xFFu << 23) | (man << 13);  // inf/nan
  } else {
    f = (sign << 31) | ((exp - 15u + 127u) << 23) | (man << 13);
  }
  float out;
  __builtin_memcpy(&out, &f, sizeof(out));
  return out;
}

static uint16_t fp32_to_fp16(float x) {
  uint32_t f;
  __builtin_memcpy(&f, &x, sizeof(f));
  uint32_t sign = (f >> 31) & 0x1u;
  int32_t  exp  = (int32_t)((f >> 23) & 0xFFu) - 127 + 15;
  uint32_t man  = f & 0x7FFFFFu;
  if (exp <= 0) return (uint16_t)(sign << 15);                        // underflow to zero
  if (exp >= 0x1F) return (uint16_t)((sign << 15) | (0x1Fu << 10));   // overflow to inf
  return (uint16_t)((sign << 15) | ((uint32_t)exp << 10) | (man >> 13));
}

static int check_output(uint16_t *actual, uint16_t *golden_ref, int len) {
  int errors = 0;
  for (int i = 0; i < len; i++) {
    // expf() isn't bit-exact vs. the PACE/datagen golden model -> relative tolerance.
    float a = fp16_to_fp32(actual[i]);
    float g = fp16_to_fp32(golden_ref[i]);
    float diff = a - g;
    if (diff < 0) diff = -diff;
    float tol = 0.05f * (g < 0 ? -g : g) + 1e-3f;
    if (diff > tol) {
      errors++;
      if (errors <= 8)
        printf("idx:%d actual=%x(%f) golden=%x(%f)\n", i, actual[i], a, golden_ref[i], g);
    }
  }
  return errors;
}

uint16_t ofmap_hwfpu[INPUTS_LEN];

int main(void) {
  // Only core 0 runs (single-core baseline, matches pace_exp_fp16).
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
  if ((hartid & 0x1f) != 0) return 0;

  enable_mcycle();

  uint32_t compute_start, compute_end;
  read_mcycle(compute_start);
  for (int i = 0; i < INPUTS_LEN; i++) {
    float x = fp16_to_fp32(ifmap[i]);
    float y = expf(x);
    ofmap_hwfpu[i] = fp32_to_fp16(y);
  }
  read_mcycle(compute_end);

  int errors = check_output(ofmap_hwfpu, golden, INPUTS_LEN);
  printf("HW-FPU expf errors = %d\n", errors);
  printf("HW-FPU expf compute cycles = %u (avg %u cyc/elem over %d elems)\n",
         compute_end - compute_start, (compute_end - compute_start) / INPUTS_LEN, INPUTS_LEN);
  return errors;
}
