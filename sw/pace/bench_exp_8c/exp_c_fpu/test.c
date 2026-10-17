/*
 * Exp-C baseline: fdlibm expf on the FPU, all 8 cores, data in L1.
 *
 * pulp-gcc's -march=rv32imcxgap9 compiles float code -- the toolchain's libm included --
 * with FP operands in the integer registers (Zfinx-style), but this CV32E40P has
 * PULP_ZFINX = 0 and a separate FP register file, so compiled float code computes on
 * stale f-registers. expf is therefore fdlibm's (expf_rvf.c), built separately by a
 * standard RISC-V GCC (see Makefile) so its FP operands live in the f-registers. Under
 * ilp32 a float argument/result is passed in a0, so this file treats it as
 * uint32_t -> uint32_t and contains no float code at all.
 *
 * Inputs, outputs and expf's constants are in L1. The check is integer only: distance
 * in units in the last place from a host-generated reference (exp() > 0, so the bit
 * patterns are ordered like the values). Timing: see ../bench_8c.h.
 */

#include "../bench_8c.h"
#include "data.h"
#include "expc_ref.h"

#define CHUNK (INPUTS_LEN / NUM_CORES)

// The target build contracts a*b+c into fmadd and the host reference does not, so the
// two can legitimately differ in the last place.
#define ULP_TOL 2

#ifndef EXPC_NAME
#define EXPC_NAME "Exp-C rvf (L1, 8c)"
#endif

extern uint32_t expf_rvf(uint32_t x_bits);

__attribute__((section(".data_l1"))) uint32_t ifmap_l1[INPUTS_LEN];
__attribute__((section(".bss_l1")))  uint32_t ofmap_l1[INPUTS_LEN];

int main(void) {
  uint32_t core = bench_core_id();
  if (core >= NUM_CORES) return 0;

  // Enable the FPU (mstatus.FS = Initial) so FP instructions are legal.
  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));
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
