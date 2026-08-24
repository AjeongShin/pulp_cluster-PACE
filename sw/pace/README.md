# PACE

PACE evaluates a function as a piecewise polynomial inside the FPU: software
loads a coefficient bank, an instruction hands an operand to the FPU, and the
polynomial for the matching partition is evaluated with Horner's method.

    pace_pwpa/      generic polynomial, no exponent handling (fits exp here)
    pace_inv/       1/x
    pace_sqrt/      sqrt(x)
    pace_rsqrt/     1/sqrt(x)
    pace_inv_fp16/  1/x in FP16
    pace_vinv_fp16/ 1/x in FP16, two lanes per instruction
    datagen/        generates data.h from params.json
    probe/          signal probe and a waveform set

## Running

    source env/env.sh
    make scripts/compile.tcl && make compile && make build

    cd sw/pace/pace_sqrt
    USE_CV32E40P=1 make clean all run

`USE_CV32E40P=1` is required. Without it the arch flags fall through to the RI5CY
path, which leaves the PULP hardware loops enabled. CV32E40P does not implement
them and the failure is silent: the firmware builds, boots, then takes an
exception on the first loop body instruction and runs away through unmapped
memory.

## Configuration

| setting | value | why |
|---|---|---|
| `CoreType` | `CV32` | |
| `HwpePresent` | `0` | frees the config slot at `0x1020_1000` for the coefficient bank |
| `HMRPresent` | `1` | `0` leaves the HMR register interface without a ready and boot deadlocks |
| `PaceDegree` | `2` | maximum degree; software selects up to this through `CSR_PACE` |
| `PaceParts` | `16` | |

The FPU is instantiated for core 0 only. Upstream ties the APU off for all cores,
so this adds one rather than changing one.

## Software interface

Coefficients are plain word stores to `0x1020_1000`. `CSR_PACE` is `0xba0` and
carries the polynomial degree in `[2:0]`; the function comes from the instruction.

Scalar PACE is an OP-FP instruction (opcode `0x53`), function in `funct5`, format in
`fmt`:

| funct5 | function | FP32 (`fmt 00`) | FP16 (`fmt 10`) |
|---|---|---|---|
| `01100` | pwpa | `0x60c505d3` | |
| `01101` | inv | `0x68c505d3` | `0x6cc505d3` |
| `01110` | sqrt | `0x70c505d3` | |
| `01111` | rsqrt | `0x78c505d3` | |

Vectorial PACE sits in the PULP SIMD space (opcode `0x33`, `instr[31:30] = 10`) with
the function in `instr[26:25]` and the format in `instr[13:12]`. Two FP16 lanes per
instruction: `vpace.h fa1, fa0` is `0xba0525b3`.

pulp-gcc has no `.insn` here and the firmware is soft-float, so every sequence is raw
words on fixed registers:

    fmv.w.x fa2, x0      0xf0000653
    fmv.w.x fa0, a5      0xf0078553
    PACE_S  fa1,fa0,fa2  0x68c505d3
    fmv.x.w a4, fa1      0xe0058753

## Regenerating data.h

    cd sw/pace
    python3 datagen/pwpa/datagen.py -c pace_sqrt/params.json pace_sqrt/data.h

Reproduces the committed `data.h` byte for byte.

## Verification

Each test compares all 1024 results against the generator's model with no
tolerance. All four operations, both formats and the vectorial form are covered.

    hello  pace_pwpa  pace_inv  pace_sqrt  pace_rsqrt
    pace_inv_fp16  pace_vinv_fp16                          all errors = 0

`probe/pace_signals.tcl` follows the coefficients from the bank into fpnew:

    PACE_TEST=pace_sqrt questa-2023.4-zr vsim -c -do sw/pace/probe/pace_signals.tcl

Against `params[0..2]` of `pace_sqrt` (`BDE03E82 BDB0C5B6 BD8FFA28`):

    param_q[0..2]       bde03e82 bdb0c5b6 bd8ffa28
    pace_param_o        bde03e82
    wrapper pace_param  bde03e82
    fpnew pace_param_i  bde03e82
    op_i                PACE_SQRT
    pace_mode           extend 0, enable 1, degree 2
    req/gnt             1/1

For waveforms, run with `gui=1` and `do ../probe/pace_waves.do`.

Passing tests only show the answer is right. These break it on purpose:

| test | change | result |
|---|---|---|
| `pace_rsqrt` | degree 2 to 1 | 1024 errors |
| `pace_sqrt` | funct5 set to the inv one | 930 errors |
| `pace_inv_fp16` | fmt set back to FP32 | 1024 errors |
| `pace_vinv_fp16` | vector word replaced with the scalar one | 512, exactly half |

The second one is what shows the function really comes from `funct5`: nothing else in
the test changes, and the CSR no longer carries a function field.
