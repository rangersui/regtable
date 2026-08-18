# regtable desktop build.
#
#   make          build example and test
#   make run      build and start the interactive example
#   make test     build and run the regression test
#   make strict   rebuild everything with -Werror and run the test
#   make fuzz     libFuzzer + ASan/UBSan on the CLI byte path (needs clang)
#   make clean
#
# Works with GNU make on Linux, macOS, Git Bash, and Windows cmd
# (needs gcc or clang on PATH). No MCU toolchain involved; the
# same library sources compile unchanged on the target.

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2

LIB_SRC  = regtable_core.c regtable_cli.c
LIB_HDR  = regtable_core.h regtable_cli.h

# Windows only differs in the .exe suffix. GNU make on Windows runs
# recipes through sh when one is on PATH (Git for Windows provides
# it), so rm works in cmd too. Without sh, use build.bat instead.
ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif
RM := rm -f

EXAMPLE = example$(EXE)
TEST    = regtest$(EXE)

.PHONY: all run test strict fuzz clean

all: $(EXAMPLE) $(TEST)

# warnings become errors; forces a full rebuild so nothing stale slips by
strict:
	$(MAKE) clean
	$(MAKE) test CFLAGS="$(CFLAGS) -Werror"

$(EXAMPLE): example_desktop.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ example_desktop.c $(LIB_SRC) -lm

$(TEST): regtable_test.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ regtable_test.c $(LIB_SRC) -lm

run: $(EXAMPLE)
	./$(EXAMPLE)

test: $(TEST)
	./$(TEST)

# Coverage-guided search for inputs the regression test did not think
# of. Seeds a corpus with a few valid lines, then runs for FUZZ_TIME
# seconds. Crashes land in the working directory as crash-* files.
FUZZ      = fuzz$(EXE)
FUZZ_TIME ?= 60

fuzz: regtable_fuzz.c $(LIB_SRC) $(LIB_HDR)
	clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -o $(FUZZ) regtable_fuzz.c $(LIB_SRC)
	mkdir -p corpus
	printf 'help\n'                      > corpus/help
	printf 'list --json\n'               > corpus/list
	printf 'get gain\n'                  > corpus/get
	printf 'set gain 1.5\n'              > corpus/set1
	printf 'set interval 0x1F4 --json\n' > corpus/set2
	printf 'info hwreg --json\n'         > corpus/info
	printf 'set offset -0x10\nset led TRUE\n' > corpus/multi
	./$(FUZZ) corpus -max_len=256 -max_total_time=$(FUZZ_TIME)

clean:
	-$(RM) $(EXAMPLE) $(TEST) $(FUZZ) fuzz.lib fuzz.exp fuzz.pdb
	-$(RM) -r corpus crash-* leak-* timeout-*
