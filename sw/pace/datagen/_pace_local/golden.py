#!/usr/bin/env python3
# Copyright 2023 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Arpan Suravi Prasad <prasadar@iis.ee.ethz.ch>

import torch
import numpy as np

def silu(x):
    x_t = torch.from_numpy(np.asarray(x))
    y_t = torch.nn.functional.silu(x_t)
    return y_t.numpy()

def exp(x):
    x_t = torch.from_numpy(np.asarray(x))
    y_t = torch.exp(x_t)
    return y_t.numpy()

def inv(x):
    x_t = torch.from_numpy(np.asarray(x))
    y_t = 1 / x_t
    return y_t.numpy()

def sqrt(x):
    x_t = torch.from_numpy(np.asarray(x))
    y_t = torch.sqrt(x_t)
    return y_t.numpy()

def rsqrt(x):
    x_t = torch.from_numpy(np.asarray(x))
    y_t = torch.rsqrt(x_t)
    return y_t.numpy()

def tanh(x):
    x_t = torch.from_numpy(np.asarray(x))
    y_t = torch.tanh(x_t)
    return y_t.numpy()

def sigmoid(x):
    x_t = torch.from_numpy(np.asarray(x))
    y_t = torch.sigmoid(x_t)
    return y_t.numpy()

def gelu(x):
    x_t = torch.from_numpy(np.asarray(x))
    y_t = torch.nn.functional.gelu(x_t)
    return y_t.numpy()    # back to numpy

ACTIVATIONS = {
    "silu": silu,
    "exp": exp,
    "inv": inv,
    "sqrt": sqrt,
    "rsqrt": rsqrt,
    "gelu": gelu,
    "tanh": tanh,
    "sigmoid": sigmoid
}

def _in_fp32(fn, x):
    """Evaluate in FP32 and cast back to the input dtype.

    torch 1.10 CPU has no Half kernel for exp, rsqrt or gelu, so an FP16 ifmap fails for
    every activation except inv (which is a division). Widening also makes the reference
    the correctly-rounded FP32 value rather than an FP16-internal computation.
    """
    x = np.asarray(x)
    y = fn(x.astype(np.float32))
    return np.asarray(y, dtype=x.dtype)


def golden_model(ifmap, fn_name):
    if fn_name not in ACTIVATIONS:
        raise ValueError(f"Unknown activation '{fn_name}'. "
                         f"Available: {list(ACTIVATIONS.keys())}")

    fn = ACTIVATIONS[fn_name]
    return _in_fp32(fn, ifmap)