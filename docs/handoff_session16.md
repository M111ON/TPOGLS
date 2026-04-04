# Geomatrix / POGLS — Handoff Session 16
**Date:** 2026-04-03
**Status:** Files written — pending compile + test run

---

## Files Delivered

| File | Status |
|------|--------|
| `geo_fibo_clock.h` | **UPDATED** — GeoSeed + ThirdEye embedded |
| `geo_dodeca_torus.h` | **UPDATED** — torus_run_field signature changed |
| `stress_test_s16.c` | **NEW** — T1-T10 patched + T11 added |
| `geo_thirdeye.h` | frozen (from Geo project, pulled as-is) |
| `geo_config.h` | frozen (from Geo project) |

---

## Session 16 Changes

### 1. FiboCtx — GeoSeed + ThirdEye

```c
typedef struct {
    FiboDualCh   dual;
    FiboClockCtx clk;
    uint64_t     prev_delta;

    GeoSeed  seed;    /* [NEW] gen2=spatial, gen3=temporal — 2 register */
    ThirdEye eye;     /* [NEW] 144-cycle observer, co-fires with FLUSH  */
    uint32_t prev_fx; /* [NEW] for val_drift = |fx_curr - fx_prev|      */
} FiboCtx;
```

Init:
```c
fibo_ctx_init(&fclk);           // zero seed
fibo_ctx_set_seed(&fclk, seed); // inject GeoSeed
```

### 2. API Breaking Changes (call sites updated in stress_test_s16.c)

| Old | New |
|-----|-----|
| `fibo_hop_fast(ctx, block, route_sig)` | `fibo_hop_fast(ctx, block)` |
| `fibo_hop(ctx, block, hop, route_sig)` | `fibo_hop(ctx, block, hop)` |
| `torus_run_field(n, steps, xc, fclk, route_sig)` | `torus_run_field(n, steps, xc, fclk)` |

SIG verify now uses `ctx->seed.gen2` internally — not caller's route_sig.

### 3. torus_run_field — XRay gated by ThirdEye state

```
QRPN_NORMAL  → record all face hits (unchanged behavior)
QRPN_STRESSED→ record only even faces (spatial filter)
QRPN_ANOMALY → record all + set proxy.core.raw bit63 (anomaly flag)
```

Returns `FiboEvent` OR'd over all steps — includes:
- `FIBO_EV_TE_STRESSED` (0x20) at flush boundary if STRESSED
- `FIBO_EV_TE_ANOMALY`  (0x40) at flush boundary if ANOMALY

### 4. New helpers

```c
uint8_t fibo_mirror_mask(const FiboCtx *ctx); // te_get_mask() for current spoke
uint8_t fibo_qrpn_state(const FiboCtx *ctx);  // ctx->eye.qrpn_state
```

### 5. T11 — ThirdEye state transition

Tests 7 correctness conditions (A..G):
- A: init GeoSeed → NORMAL
- B: 144 clean steps → stays NORMAL
- C: force hot_slots=65 → STRESSED (or ANOMALY)
- D: force hot_slots=97 → ANOMALY
- E: torus_run_field returns FIBO_EV_TE_* flags at flush
- F: mask in ANOMALY = 0x3F (all 6 spokes)
- G: popcount(mask_stressed) > popcount(mask_normal)

---

## Compile

```bash
# Requires: geo_config.h, geo_thirdeye.h in include path
gcc -O2 -mavx2 -I. stress_test_s16.c -o stress_test_s16 && ./stress_test_s16
# expected: 11/11 passed
```

---

## Known: Other Files That Need Call-Site Patches

If `geo_diamond_v5x4.h` or `geo_diamond_field5.h` call `fibo_hop_fast` with
3 args internally, they will need the same route_sig removal.
Check with: `grep -n "fibo_hop_fast\|fibo_hop(" geo_diamond_*.h`

---

## Session 17 Open Items

| งาน | Note |
|-----|------|
| `ts_mirror.h` integration | DiamondBlock → 10B MirrorBlock on read path |
| GeoSeed generation strategy | currently zero-init; real entropy from baseline XOR |
| Python `.so` binding | pogls_write/read/rewind + batch ExecWindow |
| streaming GB test | carry-across-batch correctness at scale |
| `geo_route.h` + `theta_map.h` | needed for ts_pipeline full write path |
