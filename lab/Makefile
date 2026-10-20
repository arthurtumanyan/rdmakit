# Builds every stepN_*.c into build/<arch>/ so that builds for different architectures can share one source tree.
#   make          build all steps
#   make clean
ARCH  := $(shell uname -m)
OUT   := build/$(ARCH)
SRCS  := $(wildcard step*.c)
BINS  := $(patsubst %.c,$(OUT)/%,$(SRCS))
CFLAGS += -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDLIBS += -libverbs -lrdmacm -lpthread

all: $(BINS)
$(OUT)/%: %.c common.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
$(OUT):
	mkdir -p $@
clean:
	rm -rf build
.PHONY: all clean
