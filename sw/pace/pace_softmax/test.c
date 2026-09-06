/*
 * Scalar PACE softmax on the PULP cluster.
 *
 * Softmax depends on every element of a row, so it cannot be a single instruction.
 * PACE evaluates the two pointwise pieces -- exp on the generic pwpa opcode and 1/x
 * on PACE_INV -- and the row maximum, the subtraction, the reduction and the scaling
 * are ordinary FP instructions:
 *
 *   max over row -> subtract -> exp (PACE) -> sum -> 1/x (PACE) -> scale
 *
 * exp and inv need different coefficients and there is one bank, so the kernel runs
 * every exp first, then overwrites the bank before the reciprocals. The Snitch
 * reference does the same with two DMA transfers; here it is a store loop.
 *
 * The firmware is soft-float (rv32imcxgap9 has no F extension), so GCC can neither
 * allocate FP registers nor assemble the instructions. Every FP operation below is a
 * raw word on fixed registers, moving bit patterns in through a5/a3 and out through
 * a4. That keeps each helper self-contained: no value is left live in an f-register
 * across a statement the compiler might reorder.
 */

#include <stdio.h>
#include <stdint.h>
#include "data.h"

// PACE coefficient bank: cluster base + peripheral offset + accelerator config slot.
#define PACE_PARAM_BASE ((volatile param_t *)0x10201000u)

// CSR_PACE = 0xba0, carrying the polynomial degree.
#define write_csr_pace(val) __asm__ volatile("csrw 0xba0, %0" ::"r"(val))

// Shared prologue/epilogue of every helper:
//   fmv.w.x fa0, a5   0xf0078553    first operand  -> fa0
//   fmv.w.x fa1, a3   0xf00685d3    second operand -> fa1
//   fmv.w.x fa2, x0   0xf0000653    zero -> fa2. Not required: the PACE datapath
//                                   ignores rs2, and dropping it still gives errors = 0.
//   fmv.x.w a4, fa1   0xe0058753    result         -> a4

#define FP_BINOP(name, word)                            \
  static inline uint32_t name(uint32_t a, uint32_t b) { \
    register uint32_t in_a __asm__("a5") = a;           \
    register uint32_t in_b __asm__("a3") = b;           \
    register uint32_t out __asm__("a4");                \
    __asm__ volatile(".word 0xf0078553\n\t"             \
                     ".word 0xf00685d3\n\t"             \
                     ".word " #word "\n\t"              \
                     ".word 0xe0058753\n\t"             \
                     : "=r"(out)                        \
                     : "r"(in_a), "r"(in_b));           \
    return out;                                         \
  }

FP_BINOP(fp_max, 0x28b515d3)  // fmax.s fa1, fa0, fa1
FP_BINOP(fp_sub, 0x08b505d3)  // fsub.s fa1, fa0, fa1  -> fa0 - fa1
FP_BINOP(fp_add, 0x00b505d3)  // fadd.s fa1, fa0, fa1
FP_BINOP(fp_mul, 0x10b505d3)  // fmul.s fa1, fa0, fa1

#define PACE_UNOP(name, word)                  \
  static inline uint32_t name(uint32_t x) {    \
    register uint32_t in __asm__("a5") = x;    \
    register uint32_t out __asm__("a4");       \
    __asm__ volatile(".word 0xf0000653\n\t"    \
                     ".word 0xf0078553\n\t"    \
                     ".word " #word "\n\t"     \
                     ".word 0xe0058753\n\t"    \
                     : "=r"(out)                \
                     : "r"(in));                \
    return out;                                \
  }

PACE_UNOP(pace_pwpa, 0x60c505d3)  // funct5 01100 pwpa, fmt 00
PACE_UNOP(pace_inv, 0x68c505d3)   // funct5 01101 inv,  fmt 00

static void load_bank(const uint32_t *src, int len) {
  for (int i = 0; i < len; i++) PACE_PARAM_BASE[i] = src[i];
}

static int check_output(data_t *actual, data_t *golden_ref, int len) {
  int errors = len;
  for (int i = 0; i < len; i++) {
    if (actual[i] == golden_ref[i])
      errors--;
    else if (errors < len - 8)  // keep the log short: report the first few only
      printf("idx:%d actual=%x golden=%x\n", i, actual[i], golden_ref[i]);
  }
  return errors;
}

int main(void) {
  // Every core enters main(), but only core 0 drives the test.
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
  if ((hartid & 0x1f) != 0) return 0;

  // Enable the FPU (mstatus.FS = Initial) so FP instructions are legal.
  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));

  uint32_t deno[ROWS];
  uint32_t recip[ROWS];

  // --- phase 1: exp -------------------------------------------------------
  load_bank(exp_params, EXP_PARAMS_LEN);
  write_csr_pace(CSR_VALUE);

  for (int r = 0; r < ROWS; r++) {
    const int base = r * COLS;

    // Row maximum, left to right. Subtracting it is what keeps exp in range.
    uint32_t m = ifmap[base];
    for (int c = 1; c < COLS; c++) m = fp_max(ifmap[base + c], m);

    for (int c = 0; c < COLS; c++)
      expbuf[base + c] = pace_pwpa(fp_sub(ifmap[base + c], m));

    // Denominator, accumulated left to right. The order is part of the contract:
    // the generator sums in this same order, and a different one would round
    // differently and disagree bit for bit.
    uint32_t s = expbuf[base];
    for (int c = 1; c < COLS; c++) s = fp_add(expbuf[base + c], s);
    deno[r] = s;
  }

  // --- phase 2: reciprocal ------------------------------------------------
  // exp and inv have different coefficients and there is only one bank.
  load_bank(inv_params, INV_PARAMS_LEN);

  for (int r = 0; r < ROWS; r++) recip[r] = pace_inv(deno[r]);

  for (int r = 0; r < ROWS; r++) {
    const int base = r * COLS;
    for (int c = 0; c < COLS; c++)
      ofmap[base + c] = fp_mul(expbuf[base + c], recip[r]);
  }

  int errors = check_output(ofmap, golden, INPUTS_LEN);
  printf("PACE softmax errors = %d\n", errors);
  return errors;
}
