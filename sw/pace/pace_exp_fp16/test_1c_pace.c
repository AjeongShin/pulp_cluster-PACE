/*
 * PACE exp test in FP16: generic PWPA op (funct5 01100) evaluates the coefficient
 * table directly, no exponent pre/post-processing (unlike inv/sqrt/rsqrt).
 *
 * Flow: load coefficients into PACE coefficient memory -> set degree via CSR_PACE
 * (0xba0) -> run PACE_H per input -> compare bit-exact against golden.
 */

#include <stdio.h>
#include <stdint.h>
#include "data.h"

// PACE coefficient memory: cluster base + peripheral offset + accelerator config slot.
#define PACE_PARAM_BASE ((volatile param_t *)0x10201000u)

// CSR_PACE = 0xba0, the same custom CSR as in the Snitch reference.
#define write_csr_pace(val) __asm__ volatile("csrw 0xba0, %0" ::"r"(val))
#define read_mcycle(x) __asm__ volatile("csrr %0, mcycle" : "=r"(x))

// mcountinhibit.CY defaults to 1 (disabled) out of reset; clear it once to count.
#define enable_mcycle() __asm__ volatile("csrci 0x320, 0x1")

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
  // Only core 0 runs.
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
  if ((hartid & 0x1f) != 0) return 0;

  // Enable the FPU (mstatus.FS = Initial) so FP instructions are legal.
  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));
  enable_mcycle();

  uint32_t setup_start, setup_end, compute_start, compute_end;
  read_mcycle(setup_start);

  // 1) Load the coefficients into the PACE coefficient memory.
  for (int i = 0; i < PARAMS_LEN; i++) {
    PACE_PARAM_BASE[i] = params[i];
  }

  // 2) Select the polynomial degree through CSR_PACE. The function is not in the
  //    CSR: it comes from the instruction's funct5.
  write_csr_pace(CSR_VALUE);

  read_mcycle(setup_end);

  // PACE_H (FP16): opcode=0x53, funct3=0 (rm=RNE), funct7=0x32 (funct5=01100 pwpa,
  // fmt=10). No compiler support for this opcode, so raw .word on fixed registers:
  //   fmv.h.x fa2,x0      0xf4000653   zero rs2 (unused by PACE, kept for trace clarity)
  //   fmv.h.x fa0,a5      0xf4078553   input -> FP reg
  //   PACE_H  fa1,fa0,fa2 0x64c505d3
  //   fmv.x.h a4,fa1      0xe4058753   result -> int reg
  register uint32_t pace_in  asm("a5");
  register uint32_t pace_out asm("a4");

  read_mcycle(compute_start);
  for (int i = 0; i < INPUTS_LEN; i++) {
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

  // 4) Bit-exact comparison against the datagen model.
  int errors = check_output(ofmap, golden, INPUTS_LEN);
  printf("PACE exp FP16 errors = %d\n", errors);
  printf("PACE exp FP16 setup cycles = %u\n", setup_end - setup_start);
  printf("PACE exp FP16 compute cycles = %u (avg %u cyc/elem over %d elems)\n",
         compute_end - compute_start, (compute_end - compute_start) / INPUTS_LEN, INPUTS_LEN);
  return errors;
}
