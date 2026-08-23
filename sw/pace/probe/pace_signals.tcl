# PACE signal-propagation probe (batch).
#
# Confirms that the coefficients written by software actually reach the
# arithmetic unit, that the mode fields carry the CSR value, and that the APU
# handshake completes. Bit-exact test results say the answer is right; this says
# the value took the path we think it takes.
#
#   questa-2023.4-zr vsim -c -do sw/pace/probe/pace_signals.tcl
#
# Expects a test to have been built first, e.g.
#   cd sw/pace/pace_sqrt && USE_CV32E40P=1 make clean all

if {![info exists ::env(PACE_TEST)]} { set ::env(PACE_TEST) pace_sqrt }
set root [pwd]
set APP $root/sw/pace/$::env(PACE_TEST)/build/test/test

vsim +permissive -suppress 3053 -suppress 8885 -suppress 12130 -suppress 7077 \
  -lib $root/work +APP=$APP +notimingchecks +nospecify -t 1ps \
  pulp_cluster_tb_optimized +permissive-off ++$APP

set MEM /pulp_cluster_tb/cluster_i/no_hwpe_gen/i_pace_param_mem
set FPW /pulp_cluster_tb/cluster_i/i_fp_wrapper_core0

set ::seen_bank 0
set ::seen_op   0
set ::seen_res  0

when "/pulp_cluster_tb/s_clk == 1'b1" {
  # The coefficient bank and every stage of the bus that carries it, sampled on
  # the first PACE operation so the writes have completed.
  if {$::seen_bank == 0 && [examine $FPW/pace_is_op] eq "1'h1"} {
    set ::seen_bank 1
    echo "@BANK   t=$now"
    echo "   param_q\[0..2\]        = [examine -hex $MEM/param_q\[0\]] [examine -hex $MEM/param_q\[1\]] [examine -hex $MEM/param_q\[2\]]"
    echo "   pace_param_o\[31:0\]   = [examine -hex $MEM/pace_param_o\[31:0\]]"
    echo "   wrapper pace_param    = [examine -hex $FPW/pace_param\[31:0\]]"
    echo "   fpnew   pace_param_i  = [examine -hex $FPW/i_fpnew_bulk/pace_param_i\[31:0\]]"
  }
  # The resolved operation, the mode fields and the APU handshake.
  if {$::seen_op < 3 && [examine $FPW/pace_is_op] eq "1'h1" && [examine $FPW/apu_req_i] eq "1'h1"} {
    incr ::seen_op
    echo "@OP#$::seen_op t=$now"
    echo "   op_i       = [examine $FPW/i_fpnew_bulk/op_i]"
    echo "   pace_mode  = [examine $FPW/pace_mode]   (extend, enable, degree)"
    echo "   operand\[0\] = [examine -hex $FPW/apu_operands_i\[0\]]"
    echo "   req/gnt    = [examine $FPW/apu_req_i]/[examine $FPW/apu_gnt_o]"
  }
  if {$::seen_res < 3 && [examine $FPW/apu_rvalid_o] eq "1'h1"} {
    incr ::seen_res
    echo "@RES#$::seen_res t=$now  rdata = [examine -hex $FPW/apu_rdata_o]"
  }
}

run -all
echo "@@ probe done"
quit -f
