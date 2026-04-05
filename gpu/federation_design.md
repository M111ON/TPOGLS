# POGLS Federation — V4 + POGLS38

## เปรียบเทียบง่ายๆ

```
V4     = ท่อประปาเมือง   (storage, 54 lanes, 162 nodes)
POGLS38 = ระบบกรองน้ำ    (routing, 17n lattice, 289 cells)
Federation = วาล์วกลาง   (share snapshot + PHI)

น้ำ (data) วิ่งผ่านกรอง 38 → เข้าท่อ V4 → เก็บลงดิสก์
```

---

## สิ่งที่ Share (Frozen Interface)

```
┌─────────────────────────────────────────────┐
│  pogls_federation.h  — shared layer         │
│                                              │
│  1. PHI constants   ← pogls_platform.h       │
│  2. DiamondBlock    ← 64B format             │
│  3. Snapshot V4     ← 12-step commit         │
│  4. angular_addr    ← A = floor(θ × 2²⁰)    │
└─────────────────────────────────────────────┘
```

## สิ่งที่แยกกัน (Independent)

```
V4 owns:                    POGLS38 owns:
  delta lanes (54)            lattice cells (289)
  pipeline_wire               GPU kernel
  L3 router                   17n routing
  Hydra heads                 Voronoi map
  RewindBuffer                FiftyFourBridge ring
```

---

## Snapshot V4 — Gap Analysis

| Feature | V3.5 (POGLS38 ปัจจุบัน) | V4 (ต้อง port) |
|---------|--------------------------|----------------|
| Lanes | 4 (X/nX/Y/nY) | 8 (A×4 + B×4) |
| Merkle | single root | Dual (root_A + root_B → root_AB) |
| TORN detect | `stat()==0` | `st_size > 0` |
| Commit steps | 8 | 12 |
| World B | ❌ | ✅ lanes 4-7 |

### Gap ที่ต้อง fill — 3 จุด

```
GAP-1: TORN detection
  เดิม:  if (stat(pending_path) == 0) → CLEAN
  ใหม่:  if (stat(pending_path).st_size > 0) → TORN
  เหตุ:  V4 re-opens .pending ทันทีหลัง commit
         → file มีอยู่เสมอ แค่ size=0

GAP-2: Dual Merkle
  เดิม:  root = SHA256(lane0||lane1||lane2||lane3)
  ใหม่:  root_A = SHA256(lane0-3)
         root_B = SHA256(lane4-7)
         root_AB = SHA256(root_A || root_B)  ← ATOMIC FINAL

GAP-3: 12-step commit
  เพิ่ม:  step 7-8 = fsync World B lanes
          step 9-10 = rename B lanes
          ก่อน step 11 (atomic rename merkle)
```

---

## pogls_federation.h — โครงสร้าง

```c
/*
 * pogls_federation.h — V4 + POGLS38 shared layer
 * ─────────────────────────────────────────────
 * Rules:
 *   - PHI constants from pogls_platform.h ONLY
 *   - DiamondBlock 64B format FROZEN
 *   - Snapshot = V4 12-step (World A+B)
 *   - TORN = st_size > 0 (not stat()==0)
 *   - No GPU in commit path
 */

#ifndef POGLS_FEDERATION_H
#define POGLS_FEDERATION_H

#include "pogls_platform.h"   /* PHI constants */
#include "storage/pogls_delta.h"        /* World A */
#include "storage/pogls_delta_world_b.h" /* World B */

/* Federation context — one per shared vault */
typedef struct {
    Delta_ContextAB  snap;       /* V4 dual-world snapshot */
    uint64_t         fed_epoch;  /* shared monotonic counter */
    uint32_t         magic;      /* FED_MAGIC */
} FederationCtx;

#define FED_MAGIC  0x46454400u   /* "FED\0" */

/* Init: open both V4 worlds */
static inline int fed_init(FederationCtx *f, const char *vault) {
    if (!f || !vault) return -1;
    memset(f, 0, sizeof(*f));
    f->magic = FED_MAGIC;
    return delta_ab_open(&f->snap, vault);
}

/* Commit: V4 12-step (both worlds) */
static inline int fed_commit(FederationCtx *f) {
    if (!f) return -1;
    int r = delta_ab_commit(&f->snap);
    if (r == 0) f->fed_epoch++;
    return r;
}

/* Close */
static inline void fed_close(FederationCtx *f) {
    if (!f) return;
    delta_ab_close(&f->snap);
}

/* Recovery — V4 TORN rule: st_size > 0 */
static inline Delta_DualRecovery fed_recover(const char *vault) {
    return delta_ab_recover(vault);
}

#endif /* POGLS_FEDERATION_H */
```

---

## Integration Point

```
POGLS38 GPU kernel → output: (hilbert, lane, iso) per cell
                               ↓
                     angular_addr = PHI scatter(cell_id)
                               ↓
              fed_write(&f, value, angular_addr)
                    = pogls_write(&pw, value, angular_addr)
                               ↓
                     V4 pipeline (L3 → delta lanes)
```

---

## งานที่ต้องทำ (ordered)

```
Step 1: สร้าง pogls_federation.h  (wrapper บาง)
Step 2: Fix TORN detect ใน POGLS38 snapshot
Step 3: Add World B support ใน POGLS38 (lanes 4-7)
Step 4: Dual Merkle ใน POGLS38
Step 5: Test federation_test.c — round-trip V4↔38
Step 6: GPU kernel → fed_write() integration
```

### Step 1-2 เริ่มได้วันนี้บน Colab (ไม่ต้องการไฟล์ใหม่)
### Step 3-4 ต้องการ POGLS38 source
### Step 5-6 test + GPU wiring
