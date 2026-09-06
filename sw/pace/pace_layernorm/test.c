/*
 * Scalar PACE layernorm on the PULP cluster.
 *
 * Like softmax, layernorm is not pointwise: a whole channel has to be reduced before
 * any output element exists. PACE evaluates exactly one step of it -- 1/sqrt(variance)
 * on PACE_RSQRT -- and the two reductions, the centring and the scaling are ordinary
 * FP instructions:
 *
 *   sum, sum-of-squares -> mean -> variance -> rsqrt (PACE) -> centre and scale
 *
 * Unlike softmax there is only one coefficient set, so the bank is loaded once and
 * never reloaded. PACE_RSQRT reduces the exponent and fits only the mantissa, so its
 * coefficients cover [1, 4] whatever the variance turns out to be.
 *
 * The firmware is soft-float, so every FP operation is a raw word on fixed registers,
 * bit patterns in through a5/a3 and out through a4 -- the same idiom as pace_softmax.
 */

#include <stdio.h>
#include <stdint.h>
#include "data.h"

// PACE coefficient bank: cluster base + peripheral offset + accelerator config slot.
#define PACE_PARAM_BASE ((volatile param_t *)0x10201000u)

// CSR_PACE = 0xba0, carrying the polynomial degree.
#define write_csr_pace(val) __asm__ volatile("csrw 0xba0, %0" ::"r"(val))

// Shared prologue/epilogue:
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

FP_BINOP(fp_sub, 0x08b505d3)  // fsub.s fa1, fa0, fa1  -> fa0 - fa1
FP_BINOP(fp_add, 0x00b505d3)  // fadd.s fa1, fa0, fa1
FP_BINOP(fp_mul, 0x10b505d3)  // fmul.s fa1, fa0, fa1

static inline uint32_t pace_rsqrt(uint32_t x) {
  register uint32_t in __asm__("a5") = x;
  register uint32_t out __asm__("a4");
  __asm__ volatile(".word 0xf0000653\n\t"
                   ".word 0xf0078553\n\t"
                   ".word 0x78c505d3\n\t"  // funct5 01111 rsqrt, fmt 00
                   ".word 0xe0058753\n\t"
                   : "=r"(out)
                   : "r"(in));
  return out;
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

// Row-major storage, so a channel is a strided column.
#define AT(e, c) ((e) * CHANNELS + (c))

int main(void) {
  // Every core enters main(), but only core 0 drives the test.
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
  if ((hartid & 0x1f) != 0) return 0;

  // Enable the FPU (mstatus.FS = Initial) so FP instructions are legal.
  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));

  for (int i = 0; i < PARAMS_LEN; i++) PACE_PARAM_BASE[i] = params[i];
  write_csr_pace(CSR_VALUE);

  const uint32_t inv_n = INV_N;

  for (int c = 0; c < CHANNELS; c++) {
    // One sequential pass over the channel accumulating both reductions. The order
    // is part of the contract, and measurably so: the generator walks the channel
    // the same way, and running this loop backwards instead gives 968 errors out of
    // 1024 -- same values, same operations, different rounding.
    uint32_t s = 0u;  // +0.0f
    uint32_t q = 0u;
    for (int e = 0; e < ELEMS; e++) {
      const uint32_t x = ifmap[AT(e, c)];
      // Written as (x * inv_n) * x to mirror the Snitch reference. With ELEMS a
      // power of two, inv_n is exact and (x * x) * inv_n gives the same bits;
      // measured, both give errors = 0. It would matter for a non-power-of-two N.
      const uint32_t x_prod = fp_mul(fp_mul(x, inv_n), x);
      s = fp_add(s, x);
      q = fp_add(q, x_prod);
    }

    const uint32_t mean = fp_mul(s, inv_n);
    const uint32_t sigma2 = fp_sub(q, fp_mul(mean, mean));
    const uint32_t rsq = pace_rsqrt(sigma2);

    for (int e = 0; e < ELEMS; e++)
      ofmap[AT(e, c)] = fp_mul(fp_sub(ifmap[AT(e, c)], mean), rsq);
  }

  int errors = check_output(ofmap, golden, INPUTS_LEN);
  printf("PACE layernorm errors = %d\n", errors);
  return errors;
}
