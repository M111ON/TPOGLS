/*
 * geo_config.h — Geomatrix Single Source of Truth
 * ═══════════════════════════════════════════════
 * ALL geometry constants live here.
 * Every other file #include this — never redefine.
 *
 * Number chain:
 *   6 × 9 × 64 = 3456 = 144 × 24
 *   576 = 24² = 9 × 64
 *   288 = 576/2 = 2 × 144
 *   digit_sum: 3456→9  576→9  288→9  144→9  54→9
 */

#ifndef GEO_CONFIG_H
#define GEO_CONFIG_H

/* ── Cylinder ───────────────────────────────── */
#define GEO_SPOKES          6u    /* 6 LADOS, 60° each          */
#define GEO_FACES           9u    /* 8 outer + 1 center         */
#define GEO_FACE_UNITS      64u   /* units per face = 8²        */
#define GEO_SLOTS           576u  /* 9×64 = 24² per spoke       */
#define GEO_FULL_N          3456u /* 576×6 = 144×24             */
#define GEO_OUTER_SLOTS     512u  /* legacy 8-face (compat)     */
#define GEO_CENTER_BASE     512u  /* center face start          */
#define GEO_SIDE_FULL       54u   /* 6×9 sacred ✓               */

/* ── Block / Hilbert ────────────────────────── */
#define GEO_HILBERT_N       576u  /* = GEO_SLOTS                */
#define GEO_BLOCK_BOUNDARY  288u  /* 576/2 = 2×144 ✓            */
#define GEO_GROUP_SIZE      8u    /* lines per audit group      */
#define GEO_BUNDLE_WORDS    8u    /* words per bundle           */

/* ── ThirdEye ───────────────────────────────── */
#define GEO_TE_CYCLE        144u  /* ops per snapshot           */
#define GEO_TE_FULL_CYCLES  24u   /* 144×24 = 3456              */
#define GEO_TE_SNAPS        6u    /* ring depth = GEO_SPOKES    */

/* ── QRPN thresholds ────────────────────────── */
#define GEO_HOT_THRESH      64u   /* 8² = 1 face                */
#define GEO_IMBAL_THRESH   144u   /* TE_CYCLE = single-spoke seed OK */
#define GEO_ANOMALY_HOT     96u   /* 576/6 = 1/6 spoke          */

/* ── Verify (compile-time) ──────────────────── */
#if (GEO_SPOKES * GEO_SLOTS != GEO_FULL_N)
#  error "GEO: SPOKES * SLOTS != FULL_N"
#endif
#if (GEO_TE_CYCLE * GEO_TE_FULL_CYCLES != GEO_FULL_N)
#  error "GEO: TE_CYCLE * TE_FULL_CYCLES != FULL_N"
#endif
#if (GEO_FACES * GEO_FACE_UNITS != GEO_SLOTS)
#  error "GEO: FACES * FACE_UNITS != SLOTS"
#endif
#if (GEO_BLOCK_BOUNDARY * 2 != GEO_SLOTS)
#  error "GEO: BLOCK_BOUNDARY*2 != SLOTS"
#endif

#endif /* GEO_CONFIG_H */
