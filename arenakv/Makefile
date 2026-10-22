# arenakv build. Targets: all, test, install, clean, format.
#   make install [PREFIX=/usr/local] [DESTDIR=]   installs arenakvd, akv_cli and akv_bench into $(DESTDIR)$(PREFIX)/bin
#   make SAN=1       AddressSanitizer + UndefinedBehaviorSanitizer
#   make TSAN=1      ThreadSanitizer
#   make TLS=1       TLS listeners for the text protocols (needs OpenSSL headers, links -lssl -lcrypto)
ARCH := $(shell uname -m)
PREFIX ?= /usr/local
CFLAGS ?= -O2 -g
GIT := $(shell git describe --always --dirty 2>/dev/null || echo unknown)
DUCT_DIR ?= ../rdmaduct
HAVE_RDMA_HEADERS := $(shell test -f /usr/include/infiniband/verbs.h && test -f /usr/include/rdma/rdma_cma.h && echo 1)
RDMA ?= $(if $(HAVE_RDMA_HEADERS),1,0)
# RDMA=0 builds go to a separate directory: they link the TCP-only transport library.
OUT := build/$(ARCH)$(if $(SAN),-san)$(if $(TSAN),-tsan)$(if $(filter 0,$(RDMA)),-tcp)$(if $(filter 1,$(TLS)),-tls)

CFLAGS += -std=gnu2x -Wall -Wextra -Werror -D_GNU_SOURCE -Iinclude -Isrc -I$(DUCT_DIR)/include
LDLIBS += -lpthread
# Must match the output directory of the rdmaduct makefile, which receives SAN through the command line.
DUCT_BUILD := $(DUCT_DIR)/build/$(ARCH)$(if $(SAN),-san)$(if $(filter 0,$(RDMA)),-tcp)
DUCT_MAKE_ARGS := $(if $(filter 0,$(RDMA)),RDMA=0)
DUCT_LIB := $(DUCT_BUILD)/librdmaduct.a
ifeq ($(RDMA),1)
LDLIBS += -lrdmacm -libverbs
endif
ifeq ($(TLS),1)
CFLAGS += -DAKV_HAVE_TLS
LDLIBS += -lssl -lcrypto
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
ifdef TSAN
CFLAGS += -fsanitize=thread -fno-omit-frame-pointer
LDFLAGS += -fsanitize=thread
endif

LIB_SRC := src/akv.c src/server.c src/client.c src/proto_mc.c src/proto_resp.c src/snapshot.c src/aof.c src/acl.c src/sha256.c src/pubsub.c
LIB_OBJ := $(patsubst src/%.c,$(OUT)/%.o,$(LIB_SRC))
LIB := $(OUT)/libarenakv.a
TESTS := $(patsubst tests/%.c,$(OUT)/%,$(wildcard tests/test_*.c))

all: $(LIB) $(OUT)/arenakvd $(OUT)/akv_bench $(OUT)/akv_cli

# The transport library is built by its own makefile, which is always consulted (FORCE) so that changes
# to the transport sources are linked in.
$(DUCT_LIB): FORCE
	$(MAKE) -C $(DUCT_DIR) $(DUCT_MAKE_ARGS) $(patsubst $(DUCT_DIR)/%,%,$(DUCT_LIB))

