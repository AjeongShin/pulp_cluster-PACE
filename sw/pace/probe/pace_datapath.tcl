# PACE datapath probe (batch).
#
# The bit-exact tests say the answer is right. pace_signals.tcl says the
# coefficients take the path we think they take. This one says the arithmetic
# unit does the work we think it does: it counts the Horner iterations, follows
# the partition index, and measures the APU handshake latency and the issue
# rate.
#
#   PACE_TEST=pace_pwpa questa-2023.4-zr vsim -c -do sw/pace/probe/pace_datapath.tcl
#
# Expects the test to have been built first:
#   cd sw/pace/pace_pwpa && USE_CV32E40P=1 make clean all

if {![info exists ::env(PACE_TEST)]} { set ::env(PACE_TEST) pace_pwpa }
if {[info exists ::env(PACE_OPS)]}   { set ::detail $::env(PACE_OPS) } else { set ::detail 4 }

set root [pwd]
set APP $root/sw/pace/$::env(PACE_TEST)/build/test/test

vsim +permissive -suppress 3053 -suppress 8885 -suppress 12130 -suppress 7077 \
  -lib $root/work +APP=$APP +notimingchecks +nospecify -t 1ps \
  pulp_cluster_tb_optimized +permissive-off ++$APP

set FPW /pulp_cluster_tb/cluster_i/i_fp_wrapper_core0
# Lane 0 always runs. Lane 1 only exists when Xfvec is elaborated, and only
# fills when the instruction is vectorial -- a scalar test leaves it idle, a
# vpace test should show it moving in lockstep with lane 0.
set L0 $FPW/i_fpnew_bulk/gen_operation_groups\[0\]/i_opgroup_block/gen_merged_slice/i_multifmt_slice/gen_num_lanes\[0\]/active_lane/gen_lane_instance/gen_pace_instance/i_fpnew_pace_fma_multi
set L1 $FPW/i_fpnew_bulk/gen_operation_groups\[0\]/i_opgroup_block/gen_merged_slice/i_multifmt_slice/gen_num_lanes\[1\]/active_lane/gen_lane_instance/gen_pace_instance/i_fpnew_pace_fma_multi
set ::have_l1 [expr {![catch {examine $L1/horn_fb}]}]

set ::cyc         0
set ::ops         0
set ::res         0
set ::issue_cyc   -1
set ::prev_issue  -1
set ::lat_sum     0
set ::lat_min     9999
set ::lat_max     0
set ::gap_sum     0
set ::gap_n       0
set ::horner      0
set ::horner_max  0
set ::parts_seen  {}
set ::l1_active_ops 0
set ::l1_horner_max 0
set ::l1_horner   0

proc hx {sig} { if {[catch {examine -hex $sig} v]} { return "?" } ; return $v }
proc dv {sig} { if {[catch {examine -decimal $sig} v]} { return "?" } ; return $v }

when "/pulp_cluster_tb/s_clk == 1'b1" {
  incr ::cyc

  # --- issue: the cycle the APU accepts a PACE operation -------------------
  if {[examine $FPW/pace_is_op] eq "1'h1" && [examine $FPW/apu_req_i] eq "1'h1" \
      && [examine $FPW/apu_gnt_o] eq "1'h1"} {
    incr ::ops
    set ::issue_cyc $::cyc
    set ::horner 0
    set ::l1_horner 0
    if {$::have_l1 && [examine $L1/pace_op] eq "1'h1"} { incr ::l1_active_ops }
    if {$::prev_issue >= 0} {
      incr ::gap_sum [expr {$::cyc - $::prev_issue}]
      incr ::gap_n
    }
    set ::prev_issue $::cyc
    # part_idx is a 4-bit vector: -decimal renders it signed, so -4'd1 is really
    # partition 15. Read it unsigned or the upper half aliases onto the lower.
    set p [examine -radix unsigned $L0/part_idx]
    regexp {([0-9]+)$} $p -> p
    if {[lsearch -exact $::parts_seen $p] < 0} { lappend ::parts_seen $p }
    if {$::ops <= $::detail} {
      echo "@ISSUE #$::ops  cyc=$::cyc"
      echo "   op_i        = [examine $FPW/i_fpnew_bulk/op_i]"
      echo "   pace_mode   = [examine $FPW/pace_mode]  (extend, enable, degree)"
      echo "   operand\[0\]  = [hx $FPW/apu_operands_i\[0\]]"
      echo "   part_idx    = $p"
    }
  }

  # --- Horner: each feedback beat is one multiply-add of the chain ---------
  if {[examine $L0/horn_fb] eq "1'h1"} {
    incr ::horner
    if {$::horner > $::horner_max} { set ::horner_max $::horner }
    if {$::ops <= $::detail} {
      echo "   .. horner beat $::horner  cyc=$::cyc  out_tag.degree=[dv $L0/out_tag.degree]"
    }
  }
  if {$::have_l1 && [examine $L1/horn_fb] eq "1'h1"} {
    incr ::l1_horner
    if {$::l1_horner > $::l1_horner_max} { set ::l1_horner_max $::l1_horner }
    if {$::ops <= $::detail} {
      echo "   .. lane1 horner beat $::l1_horner  cyc=$::cyc  out_tag.degree=[dv $L1/out_tag.degree]"
    }
  }

  # --- retire: result handed back to the core ------------------------------
  if {[examine $FPW/apu_rvalid_o] eq "1'h1" && $::issue_cyc >= 0} {
    incr ::res
    set lat [expr {$::cyc - $::issue_cyc}]
    incr ::lat_sum $lat
    if {$lat < $::lat_min} { set ::lat_min $lat }
    if {$lat > $::lat_max} { set ::lat_max $lat }
    if {$::res <= $::detail} {
      echo "@RETIRE #$::res  cyc=$::cyc  latency=$lat cyc  rdata=[hx $FPW/apu_rdata_o]"
    }
    set ::issue_cyc -1
  }
}

# The testbench ends with $finish; stop there instead of quitting so the
# summary below still runs.
onfinish stop
run -all

echo ""
echo "=============== PACE datapath summary : $::env(PACE_TEST) ==============="
echo "  PACE operations issued        : $::ops"
echo "  results retired               : $::res"
if {$::res > 0} {
  echo "  issue -> rvalid latency       : min $::lat_min  max $::lat_max  mean [format %.2f [expr {double($::lat_sum)/$::res}]] cycles"
}
if {$::gap_n > 0} {
  echo "  cycles between issues         : mean [format %.2f [expr {double($::gap_sum)/$::gap_n}]]  (one PACE call in the software loop)"
}
echo "  max Horner beats in one op    : $::horner_max"
echo "  distinct partitions exercised : [llength $::parts_seen] of 16   -> [lsort -integer -unique $::parts_seen]"
if {$::have_l1} {
  echo "  lane 1 elaborated             : yes"
  echo "  lane 1 pace_op asserted       : $::l1_active_ops of $::ops issues"
  echo "  lane 1 max Horner beats       : $::l1_horner_max"
} else {
  echo "  lane 1 elaborated             : no (scalar-only build)"
}
echo "========================================================================"
quit -f
