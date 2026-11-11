# Build:  make            -> build/minidb, build/minidb-server, build/bench
# Test:   make test       -> unit tests + CLI and server functional tests
# Bench:  make bench      -> 1,000,000-row index vs. linear scan comparison
# Checks: make asan   /   make tsan

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -pthread -Isrc
LDFLAGS += -pthread

ifdef SAN
CFLAGS  += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SAN)
BUILD   := build-$(SAN)
else
BUILD   := build
endif

ENGINE := common lexer parser bptree table db format
ENGINE_OBJS := $(ENGINE:%=$(BUILD)/%.o)
TESTS  := lexer parser bptree table db http threadpool concurrency
TEST_BINS := $(TESTS:%=$(BUILD)/test_%)

.PHONY: all test unit functional bench asan tsan clean
all: $(BUILD)/minidb $(BUILD)/minidb-server $(BUILD)/bench

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_%.o: tests/test_%.c $(wildcard src/*.h) tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/minidb: $(BUILD)/main.o $(ENGINE_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD)/minidb-server: $(BUILD)/server.o $(BUILD)/http.o $(BUILD)/threadpool.o $(ENGINE_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD)/bench: $(BUILD)/bench.o $(ENGINE_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD)/test_%: $(BUILD)/test_%.o $(BUILD)/http.o $(BUILD)/threadpool.o $(ENGINE_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

unit: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do ./$$t || fail=1; done; \
	if [ $$fail -eq 0 ]; then echo "ALL UNIT TESTS PASSED"; else echo "UNIT TESTS FAILED"; exit 1; fi

functional: all
	bash tests/functional/test_cli.sh $(BUILD)
	bash tests/functional/test_server.sh $(BUILD)

test: unit functional

bench: $(BUILD)/bench
	./$(BUILD)/bench 1000000

asan:
	$(MAKE) SAN=address test

# gcc's ThreadSanitizer cannot start under high-entropy ASLR on recent kernels,
# so run it with address randomisation turned off.
tsan:
	setarch $$(uname -m) -R $(MAKE) SAN=thread unit

clean:
	rm -rf build build-address build-thread

.SECONDARY:
