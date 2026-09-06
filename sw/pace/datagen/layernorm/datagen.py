#!/usr/bin/env python3
# Copyright 2026 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""Data generator for the scalar PACE layernorm kernel.

Per channel, following the Snitch reference:

    x_prod   = (x * inv_n) * x      inv_n is 1/N; q accumulates x^2/N directly
    sum     += x                    both reductions in one pass
    q       += x_prod
    sigma2   = q - mean*mean        mean = sum * inv_n
    out      = (x - mean) * PACE_RSQRT(sigma2)

The reduction order is the contract: reversing the loop gives 968 mismatches of 1024.
The `x_prod` association does not matter while N is a power of two, because inv_n is
then exact and the multiply is a pure exponent adjustment.
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
from pwpa import build_bst_bps, fit_pwpa, generate_bps            # noqa: E402
import data_utils                                                 # noqa: E402
from data_utils import emit_license, format_array_declaration, format_array_definition  # noqa: E402
from datagen import arrange_params                                # noqa: E402

# PACE_RSQRT fits the mantissa only; the exponent is handled by frexp/ldexp.
RSQRT_FIT_RANGE = (1.0, 4.0)


def channel_stats(col, inv_n, np_prec):
    """One sequential pass accumulating sum and sum of squares, as test.c does."""
    s = np_prec(0.0)
    q = np_prec(0.0)
    for v in col:
        x = np_prec(v)
        x_scaled = np_prec(x * inv_n)
        x_prod = np_prec(x_scaled * x)
        s = np_prec(s + x)
        q = np_prec(q + x_prod)
    return s, q


def build(elems, channels, n_deg, n_part, np_prec, prec, eps, seed):
    rng = np.random.default_rng(seed)
    # A distinct mean and spread per channel, so each one exercises a different
    # partition of the rsqrt fit and a different exponent.
    mu = rng.uniform(-2.0, 2.0, size=channels)
    sigma = rng.uniform(0.5, 2.0, size=channels)
    x = np.empty((elems, channels), dtype=np_prec)
    for c in range(channels):
        x[:, c] = rng.normal(mu[c], sigma[c], size=elems).astype(np_prec)

    inv_n = np_prec(np_prec(1.0) / np_prec(elems))

    bps = generate_bps(RSQRT_FIT_RANGE[0], RSQRT_FIT_RANGE[1], n_part, mode="linear")
    coeffs = fit_pwpa(bps, degree=n_deg, func=ACTIVATIONS["rsqrt"])
    bst = build_bst_bps(bps)
    eps_const = ACTIVATIONS["rsqrt"](eps)

    mean = np.zeros(channels, dtype=np_prec)
    sigma2 = np.zeros(channels, dtype=np_prec)
    for c in range(channels):
        s, q = channel_stats(x[:, c], inv_n, np_prec)
        m = np_prec(s * inv_n)
        mean[c] = m
        sigma2[c] = np_prec(q - np_prec(m * m))

    rsq = np.asarray(
        invert_sqrt(sigma2, coeffs, bps, n_deg, eps=eps, eps_const=eps_const,
                    fn_name="rsqrt", prec=prec),
        dtype=np_prec)

    ofmap = np.zeros_like(x)
    for c in range(channels):
        for e in range(elems):
            centred = np_prec(np_prec(x[e, c]) - mean[c])
            ofmap[e, c] = np_prec(centred * rsq[c])

    params = np.asarray(
        arrange_params(np_prec, bst[2:], coeffs, eps, eps_const, super_fmt="FP32"),
        dtype=np.uint32)

    return x, ofmap, params, inv_n, sigma2, rsq


def emit(**kw):
    prec = kw["prec"]
    np_prec = data_utils.numpy_type_from_precision_t(prec)
    ctype = data_utils.ctype_from_precision_t(prec)
    hex_ctype = data_utils.hex_ctype_from_precision_t(data_utils._integer_precision_t(prec))

    elems, channels, n_deg = kw["elems"], kw["channels"], kw["n_deg"]
    if n_deg >= (1 << 3):
        raise ValueError(f"n_deg {n_deg} does not fit the 3-bit CSR_PACE degree field")

    x, ofmap, params, inv_n, sigma2, rsq = build(
        elems, channels, n_deg, kw["n_part"], np_prec, prec, kw["eps"], kw.get("seed"))

    inv_n_bits = int(np.asarray(inv_n, dtype=np.float32).view(np.uint32))

    out = [emit_license()]
    out += [f"// Scalar PACE layernorm: {channels} channels x {elems} elements, {prec}.",
            "// PACE_RSQRT supplies 1/sqrt(variance); the reductions, the centring and",
            "// the scaling are ordinary FP instructions. One coefficient set, so unlike",
            "// softmax the bank is never reloaded."]
    out += [f"#define PACE_DEGREE {n_deg}",
            "#define CSR_VALUE PACE_DEGREE",
            f"#define ENABLE_{prec} 1",
            f"#define CHANNELS {channels}",
            f"#define ELEMS {elems}",
            f"#define INPUTS_LEN {elems * channels}",
            f"#define PARAMS_LEN {len(params)}",
            f"#define INV_N 0x{inv_n_bits:08x}u  // 1/{elems} as an FP32 bit pattern"]
    out += [f"typedef {hex_ctype} data_t;", "typedef uint32_t param_t;"]
    out += [format_array_declaration(f"extern {hex_ctype}", "ifmap", (elems * channels,))]
    out += [format_array_declaration(hex_ctype, "ofmap", (elems * channels,))]
    out += [format_array_definition("uint32_t", "params", params, hex_format=True)]
    out += [format_array_definition(ctype, "ifmap", x.reshape(-1), hex_format=True)]
    out += [format_array_definition(ctype, "golden", ofmap.reshape(-1), hex_format=True)]
    return "\n\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-c", "--cfg", type=Path, required=True)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    args.output.write_text(emit(**json.loads(args.cfg.read_text())))


if __name__ == "__main__":
    main()
