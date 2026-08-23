# PACE waveform set, for the interactive session.
#
#   cd sw/pace/pace_sqrt
#   USE_CV32E40P=1 make clean all run gui=1
#   # then, at the vsim prompt:
#   do ../probe/pace_waves.do
#   run -all

set MEM /pulp_cluster_tb/cluster_i/no_hwpe_gen/i_pace_param_mem
set FPW /pulp_cluster_tb/cluster_i/i_fp_wrapper_core0

add wave -divider "PACE coefficient bank"
add wave -hex $MEM/periph_slave/req $MEM/periph_slave/add $MEM/periph_slave/wdata
add wave -hex $MEM/param_q
add wave -hex $MEM/pace_param_o

add wave -divider "coefficient bus at the FPU"
add wave -hex $FPW/pace_param

add wave -divider "PACE mode and operation"
add wave      $FPW/pace_mode_i
add wave      $FPW/pace_mode
add wave      $FPW/pace_is_op
add wave      $FPW/i_fpnew_bulk/op_i

add wave -divider "APU handshake"
add wave      $FPW/apu_req_i $FPW/apu_gnt_o $FPW/apu_rvalid_o
add wave -hex $FPW/apu_operands_i
add wave -hex $FPW/apu_rdata_o

configure wave -namecolwidth 260
configure wave -valuecolwidth 120
