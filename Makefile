# regtable desktop build.
#
#   make          build example and test
#   make run      build and start the interactive example
#   make test     build and run the regression test
#   make run-tcp  build and start the desktop Modbus TCP slave example
#   make run-mqtt build and start the desktop MQTT demo (stdout broker)
#   make codegen  generate tables from tools/example.yaml and the SVD fixture, test them
#                 (python with PyYAML; runs the package from python/, no install)
#   make strict   rebuild everything with -Werror and run the test
#   make fuzz     libFuzzer + ASan/UBSan on the CLI byte path (needs clang)
#   make fuzz-mb  same, on the Modbus RTU frame path
#   make clean
#
# Works with GNU make on Linux, macOS, Git Bash, and Windows cmd
# (needs gcc or clang on PATH). No MCU toolchain involved; the
# same library sources compile unchanged on the target.

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2
# override: -Isrc stays even when CFLAGS=... is given on the command line
override CFLAGS += -Isrc

LIB_SRC  = src/regtable_core.c src/regtable_cli.c src/regtable_modbus.c src/regtable_mqtt.c
LIB_HDR  = src/regtable_core.h src/regtable_cli.h src/regtable_modbus.h src/regtable_mqtt.h

# Windows only differs in the .exe suffix. GNU make on Windows runs
# recipes through sh when one is on PATH (Git for Windows provides
# it), so rm works in cmd too. Without sh, use build.bat instead.
# -lm only off Windows: there the math functions are in the CRT and
# MSVC-target clang has no m.lib.
ifeq ($(OS),Windows_NT)
  EXE    := .exe
  LIBM   :=
  LIBNET := -lws2_32
  PY     ?= python
else
  EXE    :=
  LIBM   := -lm
  LIBNET :=
  PY     ?= python3
endif
RM := rm -f

EXAMPLE = example$(EXE)
TCPSLAVE = tcpslave$(EXE)
MQTTDEMO = mqttdemo$(EXE)
TEST    = regtest$(EXE)
MBTEST  = mbtest$(EXE)
MQTEST  = mqtest$(EXE)

.PHONY: all run run-tcp run-mqtt test strict codegen fuzz fuzz-mb clean

all: $(EXAMPLE) $(TEST) $(MBTEST) $(MQTEST) $(TCPSLAVE) $(MQTTDEMO)

# warnings become errors; forces a full rebuild of everything (examples
# included) so nothing stale or unbuilt slips by
strict:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -Werror"
	$(MAKE) test CFLAGS="$(CFLAGS) -Werror"

$(EXAMPLE): example_desktop.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ example_desktop.c $(LIB_SRC) $(LIBM)

$(TEST): regtable_test.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ regtable_test.c $(LIB_SRC) $(LIBM)

$(MBTEST): regtable_modbus_test.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ regtable_modbus_test.c $(LIB_SRC) $(LIBM)

$(MQTEST): regtable_mqtt_test.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ regtable_mqtt_test.c $(LIB_SRC) $(LIBM)

run: $(EXAMPLE)
	./$(EXAMPLE)

run-tcp: $(TCPSLAVE)
	./$(TCPSLAVE)

$(TCPSLAVE): example_modbus_tcp.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ example_modbus_tcp.c $(LIB_SRC) $(LIBM) $(LIBNET)

run-mqtt: $(MQTTDEMO)
	./$(MQTTDEMO)

$(MQTTDEMO): example_mqtt_desktop.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ example_mqtt_desktop.c $(LIB_SRC) $(LIBM)

test: $(TEST) $(MBTEST) $(MQTEST)
	./$(TEST)
	./$(MBTEST)
	./$(MQTEST)

# The validator rejection suite, then YAML -> registers.c/.h/.md and a
# typed Python client, then compile and run the smoke test against the
# generated table, then the Python client against a CLI built over it.
# Needs python with PyYAML.
RT = $(PY) python/regtable
codegen: $(EXAMPLE)
	$(PY) python/tests/gen_test.py
	$(RT) gen tools/example.yaml -o gen
	$(RT) gen python/tests/svd_demo.yaml -o gen_svd
	$(CC) $(CFLAGS) -Werror -Igen_svd -c gen_svd/registers.c -o gen_svd/registers.o
	$(CC) $(CFLAGS) -Werror -Igen -o gen/smoke$(EXE) \
	    tools/gen_smoke.c gen/registers.c $(LIB_SRC) $(LIBM)
	./gen/smoke$(EXE)
	g++ -x c++ -std=c++11 -Wall -Wextra -Werror -Isrc -Igen \
	    -c tools/gen_smoke.c -o gen/smoke_cxx.o
	$(CC) $(CFLAGS) -Werror -Igen -o gen/cli$(EXE) \
	    tools/gen_cli.c gen/registers.c $(LIB_SRC) $(LIBM)
	$(PY) python/tests/client_test.py gen/cli$(EXE) ./$(EXAMPLE) gen
	$(RT) watch --yaml tools/example.yaml --every 0 --count 1 --json --pipe ./gen/cli$(EXE)
	$(RT) watch temp led --every 0 --count 1 --pipe ./gen/cli$(EXE)
	$(RT) fetch --pipe ./gen/cli$(EXE)

# Coverage-guided search for inputs the regression test did not think
# of. Seeds a corpus with a few valid lines, then runs for FUZZ_TIME
# seconds. Crashes land in the working directory as crash-* files.
FUZZ      = fuzz$(EXE)
FUZZ_TIME ?= 60

fuzz: regtable_fuzz.c $(LIB_SRC) $(LIB_HDR)
	clang -g -O1 -Isrc -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -o $(FUZZ) regtable_fuzz.c $(LIB_SRC) $(LIBM)
	mkdir -p corpus
	printf 'help\n'                      > corpus/help
	printf 'list --json\n'               > corpus/list
	printf 'get gain\n'                  > corpus/get
	printf 'set gain 1.5\n'              > corpus/set1
	printf 'set interval 0x1F4 --json\n' > corpus/set2
	printf 'info hwreg --json\n'         > corpus/info
	printf 'set offset -0x10\nset led TRUE\n' > corpus/multi
	./$(FUZZ) corpus -max_len=256 -max_total_time=$(FUZZ_TIME)

FUZZMB = fuzz_mb$(EXE)

fuzz-mb: regtable_modbus_fuzz.c $(LIB_SRC) $(LIB_HDR)
	clang -g -O1 -Isrc -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -o $(FUZZMB) regtable_modbus_fuzz.c $(LIB_SRC) $(LIBM)
	mkdir -p corpus_mb
	./$(FUZZMB) corpus_mb -max_len=260 -max_total_time=$(FUZZ_TIME)

clean:
	-$(RM) $(EXAMPLE) $(TEST) $(MBTEST) $(MQTEST) $(TCPSLAVE) $(MQTTDEMO) $(FUZZ) $(FUZZMB) fuzz.lib fuzz.exp fuzz.pdb fuzz_mb.lib fuzz_mb.exp fuzz_mb.pdb
	-$(RM) -r corpus corpus_mb gen gen_svd crash-* leak-* timeout-*
