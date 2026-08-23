# PACE

PACE evaluates a function as a piecewise polynomial inside the FPU: software
loads a coefficient bank, an instruction hands an operand to the FPU, and the
polynomial for the matching partition is evaluated with Horner's method.

    pace_pwpa/    generic polynomial, no exponent handling (fits exp here)
    pace_inv/     1/x
    pace_sqrt/    sqrt(x)
    pace_rsqrt/   1/sqrt(x)
    datagen/      generates data.h from params.json
    probe/        signal probe and a waveform set

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

Coefficients are plain word stores to `0x1020_1000`. `CSR_PACE` is `0xba0`:

| bits | field |
|---|---|
| `[4:2]` | degree |
| `[1:0]` | 0 pwpa, 1 inv, 2 sqrt, 3 rsqrt |

`PACE_S` is an OP-FP instruction, `funct5 = 01100`. pulp-gcc has no `.insn` here
and the firmware is soft-float, so the sequence is raw words on fixed registers:

    fmv.w.x fa2, x0      0xf0000653
    fmv.w.x fa0, a5      0xf0078553
    PACE_S  fa1,fa0,fa2  0x60c505d3
    fmv.x.w a4, fa1      0xe0058753

## Regenerating data.h

    cd sw/pace
    python3 datagen/pwpa/datagen.py -c pace_sqrt/params.json pace_sqrt/data.h

Reproduces the committed `data.h` byte for byte.

## Verification

Each test compares all 1024 results against the generator's model with no
tolerance. All four operations are covered.

    hello  pace_pwpa  pace_inv  pace_sqrt  pace_rsqrt      all errors = 0

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

| change | result |
|---|---|
| degree 2 to 1 | 1024 errors |
| sqrt word replaced with the inv one | 930 errors |
| FP16 format field set back to FP32 | 1024 errors |
| vector word replaced with the scalar one | 512, exactly half |

The last two were measured on the transprecision branch.
