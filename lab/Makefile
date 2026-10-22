# Builds the lab steps into build/<arch>/ so that builds for different architectures can share one source tree.
#   make          steps 1 to 6 (libibverbs, librdmacm) when the RDMA headers exist, and steps 7 to 12 (rdmaduct)
#   make RDMA=0   steps 7 to 12 only, on the TCP build of rdmaduct (no RDMA libraries needed): build/<arch>-tcp/
#   make TLS=1    steps 7 to 12 on the TLS build of rdmaduct (OpenSSL), so that step 7 accepts --tls
#   make SAN=1    steps 7 to 12 with AddressSanitizer and UndefinedBehaviorSanitizer, on the sanitizer build of
#                 rdmaduct
#   make test     runs steps 7 to 12 over TCP on loopback and checks their output (tests/run.sh); in RDMA builds
#                 also over RDMA when an RDMA device is found (ADDR: the address of its interface)
#   make clean
ARCH := $(shell uname -m)
DUCT_DIR ?= ../rdmaduct
HAVE_RDMA_HEADERS := $(shell test -f /usr/include/infiniband/verbs.h && test -f /usr/include/rdma/rdma_cma.h && echo 1)
RDMA ?= $(if $(HAVE_RDMA_HEADERS),1,0)
VARIANT := $(if $(SAN),-san)$(if $(filter 0,$(RDMA)),-tcp)$(if $(filter 1,$(TLS)),-tls)
OUT := build/$(ARCH)$(VARIANT)

CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
CPPFLAGS += -I$(DUCT_DIR)/include
ifdef SAN
DUCT_CFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif
VERBS_LIBS := -libverbs -lrdmacm -lpthread

# rdmaduct is built by its own makefile into the directory that matches RDMA and TLS.
DUCT_LIB := $(DUCT_DIR)/build/$(ARCH)$(VARIANT)/librdmaduct.a
DUCT_ARGS := $(if $(filter 0,$(RDMA)),RDMA=0) $(if $(filter 1,$(TLS)),TLS=1) $(if $(SAN),SAN=1)
DUCT_LIBS := $(DUCT_LIB) -lpthread $(if $(filter 1,$(RDMA)),-lrdmacm -libverbs) $(if $(filter 1,$(TLS)),-lssl -lcrypto)

VERBS_SRCS := $(wildcard step[1-6]_*.c)
DUCT_SRCS := $(wildcard step*_duct_*.c)
VERBS_BINS := $(if $(filter 1,$(RDMA)),$(patsubst %.c,$(OUT)/%,$(VERBS_SRCS)))
DUCT_BINS := $(patsubst %.c,$(OUT)/%,$(DUCT_SRCS))

all: $(VERBS_BINS) $(DUCT_BINS)

$(VERBS_BINS): $(OUT)/%: %.c common.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(VERBS_LIBS)

$(DUCT_BINS): $(OUT)/%: %.c duct_common.h $(DUCT_LIB) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DUCT_CFLAGS) -o $@ $< $(DUCT_LIBS) -lm

$(DUCT_LIB): FORCE
	$(MAKE) -C $(DUCT_DIR) $(DUCT_ARGS) build/$(ARCH)$(VARIANT)/librdmaduct.a

$(OUT):
	mkdir -p $@

# Local address of the first RDMA device's network interface (as in ../rdmaduct/Makefile).
ADDR ?= $(if $(filter 1,$(RDMA)),$(shell rdma link show 2>/dev/null | sed -n 's/.*netdev \([^ ]*\).*/\1/p' | head -1 | xargs -r -I{} ip -4 -o addr show dev {} | awk '{print $$4}' | cut -d/ -f1 | head -1))

test: $(DUCT_BINS)
	DUCT_TEST_ADDR="$(ADDR)" tests/run.sh $(OUT)

clean:
	rm -rf build

FORCE:

.PHONY: all test clean FORCE
