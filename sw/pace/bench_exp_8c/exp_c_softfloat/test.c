/*
 * Exp-C baseline without the FPU: the same fdlibm expf source as ../exp_c_fpu, built
 * without the F extension (-march=rv32imc), so every float operation is a call into
 * libgcc's integer-only soft-float routines and no FP instruction executes. This is
 * what a cluster without FPUs runs. All 8 cores, data in L1. This file contains no
 * float code; the check is integer only. Timing: see ../bench_8c.h.
 */

#include "../bench_8c.h"
#include "data.h"
#include "expc_ref.h"

#define CHUNK (INPUTS_LEN / NUM_CORES)

// Without the F extension nothing is contracted into fmadd, so the result should match
// the host reference (built with -ffp-contract=off) exactly; keep the same tolerance as
// the FPU version so the two checks are comparable.
#define ULP_TOL 2

#ifndef EXPC_NAME
#define EXPC_NAME "Exp-C soft-float (L1, 8c)"
#endif

extern uint32_t expf_rvf(uint32_t x_bits);

__attribute__((section(".data_l1"))) uint32_t ifmap_l1[INPUTS_LEN];
__attribute__((section(".bss_l1")))  uint32_t ofmap_l1[INPUTS_LEN];

int main(void) {
  uint32_t core = bench_core_id();
  if (core >= NUM_CORES) return 0;

  enable_mcycle();

  if (core == 0) {
    for (int i = 0; i < INPUTS_LEN; i++) ifmap_l1[i] = ifmap[i];
  }

  uint32_t t0, t1, t2;

  synch_barrier();
  read_mcycle(t0);
  for (int i = core * CHUNK; i < (int)(core + 1) * CHUNK; i++) {
    ofmap_l1[i] = expf_rvf(ifmap_l1[i]);
  }
  read_mcycle(t1);
  bench_core_cycles[core] = t1 - t0;
  synch_barrier();
  read_mcycle(t2);

  if (core != 0) return 0;

  int errors = 0, exact = 0;
  uint32_t max_ulp = 0;
  for (int i = 0; i < INPUTS_LEN; i++) {
    uint32_t a = ofmap_l1[i], r = expc_ref[i];
    uint32_t d = a > r ? a - r : r - a;
    if (d == 0) exact++;
    if (d > max_ulp) max_ulp = d;
    if (d > ULP_TOL) {
      errors++;
      if (errors <= 8) printf("idx:%d in=%x actual=%x ref=%x\n", i, ifmap[i], a, r);
    }
  }
  printf("%s exact %d, max %u ulp from host expf\n", EXPC_NAME, exact, max_ulp);
  bench_report(EXPC_NAME, t2 - t0, INPUTS_LEN, errors);
  return errors;
}
