# rdmaduct build. Targets: all, test, test-rdma, clean, format.
#   make RDMA=0      build the TCP transport only (no libibverbs / librdmacm needed)
#   make SAN=1       build with AddressSanitizer and UndefinedBehaviorSanitizer
#   make test-rdma   run the RDMA suite; ADDR selects the local address of the RDMA device
ARCH := $(shell uname -m)
OUT := build/$(ARCH)$(if $(SAN),-san)$(if $(filter 0,$(RDMA)),-tcp)

HAVE_RDMA_HEADERS := $(shell test -f /usr/include/infiniband/verbs.h && test -f /usr/include/rdma/rdma_cma.h && echo 1)
RDMA ?= $(if $(HAVE_RDMA_HEADERS),1,0)

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu2x -Wall -Wextra -Werror -D_GNU_SOURCE -Iinclude -Isrc
LDLIBS += -lpthread
ifeq ($(RDMA),1)
CFLAGS += -DDUCT_HAVE_RDMA
LDLIBS += -lrdmacm -libverbs
endif
ifdef SAN
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

LIB_SRC := src/hello.c src/duct.c src/tcp.c $(if $(filter 1,$(RDMA)),src/rdma.c)
LIB_OBJ := $(patsubst src/%.c,$(OUT)/%.o,$(LIB_SRC))
LIB := $(OUT)/librdmaduct.a
TESTS := $(patsubst tests/%.c,$(OUT)/%,$(wildcard tests/test_*.c))

# Local address of the first RDMA device's network interface, used by the RDMA tests.
ADDR ?= $(shell rdma link show 2>/dev/null | sed -n 's/.*netdev \([^ ]*\).*/\1/p' | head -1 | xargs -r -I{} ip -4 -o addr show dev {} | awk '{print $$4}' | cut -d/ -f1 | head -1)

all: $(LIB) $(OUT)/duct_echo $(OUT)/duct_bench $(OUT)/hello_server $(OUT)/hello_client

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(OUT)/%.o: src/%.c $(wildcard src/*.h include/*.h) | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/duct_echo: tools/duct_echo.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

$(OUT)/hello_%: examples/hello_%.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

$(OUT)/duct_bench: tools/duct_bench.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

$(OUT)/test_%: tests/test_%.c $(LIB) $(wildcard tests/*.h)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

$(OUT):
	mkdir -p $@

test: $(TESTS) $(OUT)/hello_server $(OUT)/hello_client
	@set -e; for t in $(TESTS); do echo "== $$t"; DUCT_TEST_ADDR="$(ADDR)" $$t; done
	@echo "== examples"
	@if [ "$(RDMA)" = "1" ]; then touch $(OUT)/.rdma_enabled; else rm -f $(OUT)/.rdma_enabled; fi
	@DUCT_TEST_ADDR="$(ADDR)" tests/run_examples.sh $(OUT)

test-rdma: $(OUT)/test_rdma
	DUCT_TEST_ADDR="$(ADDR)" $(OUT)/test_rdma

format:
	clang-format -i src/*.c src/*.h include/*.h tools/*.c examples/*.c tests/*.c tests/*.h

clean:
	rm -rf build

.PHONY: all test test-rdma format clean
