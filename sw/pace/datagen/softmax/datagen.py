#!/usr/bin/env python3
# Copyright 2026 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""Data generator for the scalar PACE softmax kernel.

Softmax is not pointwise, so it cannot be one instruction. PACE evaluates the two
pointwise pieces -- exp through the generic pwpa opcode, and 1/x through PACE_INV --
and the row maximum, the subtraction, the reduction and the scaling stay in software.
That is the same split the Snitch reference uses; what differs is everything around it,
because CV32E40P has no SSR streamer, no FREP loop and no DMA, so the kernel is a plain
scalar loop over one core.

The two functions need different coefficients and there is only one bank, so the kernel
loads exp's coefficients, evaluates every exp, then overwrites the bank with inv's
coefficients before the reciprocal. Snitch does the same thing with two DMA transfers.

Bit-exactness makes the reduction order part of the contract: a sum of floats depends on
the order it is accumulated in, so this model walks each row left to right in the target
precision, exactly as test.c does. Reusing the Snitch generator's reduction would model
its unrolled multi-lane order instead and disagree with our hardware for the right reason.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "_pace_local"))
sys.path.insert(0, str(_HERE.parent / "pwpa"))

from golden import ACTIVATIONS                                    # noqa: E402
from invert import invert_sqrt                                    # noqa: E402
from pwpa import build_bst_bps, compute_part_id, evaluate_pwpa, fit_pwpa, generate_bps  # noqa: E402
import data_utils                                                 # noqa: E402
from data_utils import emit_license, format_array_declaration, format_array_definition  # noqa: E402
from datagen import arrange_params                                # noqa: E402

# PACE_INV reduces the exponent and fits only the mantissa, so its coefficients always
# cover [1, 2) no matter how large the denominator gets.
INV_FIT_RANGE = (1.0, 2.0)


def row_max(row, np_prec):
    """fmax.s down the row, left to right."""
    m = np_prec(row[0])
    for v in row[1:]:
        m = np_prec(max(m, np_prec(v)))
    return m


def row_sum(row, np_prec):
    """fadd.s down the row, left to right. The order is the contract."""
    s = np_prec(row[0])
    for v in row[1:]:
        s = np_prec(s + np_prec(v))
    return s


def build(rows, cols, x_min, x_max, n_deg, n_part, np_prec, prec, eps, seed):
    rng = np.random.default_rng(seed)
    attn = rng.uniform(x_min, x_max, size=(rows, cols)).astype(np_prec)

    # --- exp: fit over the whole range the offsets can reach -------------------
    exp_bps = generate_bps(float(x_min - x_max), 0.0, n_part, mode="linear")
    exp_coeffs = fit_pwpa(exp_bps, degree=n_deg, func=ACTIVATIONS["exp"])
    exp_bst = build_bst_bps(exp_bps)

    offs = np.zeros_like(attn)
    expd = np.zeros_like(attn)
    for r in range(rows):
        m = row_max(attn[r], np_prec)
        offs[r] = np.asarray([np_prec(np_prec(v) - m) for v in attn[r]], dtype=np_prec)
        part_id = compute_part_id(offs[r], exp_bps)
        expd[r] = evaluate_pwpa(offs[r], exp_coeffs, part_id=part_id,
                                degree=n_deg, np_prec=np_prec)

    # --- inv: reciprocal of each row's denominator -----------------------------
    deno = np.asarray([row_sum(expd[r], np_prec) for r in range(rows)], dtype=np_prec)

    inv_bps = generate_bps(INV_FIT_RANGE[0], INV_FIT_RANGE[1], n_part, mode="linear")
    inv_coeffs = fit_pwpa(inv_bps, degree=n_deg, func=ACTIVATIONS["inv"])
    inv_bst = build_bst_bps(inv_bps)
    eps_const = ACTIVATIONS["inv"](eps)
    inv_deno = invert_sqrt(deno, inv_coeffs, inv_bps, n_deg, eps=eps,
                           eps_const=eps_const, fn_name="inv", prec=prec)
    inv_deno = np.asarray(inv_deno, dtype=np_prec)

    # --- scale -----------------------------------------------------------------
    ofmap = np.zeros_like(attn)
    for r in range(rows):
        for c in range(cols):
            ofmap[r, c] = np_prec(np_prec(expd[r, c]) * inv_deno[r])

    exp_params = np.asarray(
        arrange_params(np_prec, exp_bst[2:], exp_coeffs, eps, 0, super_fmt="FP32"),
        dtype=np.uint32)
    inv_params = np.asarray(
        arrange_params(np_prec, inv_bst[2:], inv_coeffs, eps, eps_const, super_fmt="FP32"),
        dtype=np.uint32)

    return attn, ofmap, exp_params, inv_params, deno, inv_deno


def emit(**kw):
    prec = kw["prec"]
    np_prec = data_utils.numpy_type_from_precision_t(prec)
    ctype = data_utils.ctype_from_precision_t(prec)
    hex_ctype = data_utils.hex_ctype_from_precision_t(data_utils._integer_precision_t(prec))

    rows, cols = kw["rows"], kw["cols"]
    n_deg = kw["n_deg"]

    attn, ofmap, exp_params, inv_params, deno, inv_deno = build(
        rows, cols, kw["x_min"], kw["x_max"], n_deg, kw["n_part"],
        np_prec, prec, kw["eps"], kw.get("seed"))

    if n_deg >= (1 << 3):
        raise ValueError(f"n_deg {n_deg} does not fit the 3-bit CSR_PACE degree field")

    out = [emit_license()]
    out += [f"// Scalar PACE softmax: {rows} rows x {cols} columns, {prec}.",
            "// exp runs on the generic pwpa opcode, 1/x on PACE_INV, and the bank is",
            "// reloaded between the two phases."]
    out += [f"#define PACE_DEGREE {n_deg}",
            "#define CSR_VALUE PACE_DEGREE",
            f"#define ENABLE_{prec} 1",
            f"#define ROWS {rows}",
            f"#define COLS {cols}",
            f"#define INPUTS_LEN {rows * cols}",
            f"#define EXP_PARAMS_LEN {len(exp_params)}",
            f"#define INV_PARAMS_LEN {len(inv_params)}"]
    out += [f"typedef {hex_ctype} data_t;", "typedef uint32_t param_t;"]
    out += [format_array_declaration(f"extern {hex_ctype}", "ifmap", (rows * cols,))]
    out += [format_array_declaration(hex_ctype, "ofmap", (rows * cols,))]
    out += [format_array_declaration(hex_ctype, "expbuf", (rows * cols,))]
    out += [format_array_definition("uint32_t", "exp_params", exp_params, hex_format=True)]
    out += [format_array_definition("uint32_t", "inv_params", inv_params, hex_format=True)]
    out += [format_array_definition(ctype, "ifmap", attn.reshape(-1), hex_format=True)]
    out += [format_array_definition(ctype, "golden", ofmap.reshape(-1), hex_format=True)]
    return "\n\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-c", "--cfg", type=Path, required=True)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    param = json.loads(args.cfg.read_text())
    args.output.write_text(emit(**param))


if __name__ == "__main__":
    main()
