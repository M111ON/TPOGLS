# POGLS S27 — Handoff
**Status:** ✅ COMPLETE

## What was done

### `rest_server_s27.py` — GeoNetCtx wired into wallet stream

สองเปลี่ยนหลักจาก S23 base:

**`CtxRecord.geo_ctx`** — `GeoNetCtx` instance อยู่ใน `CtxRecord` ต่อ `ctx_id` หนึ่งตัว ไม่สร้างใหม่ทุก request ทำให้ state สะสมตามลำดับ layer จริง (Memory persistent per session)

**`POST /wallet/load`** — เดิมเรียก `route_layer()` แบบ stateless ต่อ layer แยกกัน ตอนนี้ใช้ `_route_layer_stateful(rec.geo_ctx, i, arr.nbytes)` ที่ feed chunk ทุกตัวผ่าน shared ctx ทีละ layer ตามลำดับ ผลลัพธ์แต่ละ layer ได้ `qrpn_state_after` (state ณ จุดนั้นของ stream) top-level response มี `geo_ctx_status` รวมสถานะ ctx หลัง load ครบ

**`POST /wallet/layer`** — เพิ่ม field `geo_state`, `routing` ทุก call ถ้า pass `annotate=true` จะได้ `chunk_map` array ด้วย ทุก entry มี `{chunk_i, addr, spoke, inv_spoke, mirror_mask, is_audit}` — mirror_mask คือ ThirdEye spoke mask ณ เวลานั้นของ ctx

**`_route_layer_annotated()`** — helper ใหม่ที่ทำทั้ง summary + chunk_map ในรอบเดียว `_route_layer_stateful()` ไม่สร้าง chunk_map (ประหยัด alloc สำหรับ /wallet/load ที่ไม่ต้องการ per-chunk detail)

### `test_rest_s27.py` — 25 tests, 25 passed

ครอบคลุม: ctx lifetime, stateful routing per layer, annotated chunk map, QRPN state transitions (NORMAL→STRESSED→ANOMALY), session independence, layer stream accumulation

## API diff vs S23

```
WalletLayerBody += annotate: bool = False

/wallet/load response:
  layers[name].routing += qrpn_state_after
  + top-level geo_ctx_status

/wallet/layer response:
  + geo_state
  + routing.qrpn_state_after
  + chunk_map (only if annotate=true)
```

## S28 candidates

1. **`.so` build (WSL)** — geo_net_py logic → C extension, benchmark vs pure Python per-chunk routing
2. **`annotate` streaming** — chunk_map ใหญ่มากถ้า layer มีหลายพัน chunks → อาจ stream NDJSON แทน JSON array
3. **Twin Geometry Priority 4** — AVX2+CUDA via `geo_fast_intersect_x4` ใน `geo_fused_write_batch` (`CYL_FULL_N=3072`)
