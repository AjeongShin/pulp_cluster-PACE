# PACE waveform dump, no GUI.
#
# Runs headless to the first PACE operation, then records a short window of the
# datapath to a VCD file. Viewing it locally beats forwarding a Questa GUI over
# X: the file is small and the rendering happens on your own machine.
#
#   PACE_TEST=pace_pwpa questa-2023.4-zr vsim -c -do sw/pace/probe/pace_vcd.tcl
#
# Then, on your laptop:
#   scp sem26f53@badile05:/scratch/ajeong/pulp_cluster-PACE/pace_pwpa.vcd .
#   gtkwave pace_pwpa.vcd          # brew install --cask gtkwave
#
# PACE_WINDOW_NS sets how much to record after the first operation (default 400,
# which covers roughly nine calls of the software loop at 44 cycles each).

if {![info exists ::env(PACE_TEST)]}      { set ::env(PACE_TEST) pace_pwpa }
if {[info exists ::env(PACE_WINDOW_NS)]}  { set ::win $::env(PACE_WINDOW_NS) } else { set ::win 400 }

set root [pwd]
set APP $root/sw/pace/$::env(PACE_TEST)/build/test/test
set OUT $root/$::env(PACE_TEST).vcd

vsim +permissive -suppress 3053 -suppress 8885 -suppress 12130 -suppress 7077 \
  -lib $root/work +APP=$APP +notimingchecks +nospecify -t 1ps \
  pulp_cluster_tb_optimized +permissive-off ++$APP

set FPW /pulp_cluster_tb/cluster_i/i_fp_wrapper_core0
set MEM /pulp_cluster_tb/cluster_i/no_hwpe_gen/i_pace_param_mem
set L0 $FPW/i_fpnew_bulk/gen_operation_groups\[0\]/i_opgroup_block/gen_merged_slice/i_multifmt_slice/gen_num_lanes\[0\]/active_lane/gen_lane_instance/gen_pace_instance/i_fpnew_pace_fma_multi
# Lane 1 only exists when the vectorial lanes are elaborated; a scalar-only
# build has no such instance, so adding it is allowed to fail.
set L1 $FPW/i_fpnew_bulk/gen_operation_groups\[0\]/i_opgroup_block/gen_merged_slice/i_multifmt_slice/gen_num_lanes\[1\]/active_lane/gen_lane_instance/gen_pace_instance/i_fpnew_pace_fma_multi

# Run to the first PACE operation. Everything before it is boot and the
# coefficient stores, and recording all of it would make the file useless.
set ::armed 0
when -label arm "$FPW/pace_is_op == 1'b1 && $FPW/apu_req_i == 1'b1" {
  if {$::armed == 0} { set ::armed 1 ; nowhen arm ; stop }
}
onfinish stop
run -all

if {$::armed == 0} {
  echo "!! no PACE operation was seen -- is $::env(PACE_TEST) built?"
  quit -f
}

echo "  first PACE operation at t=$now, recording ${::win}ns"

vcd file $OUT
vcd add -r $FPW/*
vcd add -r $MEM/*
vcd add -r $L0/*
if {[catch { vcd add -r $L1/* }]} { echo "  (lane 1 not elaborated -- scalar-only build)" }
run ${::win}ns
vcd flush

echo ""
echo "  wrote $OUT"
echo "  copy it to your laptop and open it with gtkwave:"
echo "    scp sem26f53@badile05:$OUT ."
echo ""
quit -f