$(OUT)/akv_bench: tools/akv_bench.c $(LIB) $(DUCT_LIB) $(wildcard src/*.h)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(DUCT_LIB) $(LDLIBS)

$(OUT)/akv_cli: tools/akv_cli.c $(LIB) $(DUCT_LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(DUCT_LIB) $(LDLIBS)

$(OUT)/arenakvd: tools/arenakvd.c $(LIB) $(DUCT_LIB)
	$(CC) $(CFLAGS) -DAKV_GIT=\"$(GIT)\" $(LDFLAGS) -o $@ $< $(LIB) $(DUCT_LIB) $(LDLIBS)

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(OUT)/%.o: src/%.c $(wildcard src/*.h include/*.h) $(DUCT_DIR)/include/rdmaduct.h | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test_%: tests/test_%.c $(LIB) $(DUCT_LIB) $(wildcard src/*.h include/*.h)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(DUCT_LIB) $(LDLIBS)

$(OUT):
	mkdir -p $@

# Local address of the first RDMA device's network interface, used by the RDMA server tests (override with ADDR=).
# Empty when RDMA=0: the library is then built without RDMA support and the RDMA cases cannot run.
ifeq ($(RDMA),1)
ADDR ?= $(shell rdma link show 2>/dev/null | sed -n 's/.*netdev \([^ ]*\).*/\1/p' | head -1 | xargs -r -I{} ip -4 -o addr show dev {} | awk '{print $$4}' | cut -d/ -f1 | head -1)
else
ADDR ?=
endif

# ThreadSanitizer cannot map its shadow memory under high ASLR entropy; run those binaries with ASLR off.
RUN := $(if $(TSAN),setarch $(ARCH) -R,)

test: $(TESTS) $(OUT)/arenakvd $(OUT)/akv_cli
	@set -e; for t in $(TESTS); do echo "== $$t"; DUCT_TEST_ADDR="$(ADDR)" $(RUN) $$t; done
	@echo "== tests/cli.sh"; $(RUN) tests/cli.sh $(OUT)
	@echo "== tests/durability.py"; $(RUN) python3 tests/durability.py $(OUT)

install: $(OUT)/arenakvd $(OUT)/akv_bench $(OUT)/akv_cli
	install -D -m 755 $(OUT)/arenakvd $(DESTDIR)$(PREFIX)/bin/arenakvd
	install -D -m 755 $(OUT)/akv_cli $(DESTDIR)$(PREFIX)/bin/akv_cli
	install -D -m 755 $(OUT)/akv_bench $(DESTDIR)$(PREFIX)/bin/akv_bench

format:
	clang-format -i src/*.c src/*.h include/*.h tools/*.c tests/*.c tests/fuzz/*.c

clean:
	rm -rf build

# Third-party clients (valkey-cli, valkey-benchmark, memtier_benchmark, redis-py) against the server; needs Docker.
compat: $(OUT)/arenakvd
	tests/compat.sh $(OUT)

# Coverage-guided fuzzing with libFuzzer (clang): make fuzz [FUZZ_TIME=seconds per harness, default 60].
# -print_funcs=0: libFuzzer would otherwise start the symbolizer for every newly covered function, and the
# sanitizer runtime closes every possible descriptor (up to RLIMIT_NOFILE) before it starts a process.
# Each harness in tests/fuzz runs on a copy of its seed corpus (tests/fuzz/corpus/<name>) under
# AddressSanitizer and UndefinedBehaviorSanitizer; a crash, a leak or a sanitizer report stops the target
# with the harness output.
FUZZ_CC ?= clang
FUZZ_TIME ?= 60
FUZZ_OUT := build/fuzz
FUZZ_FLAGS := -std=gnu2x -g -O1 -Wall -Wextra -Werror -D_GNU_SOURCE -Iinclude -Isrc -I$(DUCT_DIR)/include \
	-I$(DUCT_DIR)/src -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
FUZZ_DUCT := $(DUCT_DIR)/src/hello.c $(DUCT_DIR)/src/duct.c $(DUCT_DIR)/src/tcp.c
FUZZERS := $(patsubst tests/fuzz/%.c,$(FUZZ_OUT)/%,$(wildcard tests/fuzz/fuzz_*.c))

$(FUZZ_OUT)/fuzz_%: tests/fuzz/fuzz_%.c $(LIB_SRC) $(wildcard src/*.h include/*.h)
	@mkdir -p $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_FLAGS) -o $@ $< $(LIB_SRC) $(FUZZ_DUCT) -lpthread

fuzz: $(FUZZERS)
	@set -e; for f in $(FUZZERS); do \
		n=$${f##*/fuzz_}; c=$(FUZZ_OUT)/corpus-$$n; log=$(FUZZ_OUT)/$$n.log; mkdir -p $$c; \
		if [ -d tests/fuzz/corpus/$$n ]; then cp tests/fuzz/corpus/$$n/* $$c/; fi; \
		echo "== fuzz_$$n ($(FUZZ_TIME) s)"; \
		if ! $$f -max_total_time=$(FUZZ_TIME) -timeout=10 -rss_limit_mb=4096 -print_funcs=0 -print_final_stats=1 -artifact_prefix=$(FUZZ_OUT)/$$n- $$c >$$log 2>&1; then \
			tail -n 80 $$log; exit 1; fi; \
		grep -E '^stat::(number_of_executed_units|peak_rss_mb|new_units_added)' $$log | tr '\n' ' '; echo; \
	done

.PHONY: all test install compat format clean fuzz FORCE

FORCE:
