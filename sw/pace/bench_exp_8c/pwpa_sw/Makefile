PULP_APP = test
PULP_APP_FC_SRCS = test.c
PULP_APP_HOST_SRCS = test.c
PULP_CFLAGS = -O3 -g

# Test data: exp over [-11, 0], 1024 elements (../data). Point DATA_DIR at another
# data.h to rerun on other data; see ../README.md for regenerating it.
DATA_DIR ?= ../data/fp32
PULP_CFLAGS += -I$(abspath $(DATA_DIR))

include $(PULP_SDK_HOME)/install/rules/pulp_rt.mk
