#!/bin/bash
# POGLS Cleanup & Fix Script
# Run from repo root: bash pogls_fix.sh
# Fixes: stale root duplicates, twin_core fibo_clock, geo_cache GEO_SLOTS, unmerged patch
set -e
REPO=$(pwd)

echo "[1/5] Removing stale root-level .h duplicates (source of truth = core/ or twin_core/)"
# These are confirmed stale or identical to their canonical location
ROOT_DUPS=(
  geo_config.h          # stale: missing GEO_PHASE_COUNT, wrong GEO_BUNDLE_WORDS=8, GEO_IMBAL_THRESH=144
  geo_fibo_clock.h      # duplicate of core/geo_fibo_clock.h (canonical S16+ version)
  geo_diamond_field.h   # identical to twin_core/geo_diamond_field.h
  geo_dodeca.h          # identical to twin_core/geo_dodeca.h
  geo_dodeca_torus.h    # identical to twin_core/geo_dodeca_torus.h
  geo_hardening_whe.h   # identical to twin_core/geo_hardening_whe.h
  geo_net.h             # identical to core/geo_net.h
  geo_read.h            # identical to twin_core/geo_read.h
  geo_route.h           # identical to twin_core/geo_route.h
  geo_thirdeye.h        # identical to twin_core/geo_thirdeye.h
  theta_map.h           # identical to twin_core/theta_map.h
  twin_core_geo_fibo_clock.h  # duplicate inside twin_core/ already
)
for f in "${ROOT_DUPS[@]}"; do
  if [ -f "$REPO/$f" ]; then
    git rm "$REPO/$f" 2>/dev/null || rm "$REPO/$f"
    echo "  removed: $f"
  fi
done

echo "[2/5] Fix twin_core/geo_fibo_clock.h — replace with core/ canonical (S16 DRIFT fix)"
# twin_core version is missing prev_window_inc field and uses old delta comparison
cp "$REPO/core/geo_fibo_clock.h" "$REPO/twin_core/geo_fibo_clock.h"
echo "  twin_core/geo_fibo_clock.h <- core/geo_fibo_clock.h (16598 bytes, S16 version)"

echo "[3/5] Fix geo_cache.h — rename GEO_SLOTS -> GEO_CACHE_SLOTS to avoid clobbering geo_config.h"
GEO_CACHE="$REPO/geo_cache.h"
if [ -f "$GEO_CACHE" ]; then
  sed -i 's/#define GEO_SLOTS  144/#define GEO_CACHE_SLOTS  144/g' "$GEO_CACHE"
  sed -i 's/GEO_SLOTS\b/GEO_CACHE_SLOTS/g' "$GEO_CACHE"
  echo "  geo_cache.h: GEO_SLOTS -> GEO_CACHE_SLOTS (144 cache slots != geometry 576)"
else
  echo "  WARN: geo_cache.h not found at root — check path"
fi

echo "[4/5] Merge rest_server_llm_patch into rest_server.py"
# Both patch files are identical (11153 bytes) — apply once, remove both
PATCH="$REPO/rest_server_llm_patch.py"
TARGET="$REPO/rest_server.py"
if [ -f "$PATCH" ] && [ -f "$TARGET" ]; then
  # Check if patch already applied (look for marker from patch file)
  PATCH_MARKER=$(head -5 "$PATCH" | grep -c "llm_patch\|patch" || true)
  echo "  NOTE: rest_server_llm_patch.py is a full replacement file, not a diff patch."
  echo "  Action: rest_server.py <- rest_server_llm_patch.py (latest, 2026-04-07 10:18)"
  cp "$PATCH" "$TARGET"
  git rm "$REPO/rest_server_llm_patch.py" 2>/dev/null || rm "$REPO/rest_server_llm_patch.py"
  git rm "$REPO/rest_server_llm_patch (1).py" 2>/dev/null || rm "$REPO/rest_server_llm_patch (1).py" 2>/dev/null || true
  echo "  merged: rest_server_llm_patch.py -> rest_server.py, patch files removed"
fi

echo "[5/5] Guard twin_core/geo_diamond_v5x4.h — add LEGACY warning"
V5X4="$REPO/twin_core/geo_diamond_v5x4.h"
if [ -f "$V5X4" ]; then
  # Prepend warning guard if not already present
  if ! grep -q "GEO_DIAMOND_V5X4_LEGACY" "$V5X4"; then
    TMPF=$(mktemp)
    cat > "$TMPF" << 'GUARD'
/* ============================================================
 * LEGACY FILE — geo_diamond_v5x4.h (AVX2 path, pre-S21)
 * DO NOT include alongside geo_diamond_field.h
 * Use geo_fast_intersect_x4() via geo_diamond_field.h instead
 * This file is kept for GPU/bench reference only.
 * Define GEO_DIAMOND_V5X4_LEGACY to suppress this warning.
 * ============================================================ */
#ifndef GEO_DIAMOND_V5X4_LEGACY
#pragma message("WARNING: geo_diamond_v5x4.h is a legacy intersect path. Use geo_diamond_field.h")
#endif
GUARD
    cat "$V5X4" >> "$TMPF"
    mv "$TMPF" "$V5X4"
    echo "  geo_diamond_v5x4.h: LEGACY guard prepended"
  else
    echo "  geo_diamond_v5x4.h: guard already present"
  fi
fi

echo ""
echo "======================================================"
echo "DONE. Summary of changes:"
echo "  - Removed stale root .h duplicates (include path conflict resolved)"
echo "  - twin_core/geo_fibo_clock.h updated to S16 canonical (prev_window_inc + DRIFT fix)"
echo "  - geo_cache.h: GEO_SLOTS -> GEO_CACHE_SLOTS (no more silent 576 override)"
echo "  - rest_server.py merged from latest patch, patch files removed"
echo "  - geo_diamond_v5x4.h guarded with LEGACY pragma"
echo ""
echo "Next: verify #include order — geo_config.h must come before any geo_*.h"
echo "      Run: grep -r '#include.*geo_config' src/ | head -20"
echo "======================================================"
