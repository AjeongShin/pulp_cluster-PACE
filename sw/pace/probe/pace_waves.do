# PACE waveform set, shared by the interactive session and the VCD dump.
#
#   cd sw/pace/pace_sqrt
#   USE_CV32E40P=1 make clean all run gui=1
#   # then, at the vsim prompt:
#   do ../probe/pace_waves.do
#   run -all
#
# The groups follow the operand from software to result, so reading the window
# top to bottom is reading the datapath left to right:
#
#   coefficient bank -> coefficient bus -> mode/op -> APU handshake
#     -> frexp -> partition -> coefficient select -> Horner -> ldexp -> result
#
# The first four groups sit at the FPU boundary and are format-independent. The
# per-lane groups are the inside of fpnew_pace_fma_multi, one instance per SIMD
# lane: lane 0 always runs, lane 1 only fills for the vectorial form, so a scalar
# test shows it idle and a vpace test shows both moving together.

set MEM /pulp_cluster_tb/cluster_i/no_hwpe_gen/i_pace_param_mem
set FPW /pulp_cluster_tb/cluster_i/i_fp_wrapper_core0

set LANE_BASE $FPW/i_fpnew_bulk/gen_operation_groups\[0\]/i_opgroup_block/gen_merged_slice/i_multifmt_slice/gen_num_lanes
set LANE_TAIL active_lane/gen_lane_instance/gen_pace_instance/i_fpnew_pace_fma_multi

# ---------------------------------------------------------------- boundary ---

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

# ------------------------------------------------------------------- lanes ---
#
# Everything below is wrapped in `catch` because the lane count and the pace
# instance both depend on elaboration: a build without Xfvec has only lane 0, and
# a build with PaceFeatures.FmtConfig = 0 has no pace instance at all. A missing
# signal should leave the rest of the window intact rather than abort the script.

proc pace_add_lane {base tail lane} {
    set L $base\[$lane\]/$tail

    add wave -divider "lane $lane -- into the unit"
    catch { add wave        $L/pace_op }
    catch { add wave        $L/pace_mode_i }
    catch { add wave -hex   $L/operands_i }
    catch { add wave        $L/in_valid_i $L/in_ready_o }

    # frexp splits the operand into a mantissa inside the fit range and the
    # exponent it took out. Only inv/sqrt/rsqrt use it; for pwpa the operand
    # passes through untouched.
    add wave -divider "lane $lane -- frexp (range reduction)"
    catch { add wave -hex   $L/i_pace_frexp/operand_i }
    catch { add wave -hex   $L/i_pace_frexp/operand_o }
    catch { add wave -hex   $L/i_pace_frexp/frexp_info_o }
    catch { add wave        $L/i_pace_frexp/op_i }
    catch { add wave        $L/i_pace_frexp/src_fmt_i }

    # The partition detector is a binary search tree over the breakpoints.
    # bound_id_o is which of the intervals the operand landed in.
    add wave -divider "lane $lane -- partition (BST)"
    catch { add wave -hex   $L/i_partition_detector/bounds_i }
    catch { add wave -hex   $L/i_partition_detector/operand_i }
    catch { add wave -decimal $L/i_partition_detector/bound_id_o }
    catch { add wave        $L/i_partition_detector/out_valid_o }
    catch { add wave -decimal $L/part_idx }

    # Given the partition index and the current Horner step, this hands the FMA
    # the two coefficients it needs.
    add wave -divider "lane $lane -- coefficient select"
    catch { add wave -decimal $L/i_coeff_selector/bound_i }
    catch { add wave -decimal $L/i_coeff_selector/degree_i }
    catch { add wave -hex   $L/i_coeff_selector/coeffs_o }

    # Horner feeds the FMA result back into its own input, one pass per degree.
    # horn_act marks a step, horn_fb the feedback, out_tag.degree counts them.
    add wave -divider "lane $lane -- Horner"
    catch { add wave        $L/horn_act }
    catch { add wave        $L/horn_fb }
    catch { add wave -hex   $L/horn_ops }
    catch { add wave -hex   $L/in_tag }
    catch { add wave -hex   $L/out_tag }
    catch { add wave        $L/fma_round_mode }
    catch { add wave        $L/pin_vld $L/pout_vld }

    # ldexp puts back the exponent frexp removed, with the correction the chosen
    # operation needs: negated for inv, halved for sqrt, both for rsqrt.
    add wave -divider "lane $lane -- ldexp (rescale) and result"
    catch { add wave -hex   $L/i_pace_ldexp/operand_i }
    catch { add wave -decimal $L/i_pace_ldexp/exponent_i }
    catch { add wave        $L/i_pace_ldexp/sign_i }
    catch { add wave        $L/i_pace_ldexp/op_inv_i $L/i_pace_ldexp/op_sqrt_i $L/i_pace_ldexp/op_rsqrt_i }
    catch { add wave -hex   $L/i_pace_ldexp/operand_o }
    catch { add wave -hex   $L/scaled_result }
    catch { add wave -hex   $L/fma_res }
    catch { add wave -hex   $L/result_o }
}

pace_add_lane $LANE_BASE $LANE_TAIL 0
pace_add_lane $LANE_BASE $LANE_TAIL 1

configure wave -namecolwidth 320
configure wave -valuecolwidth 140
