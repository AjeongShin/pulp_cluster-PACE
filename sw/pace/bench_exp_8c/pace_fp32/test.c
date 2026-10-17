/*
 * PACE-FP32 exp on all 8 cores, data in L1. One core loads the shared coefficient bank
 * and stages the inputs; every core writes its own degree CSR and processes a
 * contiguous slice. PACE_S is emitted as a raw instruction word, and the input and the
 * result move between the integer and FP registers with fmv. The check is bit-exact
 * against the PwPA model output from the data generator. Timing: see ../bench_8c.h.
 */

#include "../bench_8c.h"
#include "data.h"

#define PACE_PARAM_BASE ((volatile param_t *)0x10201000u)
#define write_csr_pace(val) __asm__ volatile("csrw 0xba0, %0" ::"r"(val))

#define CHUNK (INPUTS_LEN / NUM_CORES)

__attribute__((section(".data_l1"))) uint32_t ifmap_l1[INPUTS_LEN];
__attribute__((section(".bss_l1")))  uint32_t ofmap_l1[INPUTS_LEN];

int main(void) {
  uint32_t core = bench_core_id();
  if (core >= NUM_CORES) return 0;

  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));
  enable_mcycle();

  // The coefficient bank is shared by all FPUs, so one core loads it; it also stages
  // the inputs into L1. CSR_PACE is per core, so every core writes its own.
  if (core == 0) {
    for (int i = 0; i < PARAMS_LEN; i++) PACE_PARAM_BASE[i] = params[i];
    for (int i = 0; i < INPUTS_LEN; i++) ifmap_l1[i] = ifmap[i];
  }
  write_csr_pace(CSR_VALUE);

  // PACE_S (FP32):
  //   fmv.w.x fa2,x0      0xf0000653
  //   fmv.w.x fa0,a5      0xf0078553
  //   PACE_S  fa1,fa0,fa2 0x60c505d3
  //   fmv.x.w a4,fa1      0xe0058753
  register uint32_t pace_in  asm("a5");
  register uint32_t pace_out asm("a4");
  uint32_t t0, t1, t2;

  synch_barrier();
  read_mcycle(t0);
  for (int i = core * CHUNK; i < (int)(core + 1) * CHUNK; i++) {
    pace_in = ifmap_l1[i];
    __asm__ volatile(".word 0xf0000653\n\t"
                     ".word 0xf0078553\n\t"
                     ".word 0x60c505d3\n\t"
                     ".word 0xe0058753\n\t"
                     : "=r"(pace_out)
                     : "r"(pace_in));
    ofmap_l1[i] = pace_out;
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
  bench_report("PACE-FP32 (L1, 8c)", t2 - t0, INPUTS_LEN, errors);
  return errors;
}
