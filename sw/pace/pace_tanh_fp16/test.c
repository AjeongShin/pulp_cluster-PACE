/*
 * PACE tanh test in FP16 on the PULP cluster.
 *
 * Uses the generic PWPA operation (funct5 01100), which evaluates the
 * coefficient table directly without the exponent pre- and post-processing the inv,
 * sqrt and rsqrt operations apply. The approximated function here is exp.
 *
 * Phase 2 of the PACE integration: the coefficients now live in the cluster's PACE
 * coefficient memory (a cluster peripheral, the analogue of Snitch's axi_pace_mem)
 * instead of a testbench-local memory, and the evaluation runs on the core's FPU.
 *
 * Flow (identical to the Snitch reference and to the Phase-1 standalone test):
 *   1) write the datagen coefficients into the PACE coefficient memory,
 *   2) configure the polynomial degree through CSR_PACE (0xba0),
 *   3) evaluate every input with the dedicated PACE_S instruction,
 *   4) compare bit-exact against the datagen model output ("golden").
 */

#include <stdio.h>
#include <stdint.h>
#include "data.h"

// PACE coefficient memory: cluster base + peripheral offset + accelerator config slot.
#define PACE_PARAM_BASE ((volatile param_t *)0x10201000u)

// CSR_PACE = 0xba0, the same custom CSR as in the Snitch reference.
#define write_csr_pace(val) __asm__ volatile("csrw 0xba0, %0" ::"r"(val))

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
  // Every core enters main(), but only core 0 drives the test. The core index is the
  // low bits of mhartid, the same way the boot code derives it.
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
  if ((hartid & 0x1f) != 0) return 0;

  // Enable the FPU (mstatus.FS = Initial) so FP instructions are legal.
  __asm__ volatile("csrs mstatus, %0" ::"r"(0x2000));

  // 1) Load the coefficients into the PACE coefficient memory.
  for (int i = 0; i < PARAMS_LEN; i++) {
    PACE_PARAM_BASE[i] = params[i];
  }

  // 2) Select the polynomial degree through CSR_PACE. The function is not in the
  //    CSR: it comes from the instruction's funct5.
  write_csr_pace(CSR_VALUE);

  // 3) PACE_H (FP16): opcode=0x53 (OP-FP), funct3=0x0 (rm=RNE), funct7=0x32
  //    (funct5=01100 pwpa, fmt=10 FP16). PACE reads operand 0, so the input goes in rs1;
  //    rs2 is unused and only there to complete the R-type encoding.
  //
  //    CV32E40P runs with PULP_ZFINX = 0, so it has a real FP register file and the
  //    instruction's register fields index f-registers. The firmware is built soft-float
  //    (rv32imcxgap9 has no F extension), so the compiler can neither allocate FP
  //    registers nor assemble `.insn`; the whole sequence is emitted as raw words on
  //    fixed registers, exposing only integer registers to the compiler:
  //      fmv.h.x fa2, x0     0xf4000653   dummy operand = 0
  //      fmv.h.x fa0, a5     0xf4078553   input bit pattern -> FP register
  //      PACE_H  fa1,fa0,fa2 0x64c505d3   (0x30<<25)|(12<<20)|(10<<15)|(11<<7)|0x53
  //      fmv.x.h a4, fa1     0xe4058753   result -> integer register
  register uint32_t pace_in  asm("a5");
  register uint32_t pace_out asm("a4");

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

  // 4) Bit-exact comparison against the datagen model.
  int errors = check_output(ofmap, golden, INPUTS_LEN);
  printf("PACE tanh FP16 errors = %d\n", errors);
  return errors;
}
