# PACE

PACE evaluates a function as a piecewise polynomial inside the FPU: software
loads a coefficient bank, an instruction hands an operand to the FPU, and the
polynomial for the matching partition is evaluated with Horner's method.

Operations, one opcode each:

    pace_pwpa/      generic polynomial, no exponent handling (exp lives here)
    pace_inv/       1/x
    pace_sqrt/      sqrt(x)
    pace_rsqrt/     1/sqrt(x)
    pace_inv_fp16/  1/x in FP16
    pace_vinv_fp16/ 1/x in FP16, two lanes per instruction

Kernels, where PACE is one step among several:

    pace_softmax/   max -> subtract -> exp (pwpa) -> sum -> 1/x (inv) -> scale

Activations, all on the generic pwpa opcode -- the same instruction word, only
data.h differs:

    pace_gelu/  pace_silu/  pace_tanh/  pace_sigmoid/
    pace_exp_fp16/  pace_gelu_fp16/  pace_silu_fp16/
    pace_tanh_fp16/ pace_sigmoid_fp16/

    pace_rm_ignored/  control: PACE ignores funct3 (see Verification)
    datagen/          generates data.h from params.json
    probe/            signal probe and a waveform set

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
| `PaceParts` | `16` | power of two, 4 .. 64 |

The FPU is instantiated for core 0 only. Upstream ties the APU off for all cores,
so this adds one rather than changing one.

## Changing the coefficient-bank geometry

The degree and partition count are elaboration parameters, not runtime ones. They live
in `rtl/pulp_cluster.sv` and everything downstream derives from them -- the bank width,
the parameter bus and the FPU wrapper's `PaceFeatures`:

    localparam int unsigned PaceDegree    = 2;   // <= MAX_PACE_DEGREE (4)
    localparam int unsigned PaceParts     = 16;  // power of two, 4 .. 64
    localparam int unsigned PaceParamWidth =
        ((PaceDegree + 1) * PaceParts + (PaceParts - 1) + 2 * PaceEps) * PaceDataWidth;

Change the two numbers, `make compile && make build`, and generate data with matching
`n_deg` / `n_part`. The default stays at 2/16 because that is what the regression suite
runs against; the data and the hardware must agree, so a mismatched pair fails every
input rather than degrading.

Snitch's layernorm reference uses degree 3 over 8 partitions -- 41 words instead of 65,
and more accurate for `rsqrt` despite the smaller bank. Verified end to end:

    PaceDegree 3, PaceParts 8       PARAMS_LEN = 41
      pace_rsqrt_d3        (FP32)   errors = 0
      pace_rsqrt_d3_fp16   (FP16)   errors = 0
      pace_rsqrt (deg2/16 data)     errors = 1024   <- the control

The last line is the one that matters: 65-word data on a 41-word bank fails all 1024
inputs. Had it passed, the geometry would not have reached the hardware.

Degree 3 buys accuracy only in FP32. Fitting `rsqrt` over [1,4] and comparing the model
against the true function:

    deg 2 / 16 parts   FP32 7.9e-5   FP16 9.8e-4
    deg 3 /  8 parts   FP32 4.0e-5   FP16 1.5e-3
    deg 3 / 16 parts   FP32 3.3e-6   FP16 1.8e-3

FP16 sits on its own resolution floor at ~1e-3, so raising the degree there gains
nothing. The kernels' own `verify.py` compares against a model built at the configured
degree with `rtol=0`, so it passes at any of these -- the difference is approximation
quality, not pass or fail.

## Software interface

Coefficients are plain word stores to `0x1020_1000`. `CSR_PACE` is `0xba0` and
carries the polynomial degree in `[2:0]`; the function comes from the instruction.

Scalar PACE is an OP-FP instruction (opcode `0x53`), function in `funct5`, format in
`fmt`:

| funct5 | function | FP32 (`fmt 00`) | FP16 (`fmt 10`) |
|---|---|---|---|
| `01100` | pwpa | `0x60c505d3` | `0x64c505d3` |
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

