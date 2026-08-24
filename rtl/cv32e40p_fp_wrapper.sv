// Copyright 2018 ETH Zurich and University of Bologna.
// Copyright and related rights are licensed under the Solderpad Hardware
// License, Version 0.51 (the "License"); you may not use this file except in
// compliance with the License.  You may obtain a copy of the License at
// http://solderpad.org/licenses/SHL-0.51. Unless required by applicable law
// or agreed to in writing, software, hardware and materials distributed under
// this License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

// Wrapper for a fpnew
// Contributor: Davide Schiavone <davide@openhwgroup.org>

module cv32e40p_fp_wrapper
  import cv32e40p_apu_core_pkg::*;
#(
    parameter FPU_ADDMUL_LAT = 0, // Floating-Point ADDition/MULtiplication computing lane pipeline registers number
    parameter FPU_OTHERS_LAT = 0, // Floating-Point COMParison/CONVersion computing lanes pipeline registers number

    // PACE geometry. The coefficient bank holds (degree+1) coefficients for each partition,
    // the partition boundaries and the two epsilon terms, each one word of PaceDataWidth.
    // Deriving the width keeps the bank, the bus and this port from drifting apart when the
    // degree or the partition count changes.
    parameter int unsigned PaceDegree    = 2,
    parameter int unsigned PaceParts     = 16,
    parameter int unsigned PaceEps       = 1,
    parameter int unsigned PaceDataWidth = 32,
    localparam int unsigned PaceParamWidth =
        ((PaceDegree + 1) * PaceParts + (PaceParts - 1) + 2 * PaceEps) * PaceDataWidth
) (
    // Clock and Reset
    input logic clk_i,
    input logic rst_ni,

    // APU Side: Master port
    input  logic apu_req_i,
    output logic apu_gnt_o,

    // request channel
    input logic [   APU_NARGS_CPU-1:0][31:0] apu_operands_i,
    input logic [     APU_WOP_CPU-1:0]       apu_op_i,
    input logic [APU_NDSFLAGS_CPU-1:0]       apu_flags_i,
    input logic [                 4:0]       pace_mode_i,
    input logic [ PaceParamWidth-1:0]       pace_param_i,

    // response channel
    output logic                        apu_rvalid_o,
    output logic [                31:0] apu_rdata_o,
    output logic [APU_NUSFLAGS_CPU-1:0] apu_rflags_o
);


  import cv32e40p_pkg::*;
  import fpnew_pkg::*;

  logic [        fpnew_pkg::OP_BITS-1:0] fpu_op;
  logic                                  fpu_op_mod;
  logic                                  fpu_vec_op;

  logic [ fpnew_pkg::FP_FORMAT_BITS-1:0] fpu_dst_fmt;
  logic [ fpnew_pkg::FP_FORMAT_BITS-1:0] fpu_src_fmt;
  logic [fpnew_pkg::INT_FORMAT_BITS-1:0] fpu_int_fmt;
  logic [                      C_RM-1:0] fp_rnd_mode;

  

  // assign apu_rID_o = '0;
  assign {fpu_vec_op, fpu_op_mod, fpu_op}                     = apu_op_i;

  assign {fpu_int_fmt, fpu_src_fmt, fpu_dst_fmt, fp_rnd_mode} = apu_flags_i;



  // -----------
  // FPU Config
  // -----------
  // Features (enabled formats, vectors etc.)
  localparam fpnew_pkg::fpu_features_t FPU_FEATURES = '{
      Width: C_FLEN,
      EnableVectors: C_XFVEC,
      EnableNanBox: 1'b0,
      FpFmtMask: {
    C_RVF, C_RVD, C_XF16, C_XF8, C_XF16ALT, 1'b0, 1'b0, 1'b0, 1'b0 // 9 bits(1'b0 == turn off)
  }, IntFmtMask: {
    C_XFVEC && C_XF8, C_XFVEC && (C_XF16 || C_XF16ALT), 1'b1, 1'b0
  }, 
    MxFpFmtMask: '0,  // CV32E40P does not support MX format
    MxIntFmtMask: '0, // CV32E40P does not support MX format
    PaceFeatures: '{ // parameters from snitch PACE code
      PaceDegree     : PaceDegree,
      PaceParts      : PaceParts,
      PaceEps        : PaceEps,
      PaceDataWidth  : PaceDataWidth,
      PaceParamWidth : PaceParamWidth,
      PaceBstPipeRegs: 4'b0100,
      FmtConfig      : {C_RVF, C_RVD, C_XF16, C_XF8, C_XF16ALT, 4'b0} // mirrors FpFmtMask
    }
  };

  // Implementation (number of registers etc)
  localparam fpnew_pkg::fpu_implementation_t FPU_IMPLEMENTATION = '{
      PipeRegs: '{  // FP32, FP64, FP16, FP8, FP16alt
      '{
          FPU_ADDMUL_LAT, C_LAT_FP64, C_LAT_FP16, C_LAT_FP8, C_LAT_FP16ALT, 0, 0, 0, 0
      },  // ADDMUL
      '{default: C_LAT_DIVSQRT},  // DIVSQRT
      '{default: FPU_OTHERS_LAT},  // NONCOMP
      '{default: FPU_OTHERS_LAT},
      '{default: 0}, // DOTP (not supported)
      '{default: 0}  // MXDOTP (not supported)
  },  // CONV
  UnitTypes: '{
      '{default: fpnew_pkg::MERGED},  // ADDMUL
      '{default: fpnew_pkg::MERGED},  // DIVSQRT
      '{default: fpnew_pkg::PARALLEL},  // NONCOMP
      '{default: fpnew_pkg::MERGED},
      '{default: fpnew_pkg::DISABLED}, // DOTP (not supported)
      '{default: fpnew_pkg::DISABLED}  // MXDOTP (not supported)
  },  // CONV
  PipeConfig: fpnew_pkg::AFTER};

  //---------------
  // PACE Config
  //---------------
  logic [PaceParamWidth-1:0]   pace_param;
  fpnew_pkg::pace_mode_t       pace_mode;
  logic                        pace_is_op;

  assign pace_param = pace_param_i;
  assign pace_is_op = (fpnew_pkg::operation_e'(fpu_op) inside {
      fpnew_pkg::PWPA, fpnew_pkg::PACE_INV, fpnew_pkg::PACE_SQRT, fpnew_pkg::PACE_RSQRT});

  // CSR_PACE (0xba0) carries the polynomial degree only; the decoder names the operation.
  assign pace_mode = '{
      extend: 1'b0,
      enable: pace_is_op,
      degree: fpnew_pkg::pace_deg_t'(pace_mode_i[2:0])
  };


  //---------------
  // FPU instance
  //---------------

  fpnew_top #(
      .Features      (FPU_FEATURES),
      .Implementation(FPU_IMPLEMENTATION),
      .DivSqrtSel    (fpnew_pkg::THMULTI),
      .TagType       (logic)
  ) i_fpnew_bulk (
      .clk_i         (clk_i),
      .rst_ni        (rst_ni),
      .hart_id_i     ('0),
      .operands_i    (apu_operands_i),
      .rnd_mode_i    (fpnew_pkg::roundmode_e'(fp_rnd_mode)),
      .pace_param_i  (pace_param),  // PACE parameters
      .pace_mode_i   (pace_mode),   // PACE modes
      .op_i          (fpnew_pkg::operation_e'(fpu_op)),
      .op_mod_i      (fpu_op_mod),
      .src_fmt_i     (fpnew_pkg::fp_format_e'(fpu_src_fmt)),
      .dst_fmt_i     (fpnew_pkg::fp_format_e'(fpu_dst_fmt)),
      .int_fmt_i     (fpnew_pkg::int_format_e'(fpu_int_fmt)),
      .vectorial_op_i(fpu_vec_op),
      .tag_i         (1'b0),
      .simd_mask_i   (1'b0),
      .in_valid_i    (apu_req_i),
      .in_ready_o    (apu_gnt_o),
      .flush_i       (1'b0),
      .result_o      (apu_rdata_o),
      .status_o      (apu_rflags_o),
      .tag_o         (  /* unused */),
      .out_valid_o   (apu_rvalid_o),
      .out_ready_i   (1'b1),
      .busy_o        (  /* unused */)
  );

endmodule  // cv32e40p_fp_wrapper
