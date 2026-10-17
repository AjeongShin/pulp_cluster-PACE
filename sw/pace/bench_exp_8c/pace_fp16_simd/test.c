/*
 * PACE-FP16 SIMD (vpace.h, two FP16 lanes per 32-bit register) on all 8 cores, data in
 * L1. Both lanes are loaded and stored with a single 32-bit access; loading them as two
 * 16-bit halves and packing them in integer registers took about 1.5x the cycles on a
 * single core. Otherwise the same structure as ../pace_fp32. The check is bit-exact
 * against the PwPA model output from the data generator. Timing: see ../bench_8c.h.
 */

#include "../bench_8c.h"
#include "data.h"

#define PACE_PARAM_BASE ((volatile param_t *)0x10201000u)
#define write_csr_pace(val) __asm__ volatile("csrw 0xba0, %0" ::"r"(val))

#define PAIRS_PER_CORE (INPUTS_LEN / 2 / NUM_CORES)

// Element view (h) and lane-pair view (w) of the same storage. Little-endian, so
// h[2k] is the low lane of w[k]; the union also guarantees 4-byte alignment.
typedef union {
  uint32_t w[INPUTS_LEN / 2]; // 32-bit packed data: FP16 x 2
  uint16_t h[INPUTS_LEN]; // 16-bit element data: FP16
} lanes_t;

__attribute__((section(".data_l1"))) lanes_t in_l1;
__attribute__((section(".bss_l1")))  lanes_t out_l1;

int main(void) {
  uint32_t core = bench_core_id();
  if (core >= NUM_CORES) return 0;

  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));
  enable_mcycle();

  // One core loads the shared coefficient bank and stages the inputs into L1;
  // CSR_PACE is per core, so every core writes its own.
  if (core == 0) {
    for (int i = 0; i < PARAMS_LEN; i++) PACE_PARAM_BASE[i] = params[i];
    for (int i = 0; i < INPUTS_LEN; i++) in_l1.h[i] = ifmap[i];
  }
  write_csr_pace(CSR_VALUE);

  //   fmv.w.x  fa0, a5     0xf0078553
  //   vpace.h  fa1, fa0    0xb80525b3
  //   fmv.x.w  a4, fa1     0xe0058753
  register uint32_t pace_in  asm("a5");
  register uint32_t pace_out asm("a4");
  uint32_t t0, t1, t2;

  synch_barrier();
  read_mcycle(t0);
  for (int i = core * PAIRS_PER_CORE; i < (int)(core + 1) * PAIRS_PER_CORE; i++) {
    pace_in = in_l1.w[i];
    __asm__ volatile(".word 0xf0078553\n\t"
                     ".word 0xb80525b3\n\t"
                     ".word 0xe0058753\n\t"
                     : "=r"(pace_out)
                     : "r"(pace_in));
    out_l1.w[i] = pace_out;
  }
  read_mcycle(t1);
  bench_core_cycles[core] = t1 - t0;
  synch_barrier();
  read_mcycle(t2);

  if (core != 0) return 0;

  int errors = 0;
  for (int i = 0; i < INPUTS_LEN; i++) {
    if (out_l1.h[i] != golden[i]) {
      errors++;
      if (errors <= 8) printf("idx:%d actual=%x golden=%x\n", i, out_l1.h[i], golden[i]);
    }
  }
  bench_report("PACE-FP16 SIMD (L1, 8c)", t2 - t0, INPUTS_LEN, errors);
  return errors;
}
