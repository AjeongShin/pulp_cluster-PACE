PULP_APP = test
PULP_APP_FC_SRCS = test.c
PULP_APP_HOST_SRCS = test.c
PULP_CFLAGS = -O3 -g

# Test data: exp over [-11, 0], 1024 elements (../data). Point DATA_DIR at another
# data.h to rerun on other data; see ../README.md for regenerating it.
DATA_DIR ?= ../data/fp32
PULP_CFLAGS += -I$(abspath $(DATA_DIR))

# expf must keep its FP operands in the f-registers, which pulp-gcc cannot emit, so it
# is built by a standard RISC-V GCC and linked in as an object.
RVF_PREFIX ?= /scratch/ajeong/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-

include $(PULP_SDK_HOME)/install/rules/pulp_rt.mk

$(TARGET_BUILD_DIR)/test/test: $(CURDIR)/expf_rvf.o

# The constant sections are renamed so the linker script places them in L1.
$(CURDIR)/expf_rvf.o: expf_rvf.c
	$(RVF_PREFIX)gcc -march=rv32imfc_zicsr -mabi=ilp32 -O3 -mno-relax -ffreestanding -c $< -o $@
	$(RVF_PREFIX)objcopy \
	  --rename-section .srodata.cst4=.data_l1.expf_cst4,alloc,load,data,contents \
	  --rename-section .srodata=.data_l1.expf_tab,alloc,load,data,contents \
	  --remove-section .riscv.attributes --remove-section .comment $@

# Host reference for the result check.
ref: gen_ref.c expf_rvf.c
	gcc -O2 -ffp-contract=off -I $(DATA_DIR) gen_ref.c expf_rvf.c -lm -o gen_ref
	./gen_ref > expc_ref.h
	rm -f gen_ref
