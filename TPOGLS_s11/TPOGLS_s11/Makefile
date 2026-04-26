# TPOGLS Makefile
# Build: make tests | make bench | make so | make cli

CC      = gcc
CFLAGS  = -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -I. -I./core
LDFLAGS = -lm

SO_SRC  = pogls_sdk_so.c
SO_OUT  = libpogls_v4.so

# ── .so ─────────────────────────────────────────────────────────────
so:
	$(CC) -O2 -shared -fPIC -I. -I./core -o $(SO_OUT) $(SO_SRC) -lm
	@echo "built: $(SO_OUT)"

# ── tests (17/17 from S10 handoff) ──────────────────────────────────
test_api: tests/test_api.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_edge: tests/test_edge.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_fault: tests/test_fault.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_e2e: tests/test_e2e_pipeline.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests: test_api test_edge test_fault test_e2e
	@echo "--- running tests ---"
	./test_api && ./test_edge && ./test_fault && ./test_e2e
	@echo "all tests done"

# ── bench ────────────────────────────────────────────────────────────
bench_api: bench/bench_api.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_ws: bench/bench_weight_stream.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_s4: bench/bench_s4.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench: bench_api bench_ws bench_s4
	./bench_api && ./bench_ws && ./bench_s4

# ── CLI ──────────────────────────────────────────────────────────────
cli: cli/pogls_cli.c
	$(CC) $(CFLAGS) -o pogls $< $(LDFLAGS)
	@echo "built: pogls  (encode/decode/verify/read)"

cli_s3: cli/pogls_cli_s3.c
	$(CC) $(CFLAGS) -o pogls_s3 $< $(LDFLAGS)
	@echo "built: pogls_s3  (+ cat/info)"

# ── REST server ──────────────────────────────────────────────────────
serve:
	cd python && uvicorn rest_server:app --port 8765

.PHONY: so tests bench cli cli_s3 serve
