# PACE interactive session.
#
# Loads the design, adds the waveform set, and stops on the first PACE operation
# so the cursor lands where there is something to see. Unlike pace_signals.tcl
# and pace_datapath.tcl this never quits -- it hands the session back to you.
#
#   PACE_TEST=pace_pwpa questa-2023.4-zr vsim -gui -do sw/pace/probe/pace_gui.tcl
#
# Expects the test to have been built first:
#   cd sw/pace/pace_pwpa && USE_CV32E40P=1 make clean all

if {![info exists ::env(PACE_TEST)]} { set ::env(PACE_TEST) pace_pwpa }

set root [pwd]
set APP $root/sw/pace/$::env(PACE_TEST)/build/test/test

vsim +permissive -suppress 3053 -suppress 8885 -suppress 12130 -suppress 7077 \
  -lib $root/work +APP=$APP +notimingchecks +nospecify -t 1ps \
  pulp_cluster_tb_optimized +permissive-off ++$APP

set FPW /pulp_cluster_tb/cluster_i/i_fp_wrapper_core0
set L0 $FPW/i_fpnew_bulk/gen_operation_groups\[0\]/i_opgroup_block/gen_merged_slice/i_multifmt_slice/gen_num_lanes\[0\]/active_lane/gen_lane_instance/gen_pace_instance/i_fpnew_pace_fma_multi

do sw/pace/probe/pace_waves.do

# The per-lane datapath groups now live in pace_waves.do, so the interactive
# session, the VCD dump and the batch probe all describe the same signals.

# Stop at the first PACE operation instead of running to the end. Everything
# before this is boot and the coefficient stores.
when -label first_pace "$FPW/pace_is_op == 1'b1 && $FPW/apu_req_i == 1'b1" {
  echo ""
  echo "  stopped on the first PACE operation at t=\$now"
  echo "  the batch probe reports this one as: part_idx 1, latency 5 cycles"
  echo "  step with  run 100ns  /  run -all  to continue"
  echo ""
  nowhen first_pace
  stop
}

run -all
