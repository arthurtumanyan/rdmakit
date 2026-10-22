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
# Hardening (HARDEN=0 disables): fortified libc calls (not with the sanitizers, whose runtimes replace
# them), stack protector, stack clash protection, control-flow protection (x86-64: CET; aarch64: PAC and
# BTI), position-independent executables, read-only relocations resolved at start, non-executable stack.
HARDEN ?= 1
ifeq ($(HARDEN),1)
CFLAGS += -fstack-protector-strong -fstack-clash-protection -fPIE
ifeq ($(ARCH),x86_64)
CFLAGS += -fcf-protection
endif
ifeq ($(ARCH),aarch64)
CFLAGS += -mbranch-protection=standard
endif
ifeq ($(SAN)$(TSAN),)
CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
endif
LDFLAGS += -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
endif
ifdef SAN
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
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

# Coverage-guided fuzzing with libFuzzer (clang): make fuzz [FUZZ_TIME=seconds per harness, default 60].
# -print_funcs=0: libFuzzer would otherwise start the symbolizer for every newly covered function, and the
# sanitizer runtime closes every possible descriptor (up to RLIMIT_NOFILE) before it starts a process.
# Harnesses in tests/fuzz run on the TCP transport under AddressSanitizer and UndefinedBehaviorSanitizer.
FUZZ_CC ?= clang
FUZZ_TIME ?= 60
FUZZ_OUT := build/fuzz
FUZZ_FLAGS := -std=gnu2x -g -O1 -Wall -Wextra -Werror -D_GNU_SOURCE -Iinclude -Isrc \
	-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
FUZZ_SRC := src/hello.c src/duct.c src/tcp.c
FUZZERS := $(patsubst tests/fuzz/%.c,$(FUZZ_OUT)/%,$(wildcard tests/fuzz/fuzz_*.c))

$(FUZZ_OUT)/fuzz_%: tests/fuzz/fuzz_%.c $(FUZZ_SRC) $(wildcard src/*.h include/*.h)
	@mkdir -p $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_FLAGS) -o $@ $< $(FUZZ_SRC) -lpthread

fuzz: $(FUZZERS)
	@set -e; for f in $(FUZZERS); do \
		n=$${f##*/fuzz_}; c=$(FUZZ_OUT)/corpus-$$n; log=$(FUZZ_OUT)/$$n.log; mkdir -p $$c; \
		if [ -d tests/fuzz/corpus/$$n ]; then cp tests/fuzz/corpus/$$n/* $$c/; fi; \
		echo "== fuzz_$$n ($(FUZZ_TIME) s)"; \
		if ! $$f -max_total_time=$(FUZZ_TIME) -timeout=10 -rss_limit_mb=4096 -print_funcs=0 -print_final_stats=1 -artifact_prefix=$(FUZZ_OUT)/$$n- $$c >$$log 2>&1; then \
			tail -n 80 $$log; exit 1; fi; \
		grep -E '^stat::(number_of_executed_units|peak_rss_mb|new_units_added)' $$log | tr '\n' ' '; echo; \
	done

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
	clang-format -i src/*.c src/*.h include/*.h tools/*.c examples/*.c tests/*.c tests/*.h tests/fuzz/*.c

clean:
	rm -rf build

.PHONY: fuzz all test test-rdma format clean
