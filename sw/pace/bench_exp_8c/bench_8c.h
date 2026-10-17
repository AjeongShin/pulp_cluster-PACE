/*
 * Timing harness shared by the 8-core L1 benchmarks in this folder.
 *
 * Every core processes its own contiguous slice of the input. All cores leave an
 * opening barrier together and meet again at a closing barrier; core 0's mcycle span
 * between the two is the cluster time for the whole input, i.e. the slowest core plus
 * one barrier. mcycle is a per-core counter and each core enables it at a different
 * moment, so absolute values are never compared across cores; each core only records
 * its own compute duration.
 */

#ifndef BENCH_8C_H
#define BENCH_8C_H

#include <stdint.h>
#include <stdio.h>
#include "pulp.h"

#define NUM_CORES 8

#define read_mcycle(x) __asm__ volatile("csrr %0, mcycle" : "=r"(x))
// mcountinhibit.CY defaults to 1 (disabled) out of reset; every core clears its own.
#define enable_mcycle() __asm__ volatile("csrci 0x320, 0x1")

// Each core's compute cycles, written before the closing barrier.
__attribute__((section(".bss_l1"))) static uint32_t bench_core_cycles[NUM_CORES];

static inline uint32_t bench_core_id(void) {
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
  return hartid & 0x1f;
}

// Called by core 0 after the closing barrier. span covers n_elems elements in total.
static void bench_report(const char *name, uint32_t span, int n_elems, int errors) {
  uint32_t lo = 0xffffffffu, hi = 0;
  for (int c = 0; c < NUM_CORES; c++) {
    if (bench_core_cycles[c] < lo) lo = bench_core_cycles[c];
    if (bench_core_cycles[c] > hi) hi = bench_core_cycles[c];
  }
  printf("%s errors = %d\n", name, errors);
  printf("%s cluster cycles = %u (%u.%02u cyc/elem over %d elems on %d cores)\n", name, span,
         span / n_elems, (span % n_elems) * 100 / n_elems, n_elems, NUM_CORES);
  printf("%s per-core compute cycles: min %u, max %u (%d elems per core)\n", name, lo, hi,
         n_elems / NUM_CORES);
}

#endif  // BENCH_8C_H
