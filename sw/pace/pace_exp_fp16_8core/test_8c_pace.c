/*
 * 8-core version of sw/pace/pace_exp_fp16: same 1024 FP16 inputs, split 128/core
 * across all 8 private PACE FPUs.
 *
 * Coefficient memory is broadcast to all 8 FPUs, so only core 0 loads it; CSR_PACE
 * is per-hart, so every core sets its own. One barrier gates compute on setup being
 * done, a second gates the result check on every core's slice being written.
 */

#include <stdio.h>
#include <stdint.h>
#include "pulp.h"
#include "../pace_exp_fp16/data.h"

// PACE coefficient memory: cluster base + peripheral offset + accelerator config slot.
#define PACE_PARAM_BASE ((volatile param_t *)0x10201000u)

// CSR_PACE = 0xba0, the same custom CSR as in the Snitch reference.
#define write_csr_pace(val) __asm__ volatile("csrw 0xba0, %0" ::"r"(val))
#define read_mcycle(x) __asm__ volatile("csrr %0, mcycle" : "=r"(x))

// mcountinhibit.CY defaults to 1 (disabled) out of reset; clear it once to count.
#define enable_mcycle() __asm__ volatile("csrci 0x320, 0x1")

#define NUM_CORES 8
#define CHUNK (INPUTS_LEN / NUM_CORES)

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
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
  uint32_t core = hartid & 0x1f;
  if (core >= NUM_CORES) return 0;

  // Enable the FPU (mstatus.FS = Initial) so FP instructions are legal.
  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));
  enable_mcycle();

  uint32_t setup_start = 0, setup_end = 0;
  if (core == 0) {
    read_mcycle(setup_start);
    for (int i = 0; i < PARAMS_LEN; i++) {
      PACE_PARAM_BASE[i] = params[i];
    }
    read_mcycle(setup_end);
  }
  write_csr_pace(CSR_VALUE);

  synch_barrier();  // coefficients + every core's CSR_PACE must be ready before any PACE_H issues

  // PACE_H (FP16), same encoding as test_1c_pace.c -- see that file for the
  // per-instruction breakdown. Only the ifmap[]/ofmap[] slice differs per core.
  register uint32_t pace_in  asm("a5");
  register uint32_t pace_out asm("a4");

  uint32_t compute_start, compute_end;
  read_mcycle(compute_start);
  for (int i = core * CHUNK; i < (core + 1) * CHUNK; i++) {
    pace_in = ifmap[i];
    __asm__ volatile(".word 0xf4000653\n\t"
                     ".word 0xf4078553\n\t"
                     ".word 0x64c505d3\n\t"
                     ".word 0xe4058753\n\t"
                     : "=r"(pace_out)
                     : "r"(pace_in));
    ofmap[i] = pace_out;
  }
  read_mcycle(compute_end);

  printf("PACE exp FP16 core %u compute cycles = %u (avg %u cyc/elem over %d elems)\n",
         core, compute_end - compute_start, (compute_end - compute_start) / CHUNK, CHUNK);

  synch_barrier();  // every core's slice of ofmap[] must be written before core 0 checks it

  if (core != 0) return 0;

  printf("PACE exp FP16 setup cycles = %u\n", setup_end - setup_start);
  int errors = check_output(ofmap, golden, INPUTS_LEN);
  printf("PACE exp FP16 8-core errors = %d\n", errors);
  return errors;
}
