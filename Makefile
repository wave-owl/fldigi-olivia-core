# fldigi-olivia-core -- plain make, C99, no dependencies besides libm and the pinned
# fldigi-dsp-core submodule dsp/.
#   make            static + shared library into $(BUILD) (after the dsp/ pin check)
#   make test       build and run the C tests (receiver, transmitter, fldigi goldens)
#   make sanitize   the C tests under AddressSanitizer + UndefinedBehaviorSanitizer
#   make bench      CPU benchmark (per submode, receiver and transmitter)
#   make check-dsp  only the dsp/ pin check (dsp.lock, R-920.3)
#   make BUILD=/abs/dir shared   out-of-tree build (used by wave-owl)
BUILD ?= build
CC ?= cc
# -ffp-contract=on: fuse a*b+c within one expression, as clang and gcc do by default and as
# the fldigi 4.2.13 reference build did; with it the receiver reproduces that build bit for bit.
CFLAGS ?= -O3
DSP ?= dsp
CORE_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -ffp-contract=on -fPIC -Iinclude \
             -I$(DSP)/include
LDLIBS = -lm
# Tests rebuild the golden noise exactly as Python does: no contraction there.
TEST_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -ffp-contract=off -Iinclude

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  SHLIB = libfldigi_olivia.dylib
  SHFLAGS = -dynamiclib -install_name @rpath/libfldigi_olivia.dylib
else
  SHLIB = libfldigi_olivia.so
  SHFLAGS = -shared -Wl,-soname,libfldigi_olivia.so
endif

HDRS = include/fldigi_olivia.h src/olivia_internal.h $(DSP)/include/fldigi_dsp.h
OBJ = $(BUILD)/olivia_common.o $(BUILD)/olivia_rx.o $(BUILD)/olivia_tx.o \
      $(BUILD)/dsp_fdsp_jalocha.o

all: static shared
static: $(BUILD)/libfldigi_olivia.a
shared: $(BUILD)/$(SHLIB)

$(BUILD):
	mkdir -p $(BUILD)

# The pin gate runs before anything is compiled and again whenever dsp/ or dsp.lock change.
$(BUILD)/dsp.ok: dsp.lock tools/check_dsp.sh $(wildcard $(DSP)/include/*.h $(DSP)/src/*.c) | $(BUILD)
	sh tools/check_dsp.sh dsp.lock $(DSP)
	touch $@

check-dsp:
	sh tools/check_dsp.sh dsp.lock $(DSP)

$(BUILD)/%.o: src/%.c $(HDRS) $(BUILD)/dsp.ok
	$(CC) $(CORE_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/dsp_%.o: $(DSP)/src/%.c $(HDRS) $(BUILD)/dsp.ok
	$(CC) $(CORE_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libfldigi_olivia.a: $(OBJ)
	ar rcs $@ $^

$(BUILD)/$(SHLIB): $(OBJ)
	$(CC) $(SHFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_olivia: tests/test_olivia.c tests/test_util.h $(BUILD)/libfldigi_olivia.a
	$(CC) $(TEST_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_olivia.a -o $@ $(LDLIBS)

$(BUILD)/test_olivia_fldigi: tests/test_olivia_fldigi.c tests/test_util.h $(BUILD)/libfldigi_olivia.a
	$(CC) $(TEST_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_olivia.a -o $@ $(LDLIBS)

test: $(BUILD)/test_olivia $(BUILD)/test_olivia_fldigi
	$(BUILD)/test_olivia
	$(BUILD)/test_olivia_fldigi tests/golden

SAN_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
            -DOLIVIA_NOT_REFERENCE

sanitize:
	$(MAKE) --no-print-directory BUILD=$(BUILD)/san CFLAGS="$(SAN_FLAGS)" \
		LDLIBS="$(LDLIBS) $(SAN_FLAGS)" test

$(BUILD)/bench_olivia: tests/bench_olivia.c tests/test_util.h $(BUILD)/libfldigi_olivia.a
	$(CC) $(TEST_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_olivia.a -o $@ $(LDLIBS)

bench: $(BUILD)/bench_olivia
	$(BUILD)/bench_olivia

clean:
	rm -rf $(BUILD)

.PHONY: all static shared check-dsp test sanitize bench clean
