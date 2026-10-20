# Top-level makefile: builds and tests every project and runs the benchmark verification.
#
#   make              build rdmaduct, arenakv, kvpace and the lab programs
#   make test         run every test suite (RDMA suites run when DUCT_TEST_ADDR is set or detected)
#   make test-tcp     run the suites without RDMA (no libibverbs or librdmacm needed)
#   make sanitize     run the suites under AddressSanitizer and UndefinedBehaviorSanitizer
#   make format       apply clang-format to all C sources
#   make format-check fail when a C source is not formatted
#   make verify       check benchmark figures, tables and claims against the raw data of the latest run
#   make clean        remove build output
PROJECTS := rdmaduct arenakv kvpace
LATEST := $(shell cat benchmarks/LATEST 2>/dev/null)

all:
	$(MAKE) -C rdmaduct
	$(MAKE) -C arenakv
	$(MAKE) -C kvpace
	-$(MAKE) -C lab

test:
	for p in $(PROJECTS); do $(MAKE) -C $$p test || exit 1; done

test-tcp:
	for p in $(PROJECTS); do $(MAKE) -C $$p RDMA=0 test || exit 1; done

sanitize:
	for p in $(PROJECTS); do $(MAKE) -C $$p SAN=1 test || exit 1; done

format:
	for p in $(PROJECTS) lab; do clang-format -i $$(git ls-files "$$p/*.c" "$$p/*.h"); done

format-check:
	for p in $(PROJECTS) lab; do clang-format --dry-run --Werror $$(git ls-files "$$p/*.c" "$$p/*.h") || exit 1; done

verify:
	python3 benchmarks/tools/verify.py benchmarks/data/$(LATEST)

clean:
	for p in $(PROJECTS) lab; do $(MAKE) -C $$p clean; done

.PHONY: all test test-tcp sanitize format format-check verify clean
