# kvpace build. Targets: all, test, clean, format.
#   make SAN=1      build with AddressSanitizer and UndefinedBehaviorSanitizer
ARCH := $(shell uname -m)
GIT := $(shell git describe --always --dirty 2>/dev/null || echo unknown)

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu2x -Wall -Wextra -Werror -D_GNU_SOURCE -DPACE_GIT=\"$(GIT)\" -Isrc
ifdef SAN
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif
LDLIBS += -lpthread -lm

# Optional native driver for the arenakv server (needs ../arenakv and ../rdmaduct).
AKV_DIR ?= ../arenakv
DUCT_DIR ?= ../rdmaduct
NATIVE ?= $(if $(wildcard $(AKV_DIR)/include/akvclient.h),1,0)
HAVE_RDMA_HEADERS := $(shell test -f /usr/include/infiniband/verbs.h && test -f /usr/include/rdma/rdma_cma.h && echo 1)
# RDMA=0 links the native driver against the TCP-only builds of arenakv and rdmaduct, in separate directories.
# DEP_OUT matches the output directories of both makefiles, which receive SAN through the command line.
RDMA ?= $(if $(HAVE_RDMA_HEADERS),1,0)
DEP_OUT := build/$(ARCH)$(if $(SAN),-san)$(if $(filter 0,$(RDMA)),-tcp)
DEP_ARGS := $(if $(filter 0,$(RDMA)),RDMA=0)
OUT := build/$(ARCH)$(if $(SAN),-san)$(if $(filter 0,$(RDMA)),-tcp)
ifeq ($(NATIVE),1)
CFLAGS += -DPACE_HAVE_NATIVE -I$(AKV_DIR)/include -I$(DUCT_DIR)/include
AKV_LIB := $(AKV_DIR)/$(DEP_OUT)/libarenakv.a
DUCT_LIB := $(DUCT_DIR)/$(DEP_OUT)/librdmaduct.a
ifeq ($(RDMA),1)
NATIVE_LIBS := $(AKV_LIB) $(DUCT_LIB) -lrdmacm -libverbs
else
NATIVE_LIBS := $(AKV_LIB) $(DUCT_LIB)
endif
EXTRA_SRC := src/drv_native.c
EXTRA_DEP := $(AKV_LIB) $(DUCT_LIB)
endif

LIB_SRC := src/hist.c src/zipf.c src/netbuf.c src/driver.c src/drv_resp.c src/drv_memcached.c $(EXTRA_SRC) src/runner.c src/report.c src/proc.c src/kvcache.c
LIB_OBJ := $(patsubst src/%.c,$(OUT)/%.o,$(LIB_SRC))
TESTS := $(patsubst tests/%.c,$(OUT)/%,$(wildcard tests/test_*.c))

all: $(OUT)/kvpace

$(OUT)/kvpace: $(OUT)/main.o $(LIB_OBJ) $(EXTRA_DEP)
	$(CC) $(LDFLAGS) -o $@ $(OUT)/main.o $(LIB_OBJ) $(NATIVE_LIBS) $(LDLIBS)

$(AKV_LIB):
	$(MAKE) -C $(AKV_DIR) $(DEP_ARGS) $(DEP_OUT)/libarenakv.a

$(DUCT_LIB):
	$(MAKE) -C $(DUCT_DIR) $(DEP_ARGS) $(DEP_OUT)/librdmaduct.a

$(OUT)/%.o: src/%.c $(wildcard src/*.h) | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test_%: tests/test_%.c $(LIB_OBJ) $(EXTRA_DEP) $(wildcard src/*.h)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJ) $(NATIVE_LIBS) $(LDLIBS)

$(OUT):
	mkdir -p $@

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done

format:
	clang-format -i src/*.c src/*.h tests/*.c

clean:
	rm -rf build

.PHONY: all test format clean
