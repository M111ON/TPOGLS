# TPOGLS S14 Handoff — Session End

## ผลรวม session นี้: 32/32 PASS | ASan+UBSan clean | zero warnings

---

## สิ่งที่ทำใน S14

### 1. geo_union_mask.h (✅ new)
- integer fold: `(writes × qrpn_fail × 144) / (io_total × qrpn_total)`
- zone borders: 72/36/18 — ทุกตัว divisor of 144 ✓
- polarity: DANGER 3:-3 / NORMAL 4:-4 / SAFE compress
- compile-time divisor assert

### 2. geo_payload_wired.h (✅ new)
- PayloadStore + UnionMask wired เป็น single struct
- compress gate: bypass window แรก (cold-start safe)
- pw_write / pw_read / pw_qrpn / pw_eval

### 3. geo_cache.h (✅ new)
- 2-way set associative (144 slots × 2 ways × 6 lanes = ~28KB, fits L1)
- victim buffer 8 entries/lane, evict hit=0 first
- A/B window swap every 144 writes (temporal locality)
- self-heal: collision ≥ 72 → reseed salt[lane] via PHI64
- **slot fix**: `>>32 % 144` (spread=8) vs original `>>56 scale` (spread=67)
- **lane fix**: `(hi32 ^ hi48) % 6` กัน skew

### 4. geo_pipeline.h (✅ new — full wire)
- GeoCache (L1) → PayloadWired (L2) → KV signal (caller)
- cross-layer: UnionMask fold[DANGER] → geo_reseed(salt)
- window sync: GEO_SLOTS == PW_WINDOW == 144 (shared boundary)
- L2 hit → auto-promote back to L1

---

## Test Suites

| Suite | Result | File |
|---|---|---|
| Union Mask | 8/8 | test_union_mask.c |
| PayloadWired | 8/8 | test_payload_wired.c |
| GeoCache | 8/8 | test_geo_cache.c |
| GeoPipeline | 8/8 | test_geo_pipeline.c |

---

## Architecture State

```
WRITE(key, val):
  pw_write → compress gate (SAFE: 1/4 or 3/4 keep)
  → PayloadStore.cells[lane][slot] = val
  → UnionMask.counter[lane]++
  → geo_write → GeoCache.A.ways[slot][w]
  → gp_tick → window swap @ seq%144==0
              → pw_eval → gp_sync_reseed

READ(key):
  GeoCache A → B           O(1)  L1
  → PayloadWired            O(1)  L2 + promote to L1
  → return 0 (KV signal)   O(1)  caller fallback

SELF-HEAL:
  collision ≥ 72 → reseed salt[lane] (cache internal)
  fold[DANGER]   → reseed salt[lane] (cross-layer)

SYSTEM MISS = 0:
  P(cache) × P(store) × P(KV=0) = 0
```

---

## Number Anchors (S14)

```
144 = 12²      highly composite (15 divisors)
 72 = 144/2    DANGER border / GEO_COLLIDE_THRESH
 36 = 144/4    NORMAL border
 18 = 144/8    compress threshold
  8 = victim buffer (144%8=0 ✓)
  6 = lanes = GEO_SPOKES = PL_PAIRS
```

---

## Files (S14 new)

| File | Role |
|---|---|
| `geo_union_mask.h` | fold sensor per lane |
| `geo_payload_wired.h` | PayloadStore + UnionMask |
| `geo_cache.h` | L1 cache: 2-way + victim + A/B + reseed |
| `geo_pipeline.h` | full wire: L1→L2→KV signal |

---

## Open Items (S15+)

| Priority | Item | Notes |
|---|---|---|
| 🟡 | KV_store implementation | external truth layer (hash table / robin hood) |
| ⚪ | GPU batch ≥64K | trigger เมื่อ drop rate >50% |
| ⚪ | SDK boundary | pogls_write/read/rewind C API → .so |

---

## Build

```bash
gcc -O2 -fsanitize=address,undefined -I. -o test_um    test_union_mask.c    -lm
gcc -O2 -fsanitize=address,undefined -I. -o test_pw    test_payload_wired.c -lm
gcc -O2 -fsanitize=address,undefined -I. -o test_gc    test_geo_cache.c     -lm
gcc -O2 -fsanitize=address,undefined -I. -o test_gp    test_geo_pipeline.c  -lm
```
