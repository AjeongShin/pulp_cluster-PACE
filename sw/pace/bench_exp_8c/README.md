# exp benchmark on the 8-core PULP cluster

Five implementations of the exponential on the CV32E40P-PACE cluster, all measured
under the same conditions so they can be compared directly.

| Folder | Implementation |
|---|---|
| `exp_c_softfloat` | fdlibm `expf` without the F extension: libgcc soft-float, no FP instruction (a cluster without FPUs) |
| `exp_c_fpu` | fdlibm `expf` on the FPU, hardware divide |
| `pwpa_sw` | piecewise polynomial in software: sequential breakpoint search, two `fmadd.s` |
| `pace_fp32` | PACE, FP32 (`PACE_S`) |
| `pace_fp16_simd` | PACE, FP16, two lanes per instruction (`vpace.h`) |

## Conditions

- exp over [-11, 0], degree 2, 16 partitions, uniformly spaced breakpoints
- 1024 inputs: evenly spaced values over [-11, 0], shuffled (`data/`)
- all 8 cores in parallel, each on a contiguous slice of 128 elements
- inputs, outputs, coefficients and constants in L1; instructions from L2 through the icache
- timed region: the compute loop only; loading the coefficient bank and writing the
  degree CSR are excluded

## Results

QuestaSim RTL simulation, one run each.

| Implementation | Cluster cycles | Cycles / element | Per-core compute (min ~ max) | Check |
|---|---|---|---|---|
| Exp-C, soft-float | 630021 | 615.25 | 620394 ~ 630021 | 0 ulp from host `expf` (1024/1024 exact) |
| Exp-C, FPU | 15977 | 15.60 | 15747 ~ 15977 | <= 1 ulp from host `expf` |
| PwPA-SW | 11353 | 11.09 | 10484 ~ 11353 | bit-exact vs PwPA model |
| PACE-FP32 | 1937 | 1.89 | 1890 ~ 1899 | bit-exact vs PwPA model |
| PACE-FP16 SIMD | 1042 | 1.02 | 994 ~ 1003 | bit-exact vs PwPA model |

PolyEval/cycle for the whole cluster is `1024 / cluster cycles`; multiply by the clock
frequency for PolyEval/s.

**Cluster cycles** is the larger of core 0's `mcycle` span between the opening and
closing barriers and the slowest core's own compute time. The per-core counters are not
synchronized (each core enables its counter at a different moment), and in some runs
core 0 started its span measurably later than the slowest core started computing.
See `bench_8c.h`.

## Running

From the repository root:

```
source env/env.sh
cd sw/pace/bench_exp_8c/pace_fp32
USE_CV32E40P=1 make clean all run
```

The two Exp-C tests also need a standard RISC-V GCC (we used xpack
riscv-none-elf-gcc 15.2.0). Point `RVF_PREFIX` at it, e.g.
`make RVF_PREFIX=/path/to/xpack/bin/riscv-none-elf- clean all run`.

## Why the tests look the way they do

- **No C float code in the test programs.** pulp-gcc (`-march=rv32imcxgap9`) compiles
  float code with FP operands in the integer registers (Zfinx-style), but this
  CV32E40P has `PULP_ZFINX = 0` and a separate FP register file. Compiled float code,
  the toolchain's libm included, therefore computes on stale f-registers, and a
  float-based result check passes regardless. So:
  - PACE and PwPA-SW emit their FP instructions as raw instruction words;
  - Exp-C builds fdlibm's `expf` (`exp_c_fpu/expf_rvf.c`) separately with a standard
    GCC, which keeps FP operands in the f-registers, and calls it as
    `uint32_t -> uint32_t` (ilp32 passes a float in `a0`);
  - every check compares results as integer bit patterns.
- **Exp-C** is fdlibm's `expf`, the algorithm newlib's libm uses on float-only targets,
  without libm's error-handling wrapper.
- **PwPA-SW** searches the breakpoints sequentially, as the PACE paper's software
  baseline does, so it works for any breakpoint placement. The PACE hardware uses a
  4-step binary search instead.
- **PACE-FP16 SIMD** loads and stores both lanes with one 32-bit access. Loading two
  16-bit halves and packing them in integer registers took about 1.5x the cycles on a
  single core.

## Data

`data/fp32` and `data/fp16` hold the generator configuration (`params.json`) and its
output (`data.h`: coefficients and breakpoints in the hardware layout, inputs, and the
PwPA model output used as the golden reference). To regenerate, from `sw/pace`:

```
python3 datagen/pwpa/datagen.py -c bench_exp_8c/data/fp32/params.json bench_exp_8c/data/fp32/data.h
```

After changing the inputs, regenerate the Exp-C host reference with `make ref` in
`exp_c_fpu` (`exp_c_softfloat` shares it).
