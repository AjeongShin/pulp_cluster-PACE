// Copyright 2026 ETH Zurich and University of Bologna.
// Copyright and related rights are licensed under the Solderpad Hardware
// License, Version 0.51 (the "License"); you may not use this file except in
// compliance with the License.  You may obtain a copy of the License at
// http://solderpad.org/licenses/SHL-0.51. Unless required by applicable law
// or agreed to in writing, software, hardware and materials distributed under
// this License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

// Description: PACE coefficient memory for the fpnew extension.
//              It stores memory-mapped coefficient words written by the cores and exposes the full bank to the FPU as a flat bus.

module pace_param_mem #(
  parameter int unsigned PACE_PARAM_WIDTH = 2080
) (
  input logic clk_i,
  input logic rst_ni,
  XBAR_PERIPH_BUS.Slave periph_slave,
  output logic [PACE_PARAM_WIDTH-1:0] pace_param_o
);

  localparam int unsigned NumWords = PACE_PARAM_WIDTH / 32;
  localparam int unsigned IdxBits = 7;   // add[8:2] indexes up to 128 words.

  logic [NumWords-1:0][31:0] param_q;

  logic [IdxBits-1:0] word_idx;
  logic               idx_valid;
  logic               do_write;
  logic               do_read;

  logic        rvalid_q;
  logic [31:0] rdata_q;
  logic [$bits(periph_slave.id)-1:0] id_q;

  assign word_idx  = periph_slave.add[IdxBits+1:2];
  assign idx_valid = (word_idx < NumWords);

  assign do_write = periph_slave.req & ~periph_slave.wen & idx_valid;
  assign do_read  = periph_slave.req &  periph_slave.wen;

  // Coefficient bank
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      param_q <= '0;
    end else if (do_write) begin
      for (int unsigned b = 0; b < 4; b++) begin
        if (periph_slave.be[b]) begin
          param_q[word_idx][8*b+:8] <= periph_slave.wdata[8*b+:8];
        end
      end
    end
  end

  // Response channel
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      rvalid_q <= 1'b0;
      rdata_q  <= '0;
      id_q     <= '0;
    end else begin
      rvalid_q <= periph_slave.req;
      if (periph_slave.req) begin
        id_q <= periph_slave.id;
      end
      if (do_read) begin
        rdata_q <= idx_valid ? param_q[word_idx] : '0;
      end
    end
  end

  assign periph_slave.gnt     = 1'b1;
  assign periph_slave.r_valid = rvalid_q;
  assign periph_slave.r_rdata = rdata_q;
  assign periph_slave.r_opc   = 1'b0;
  assign periph_slave.r_id    = id_q;

  assign pace_param_o = param_q;

endmodule  // pace_param_mem