## Kernels

Softmax depends on every element of a row, so it cannot be one instruction. PACE
evaluates the two pointwise pieces -- `exp` on the generic pwpa opcode and `1/x` on
`PACE_INV` -- and the row maximum, the subtraction, the reduction and the scaling are
ordinary FP instructions. The Snitch reference splits it the same way; what differs is
everything around it, because CV32E40P has no SSR streamer, no FREP loop and no DMA, so
the kernel is a plain scalar loop on one core.

Two consequences worth knowing before writing another kernel:

**The bank is reloaded mid-kernel.** `exp` and `inv` need different coefficients and
there is one bank, so the kernel runs every exp, then overwrites `0x1020_1000` before
the reciprocals. Snitch does this too, with two DMA transfers. Removing the reload makes
all 256 outputs wrong -- `PACE_INV` runs happily against exp's coefficients and returns
plausible garbage, so this is worth a control rather than an assumption.

**The reduction order is part of the contract.** A sum of floats depends on the order it
is accumulated in, so `datagen/softmax/datagen.py` walks each row left to right in the
target precision, exactly as `test.c` does. Reusing the Snitch generator's reduction
would model its unrolled multi-lane order and disagree with our hardware for the right
reason.

The firmware is soft-float, so `fmax.s`, `fsub.s`, `fadd.s` and `fmul.s` are raw words
on fixed registers just like the PACE ones. `test.c` has the encodings.

    python3 datagen/softmax/datagen.py -c pace_softmax/params.json pace_softmax/data.h

## Regenerating data.h

    cd sw/pace
    python3 datagen/pwpa/datagen.py -c pace_sqrt/params.json pace_sqrt/data.h

Reproduces the committed `data.h` byte for byte.

A new activation costs a `params.json` and one entry in `datagen/_pace_local/golden.py`.
Nothing else changes -- not the RTL, not the instruction word, not `test.c`.

`golden.py` evaluates the reference in FP32 and casts back to the requested precision.
Computing it directly in FP16 fails: torch has no Half CPU kernel for `exp`, `rsqrt` or
`gelu`, which is why `inv` -- a division -- used to be the only function with an FP16
test. Widening also makes the reference the correctly-rounded FP32 value.

FP16ALT has no test. The decoder reaches it (`fmt=10` with `rm=101`) and the datapath
evaluates it, but the generator's precision system is keyed by byte size
(`FP64:8, FP32:4, FP16:2, FP8:1`), and FP16ALT is two bytes like FP16, so there is no
label for it. `float_to_hex` does carry a bfloat16 branch, reachable through the `__fp8`
ctype, but it truncates instead of rounding to nearest -- a golden built on it would
disagree with the RNE hardware.

## Verification

Each test compares all 1024 results against the generator's model with no
tolerance. All four operations, both formats, the vectorial form and five
activations are covered.

    hello           pace_pwpa       pace_inv        pace_sqrt
    pace_rsqrt      pace_inv_fp16   pace_vinv_fp16
    pace_gelu       pace_silu       pace_tanh       pace_sigmoid
    pace_exp_fp16   pace_gelu_fp16  pace_silu_fp16
    pace_tanh_fp16  pace_sigmoid_fp16   pace_softmax        all errors = 0

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
| `pace_softmax` | the mid-kernel bank reload removed | 256, every element |

The second one is what shows the function really comes from `funct5`: nothing else in
the test changes, and the CSR no longer carries a function field.

`pace_rm_ignored` is the inverse control. It is `pace_pwpa` with `funct3` set to `101`
instead of `000` -- the format is still FP32, so the golden data is unchanged and any
mismatch would be the rounding mode alone. It gives `errors = 0`, and so does `111`.
`fpnew_pace_fma_multi.sv` hardwires `fma_round_mode = RNE` on the Horner path, so the rm
field cannot reach a PACE result. That is what makes clearing `check_fprm` in the decoder
safe, and it is worth a regression test because the decoder's FP16ALT encoding overloads
that same field.
