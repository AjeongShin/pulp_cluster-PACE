/*
 * PwPA-SW baseline: 16-segment, degree-2 Horner evaluation with ordinary FP
 * instructions and no PACE opcode, on all 8 cores, data in L1. The partition is found
 * the way the PACE paper describes its software baseline -- load the breakpoints one by
 * one and compare until the input's segment is reached -- so it works for any
 * breakpoint placement and its latency is data-dependent.
 *
 * The kernel is written as raw FP instruction words, not C floats: pulp-gcc's
 * -march=rv32imcxgap9 compiles float code Zfinx-style (FP operands in the integer
 * registers, e.g. `fmadd.s a0,a0,a1,a1`), while this CV32E40P has PULP_ZFINX = 0 and a
 * separate FP register file. For the same reason the result check is integer only
 * (bit-exact against the PwPA model output from the data generator).
 *
 * Inputs, outputs, breakpoints and coefficients all live in L1. Each core processes a
 * contiguous slice of the input. Timing: see ../bench_8c.h.
 */

#include "../bench_8c.h"
#include "data.h"

#define N_PART 16
#define N_BPS (N_PART - 1)
#define CHUNK (INPUTS_LEN / NUM_CORES)

__attribute__((section(".data_l1"))) uint32_t ifmap_l1[INPUTS_LEN];
__attribute__((section(".bss_l1")))  uint32_t ofmap_l1[INPUTS_LEN];
// c2[0..15], c1[0..15], c0[0..15] (datagen/_pace_local/helpers.py arrange_params).
__attribute__((section(".data_l1"))) uint32_t coeffs_l1[3 * N_PART];
// Interior breakpoints in ascending order.
__attribute__((section(".data_l1"))) uint32_t bps_l1[N_BPS];

// Orders FP32 bit patterns like the values they encode, using integer compares only.
// 0x80000000 = sign bit 31 mask
static inline uint32_t fp32_key(uint32_t bits) {
  return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
}

int main(void) {
  uint32_t core = bench_core_id();
  if (core >= NUM_CORES) return 0;

  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));
  enable_mcycle();

  // One core stages inputs, coefficients and sorted breakpoints into L1.
  if (core == 0) {
    for (int i = 0; i < INPUTS_LEN; i++) ifmap_l1[i] = ifmap[i];
    for (int i = 0; i < 3 * N_PART; i++) coeffs_l1[i] = params[i];
    for (int i = 0; i < N_BPS; i++) {
      uint32_t bp = params[3 * N_PART + i];
      int j = i;
      while (j > 0 && fp32_key(bps_l1[j - 1]) > fp32_key(bp)) {
        bps_l1[j] = bps_l1[j - 1];
        j--;
      }
      bps_l1[j] = bp;
    }
  }

  uint32_t t0, t1, t2;

  synch_barrier();
  read_mcycle(t0);
  {
    // Local register variables are set here, right before the asm: the call to
    // synch_barrier() above may clobber a1-a6.
    register const uint32_t *src  asm("a5") = ifmap_l1 + core * CHUNK; // Pointer to the current input element
    register uint32_t       *dst  asm("a4") = ofmap_l1 + core * CHUNK; // Pointer to the current output element
    register const uint32_t *end  asm("a3") = ifmap_l1 + (core + 1) * CHUNK; // End pointer of the core's assigned input chunk
    register const uint32_t *bps  asm("a2") = bps_l1; // Start address of the breakpoint array
    register const uint32_t *coef asm("a1") = coeffs_l1; // Start address of the coefficient array
    register const uint32_t *bpe  asm("a6") = bps_l1 + N_BPS; // End pointer of the breakpoint array
    // .balign comes before norvc so the padding is a c.nop, not two zero bytes.
    __asm__ volatile(".balign 4\n\t"
                     ".option push\n\t"
                     ".option norvc\n"
                     "1:\n\t"
                     ".word 0x0007a507\n\t"  // flw     fa0, 0(a5)        x
                     "addi  a5, a5, 4\n\t"
                     "mv    t0, a2\n"
                     "2:\n\t"
                     ".word 0x0002a587\n\t"  // flw     fa1, 0(t0)        next breakpoint
                     ".word 0xa0a59353\n\t"  // flt.s   t1, fa1, fa0      bp < x ?
                     "beqz  t1, 3f\n\t"
                     "addi  t0, t0, 4\n\t"
                     "bne   t0, a6, 2b\n"
                     "3:\n\t"
                     "sub   t0, t0, a2\n\t"  // 4 * partition
                     "add   t1, a1, t0\n\t"
                     ".word 0x00032607\n\t"  // flw     fa2, 0(t1)        c2
                     ".word 0x04032687\n\t"  // flw     fa3, 64(t1)       c1
                     ".word 0x08032707\n\t"  // flw     fa4, 128(t1)      c0
                     ".word 0x68a60643\n\t"  // fmadd.s fa2, fa2, fa0, fa3   c2*x + c1
                     ".word 0x70a60643\n\t"  // fmadd.s fa2, fa2, fa0, fa4   (..)*x + c0
                     ".word 0x00c72027\n\t"  // fsw     fa2, 0(a4)
                     "addi  a4, a4, 4\n\t"
                     "bne   a5, a3, 1b\n\t"
                     ".option pop\n"
                     : "+r"(src), "+r"(dst)
                     : "r"(end), "r"(bps), "r"(coef), "r"(bpe)
                     : "t0", "t1", "memory");
  }
  read_mcycle(t1);
  bench_core_cycles[core] = t1 - t0;
  synch_barrier();
  read_mcycle(t2);

  if (core != 0) return 0;

  int errors = 0;
  for (int i = 0; i < INPUTS_LEN; i++) {
    if (ofmap_l1[i] != golden[i]) {
      errors++;
      if (errors <= 8) printf("idx:%d actual=%x golden=%x\n", i, ofmap_l1[i], golden[i]);
    }
  }
  bench_report("PwPA-SW seq (L1, 8c)", t2 - t0, INPUTS_LEN, errors);
  return errors;
}
