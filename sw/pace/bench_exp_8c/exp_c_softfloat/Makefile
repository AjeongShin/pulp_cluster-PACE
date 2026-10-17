PULP_APP = test
PULP_APP_FC_SRCS = test.c
PULP_APP_HOST_SRCS = test.c
PULP_CFLAGS = -O3 -g

# Test data: exp over [-11, 0], 1024 elements (../data). Point DATA_DIR at another
# data.h to rerun on other data; see ../README.md for regenerating it.
DATA_DIR ?= ../data/fp32
PULP_CFLAGS += -I$(abspath $(DATA_DIR))
# expf source and host reference (expc_ref.h) are shared with ../exp_c_fpu; regenerate
# the reference there with `make ref`.
PULP_CFLAGS += -I$(abspath ../exp_c_fpu)

# expf is built without the F extension, so every float operation is a call into
# libgcc's integer-only soft-float routines and the FPU is never used.
RVF_PREFIX ?= /scratch/ajeong/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-
RVF_LIBGCC := $(shell $(RVF_PREFIX)gcc -march=rv32imc_zicsr -mabi=ilp32 -print-libgcc-file-name)

include $(PULP_SDK_HOME)/install/rules/pulp_rt.mk

$(TARGET_BUILD_DIR)/test/test: $(CURDIR)/expf_softfloat.o

# ld -r pulls the soft-float helpers out of libgcc so the firmware link does not have
# to choose a libgcc. Constant sections are renamed so the linker script places them
# in L1.
$(CURDIR)/expf_softfloat.o: ../exp_c_fpu/expf_rvf.c
	$(RVF_PREFIX)gcc -march=rv32imc_zicsr -mabi=ilp32 -O3 -mno-relax -ffreestanding -c $< -o expf_softfloat_only.o
	$(RVF_PREFIX)ld -r -o $@ expf_softfloat_only.o $(RVF_LIBGCC)
	$(RVF_PREFIX)objcopy \
	  --rename-section .srodata.cst4=.data_l1.expf_cst4,alloc,load,data,contents \
	  --rename-section .srodata=.data_l1.expf_tab,alloc,load,data,contents \
	  --rename-section .rodata.__divsf3=.data_l1.divsf3_ro,alloc,load,data,contents \
	  --rename-section .rodata.__clz_tab=.data_l1.clz_tab,alloc,load,data,contents \
	  --remove-section .riscv.attributes --remove-section .comment $@
	rm -f expf_softfloat_only.o
