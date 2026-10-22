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
ifdef SAN
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif
ifdef TSAN
CFLAGS += -fsanitize=thread -fno-omit-frame-pointer
LDFLAGS += -fsanitize=thread
endif

LIB_SRC := src/akv.c src/server.c src/client.c src/proto_mc.c src/proto_resp.c src/snapshot.c src/aof.c
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
	clang-format -i src/*.c src/*.h include/*.h tools/*.c tests/*.c

clean:
	rm -rf build

# Third-party clients (valkey-cli, valkey-benchmark, memtier_benchmark, redis-py) against the server; needs Docker.
compat: $(OUT)/arenakvd
	tests/compat.sh $(OUT)

.PHONY: all test install compat format clean FORCE

FORCE:
