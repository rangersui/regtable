# regtable desktop build.
#
#   make          build example and test
#   make run      build and start the interactive example
#   make test     build and run the regression test
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
TEST    = test$(EXE)

.PHONY: all run test clean

all: $(EXAMPLE) $(TEST)

$(EXAMPLE): example_desktop.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ example_desktop.c $(LIB_SRC)

$(TEST): regtable_test.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ regtable_test.c $(LIB_SRC)

run: $(EXAMPLE)
	./$(EXAMPLE)

test: $(TEST)
	./$(TEST)

clean:
	-$(RM) $(EXAMPLE) $(TEST)
