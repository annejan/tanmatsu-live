# Host build of the platform independent parts, so the DSP and pattern code can
# be tested without flashing a Tanmatsu.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -g
INC     := -I../../components/strudel_dsp/include -I../../components/strudel_core/include
SRC     := $(wildcard ../../components/strudel_dsp/*.c) $(wildcard ../../components/strudel_core/*.c)
LDLIBS  := -lm

TESTS := test_synth

all: $(TESTS)

test_synth: test_synth.c $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ $^ $(LDLIBS)

bench: bench.c $(SRC)
	$(CC) -Og -g -Wall $(INC) -o bench_og $^ $(LDLIBS)
	$(CC) -O2 -g -Wall $(INC) -o bench_o2 $^ $(LDLIBS)
	@echo "== -Og (what the badge builds today)"; ./bench_og
	@echo "== -O2"; ./bench_o2

check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done
	@echo "all host tests passed"

clean:
	rm -f $(TESTS) bench_og bench_o2 *.wav

.PHONY: all check bench clean
