CFLAGS += -L ../../ -I ../../include

UNAME_S := $(shell uname -s)
LDFLAGS += -pthread
ifeq ($(UNAME_S), Linux)
LDFLAGS += -lrt
endif

LDFLAGS += ../../libunicorn.a


ALL_TESTS_SOURCES = $(wildcard fuzz*.c)
ALL_TESTS = $(ALL_TESTS_SOURCES:%.c=%)
RAW_FUZZ_TESTS = $(filter fuzz_emu_%,$(ALL_TESTS))

.PHONY: all
all: ${ALL_TESTS}

.PHONY: replay
replay: all
	@set -e; for test in $(RAW_FUZZ_TESTS); do \
		./$$test corpus/raw; \
	done
	@if test -x ./fuzz_uc_api; then ./fuzz_uc_api corpus/fuzz_uc_api; fi

.PHONY: external-corpus
external-corpus: all
	./dlcorpus.sh .

.PHONY: clean
clean:
	rm -rf ${ALL_TESTS}

fuzz%: fuzz%.c
	$(CC) $(CFLAGS) $^ onedir.c $(LDFLAGS) -o $@
