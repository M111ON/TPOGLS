/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║     POGLS V3.6 — Angular Mapper (Integer Edition)                   ║
 * ║                                                                      ║
 * ║  THE LAW: A = floor( θ × 2^n )                                      ║
 * ║                                                                      ║
 * ║  V3.6 change: θ is no longer computed from double.                  ║
 * ║  Input is node index n → Fibonacci integer sampling.                ║
 * ║  No float. No math.h. No M_PI. No 2π.                               ║
 * ║                                                                      ║
 * ║  xyz_to_address: normalized via integer fixed-point (Q20).          ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */

#define _GNU_SOURCE
#include "core/pogls_v3.h"
#include "core/pogls_fibo_addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════
   ICOSPHERE TOPOLOGY TABLE  (unchanged from V3.0)
   ═══════════════════════════════════════════════════════════════════════ */

static const uint32_t TOPO_VERTEX_TABLE[5] = {
    12, 42, 162, 642, 2562,
};

static const uint32_t TOPO_BITS_TABLE[5] = {
    12, 14, 16, 18, 20,
};

/* ═══════════════════════════════════════════════════════════════════════
   CORE ADDRESS COMPUTATION  — integer only
   Input : node index n_idx (replaces double theta)
   Gear  : FIBO_GEAR field (0-15) from CoreSlot
   World : 0 = A (PHI_UP), 1 = B (PHI_DOWN)
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_compute_address(uint32_t n_idx, uint8_t gear, uint8_t world)
{
    return fibo_addr(n_idx, gear, world);
}

/* ── address struct builder ── */
POGLS_AngularAddress pogls_node_to_address(uint32_t n_idx,
                                           uint8_t  gear,
                                           uint8_t  world,
                                           uint32_t n_bits)
{
    POGLS_AngularAddress result;
    memset(&result, 0, sizeof(result));

    result.n       = n_bits;
    result.address = pogls_compute_address(n_idx, gear, world);

    if      (n_bits <= 12) result.topo_level = TOPO_SEED;
    else if (n_bits <= 14) result.topo_level = TOPO_PREVIEW;
    else if (n_bits <= 16) result.topo_level = TOPO_STANDARD;
    else if (n_bits <= 18) result.topo_level = TOPO_HIGH_FIDELITY;
    else                   result.topo_level = TOPO_ULTRA;

    result.vertex_count = pogls_topo_vertex_count(result.topo_level);
    return result;
}

/* ── backward compat shim — ห้าม call ใน V3.6 path ใหม่ ── */
POGLS_AngularAddress pogls_angle_to_address(double theta, uint32_t n)
{
    /* แปลง theta → node index ผ่าน fixed-point แทน float
       θ ∈ [0, 2π) → node = floor(θ / 2π × vertex_max)
       2π ≈ 6,283,185 / 1,000,000 — ใช้ integer fraction
       node = (theta_millirads * vertex_max) / 6283185ULL
       vertex_max เลือกจาก n                                           */
    uint32_t vertex_max;
    if      (n <= 12) vertex_max = 12;
    else if (n <= 14) vertex_max = 42;
    else if (n <= 16) vertex_max = 162;
    else if (n <= 18) vertex_max = 642;
    else              vertex_max = 2562;

    /* theta เป็น double มาจาก caller เก่า — แปลงเป็น millirads integer */
    uint64_t theta_ur = (uint64_t)(theta * 1000000.0); /* micro-radians  */
    uint32_t n_idx = (uint32_t)((theta_ur * vertex_max) / 6283185ULL);
    if (n_idx >= vertex_max) n_idx = vertex_max - 1;

    return pogls_node_to_address(n_idx, 0, 0, n);
}

/* ═══════════════════════════════════════════════════════════════════════
   XYZ TO ADDRESS  — integer fixed-point (Q20)
   val ∈ [range_min, range_max] → node index → fibo_addr
   ไม่มี float หลังจาก normalize เสร็จ
   ═══════════════════════════════════════════════════════════════════════ */

POGLS_AngularAddress pogls_xyz_to_address(double      val,
                                          double      range_min,
                                          double      range_max,
                                          uint32_t    n)
{
    /* clamp */
    if (val < range_min) val = range_min;
    if (val > range_max) val = range_max;

    /* normalize → integer Q20: [0, PHI_SCALE) */
    double span = range_max - range_min;
    uint32_t norm_q20;
    if (span <= 0.0) {
        norm_q20 = 0;
    } else {
        /* หลังบรรทัดนี้ไม่มี float อีก */
        norm_q20 = (uint32_t)(((val - range_min) / span) * (double)PHI_SCALE);
        if (norm_q20 >= PHI_SCALE) norm_q20 = PHI_SCALE - 1;
    }

    /* vertex count from n */
    uint32_t vertex_max;
    if      (n <= 12) vertex_max = 12;
    else if (n <= 14) vertex_max = 42;
    else if (n <= 16) vertex_max = 162;
    else if (n <= 18) vertex_max = 642;
    else              vertex_max = 2562;

    /* Q20 → node index (integer) */
    uint32_t n_idx = (norm_q20 * vertex_max) >> 20;   /* >> 20 = ÷ PHI_SCALE */
    if (n_idx >= vertex_max) n_idx = vertex_max - 1;

    return pogls_node_to_address(n_idx, 0, 0, n);
}

/* ── convenience: full 3D point ── */
POGLS_GeoPoint pogls_map_geo_point(double   x,   double y,   double z,
                                   double   range_min, double range_max,
                                   uint32_t n,   uint32_t topo_level)
{
    POGLS_GeoPoint pt;
    memset(&pt, 0, sizeof(POGLS_GeoPoint));
    pt.addr_x     = pogls_xyz_to_address(x, range_min, range_max, n);
    pt.addr_y     = pogls_xyz_to_address(y, range_min, range_max, n);
    pt.addr_z     = pogls_xyz_to_address(z, range_min, range_max, n);
    pt.topo_level = (uint8_t)topo_level;
    pt.dirty_bits = DIRTY_NONE;
    return pt;
}

/* ═══════════════════════════════════════════════════════════════════════
   INVERSE — address → node index (exact, integer)
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_address_to_node(uint32_t address, uint8_t world)
{
    return world ? fibo_addr_to_node_b(address)
                 : fibo_addr_to_node_a(address);
}

/* shim สำหรับ code เก่าที่รับ double — คืน 0.0 เสมอ (deprecated) */
double pogls_address_to_angle(uint64_t address, uint32_t n)
{
    (void)address; (void)n;
    return 0.0;   /* deprecated — ใช้ pogls_address_to_node() แทน */
}

/* ═══════════════════════════════════════════════════════════════════════
   TOPOLOGY HELPERS  (unchanged)
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_topo_vertex_count(uint32_t topo_level)
{
    if (topo_level > TOPO_ULTRA) topo_level = TOPO_ULTRA;
    return TOPO_VERTEX_TABLE[topo_level];
}

uint32_t pogls_topo_bit_precision(uint32_t topo_level)
{
    if (topo_level > TOPO_ULTRA) topo_level = TOPO_ULTRA;
    return TOPO_BITS_TABLE[topo_level];
}

POGLS_Mode pogls_select_mode(uint32_t topo_level, int is_editing)
{
    if (!is_editing)                    return MODE_REALTIME;
    if (topo_level >= TOPO_ULTRA)       return MODE_WARP;
    if (topo_level >= TOPO_HIGH_FIDELITY) return MODE_DEEP_EDIT;
    return MODE_REALTIME;
}

/* ═══════════════════════════════════════════════════════════════════════
   SHADOW / DEEP LANE  (unchanged)
   ═══════════════════════════════════════════════════════════════════════ */

uint64_t pogls_shadow_offset(uint64_t index) { return index << SHIFT_SHADOW; }
uint64_t pogls_deep_offset  (uint64_t index) { return index << SHIFT_DEEP;   }

POGLS_Shadow* pogls_shadow_read(POGLS_Context *ctx, uint64_t index)
{
    uint64_t off = pogls_shadow_offset(index);
    if (off + SHADOW_BLOCK_SIZE > ctx->mmap_size) return NULL;
    return (POGLS_Shadow*)((uint8_t*)ctx->mmap_base + off);
}

void pogls_shadow_write(POGLS_Context *ctx, uint64_t index,
                        int32_t coord_scaled, uint32_t flags,
                        uint64_t deep_link)
{
    POGLS_Shadow *s = pogls_shadow_read(ctx, index);
    if (!s) return;
    s->coord_scaled = coord_scaled;
    s->vector_flags = flags;
    s->deep_link    = deep_link;
}

POGLS_Deep* pogls_deep_read(POGLS_Context *ctx, uint64_t index)
{
    uint64_t off = pogls_deep_offset(index);
    if (off + DEEP_BLOCK_SIZE > ctx->mmap_size) return NULL;
    return (POGLS_Deep*)((uint8_t*)ctx->mmap_base + off);
}

/* ═══════════════════════════════════════════════════════════════════════
   WARP DECODE  (unchanged)
   ═══════════════════════════════════════════════════════════════════════ */

void pogls_warp_decode(POGLS_Deep *block,
                       void (*process_fn)(uint8_t*, uint8_t))
{
    if (!block || !process_fn) return;
    for (int i = 0; i < WARP_MAP_SIZE; i++) {
        uint8_t flags = block->warp_map[i];
        if (flags == 0) continue;
        int seg_size   = PAYLOAD_SIZE / WARP_MAP_SIZE;
        int seg_offset = i * seg_size;
        if (seg_offset >= PAYLOAD_SIZE) break;
        process_fn(block->payload + seg_offset, flags);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   CHECKER BEAM  — dirty bit เดิม แต่ไม่ recompute theta แล้ว
   ใช้ fibo_addr_to_node_a() verify address drift แทน
   ═══════════════════════════════════════════════════════════════════════ */

void pogls_mark_dirty(POGLS_GeoPoint *point, uint8_t flags)
{
    point->dirty_bits |= flags;
}

int pogls_beam_check(POGLS_Context *ctx, POGLS_GeoPoint *point)
{
    if (point->dirty_bits == DIRTY_NONE && ctx->beam_policy == BEAM_OFF)
        return 1;

    if (point->dirty_bits & DIRTY_TOPO) {
        uint32_t required = pogls_topo_vertex_count(point->topo_level);
        (void)required;
    }

    if (point->dirty_bits & DIRTY_COORD) {
        /* V3.6: verify via inverse — no float needed */
        uint32_t ax      = (uint32_t)point->addr_x.address;
        uint32_t n_back  = fibo_addr_to_node_a(ax);
        uint32_t a_check = fibo_addr_a(n_back);
        if (a_check != ax) return 0;   /* drift detected */
    }

    /* Unit circle priority — outside nodes beam immediately
       twin law: a==b always → 2a² < PHI_SCALE² = inside safe zone
       outside = edge zone, higher audit priority, no dirty bit needed */
    {
        uint64_t a  = (uint64_t)point->addr_x.address;
        uint64_t s2 = (uint64_t)PHI_SCALE * PHI_SCALE;
        if (2 * a * a >= s2) return 0;   /* outside circle → flag for audit */
    }

    point->dirty_bits = DIRTY_NONE;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
   ADAPTIVE BUFFER / CONTEXT  (unchanged)
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_adaptive_buffer_size(POGLS_Mode mode)
{
    switch (mode) {
        case MODE_TIME_TRAVEL: return BUF_TIME_TRAVEL;
        case MODE_REALTIME:    return BUF_REALTIME;
        case MODE_DEEP_EDIT:   return BUF_DEEP_EDIT;
        case MODE_WARP:        return BUF_WARP;
        default:               return BUF_REALTIME;
    }
}

POGLS_Context* pogls_init(const char *vault_path, POGLS_Mode mode,
                           uint32_t n_bits)
{
    POGLS_Context *ctx = (POGLS_Context*)calloc(1, sizeof(POGLS_Context));
    if (!ctx) return NULL;
    ctx->mode        = mode;
    ctx->n_bits      = n_bits;
    ctx->beam_policy = (POGLS_BeamPolicy)mode;
    ctx->buffer_size = pogls_adaptive_buffer_size(mode);
    ctx->work_buffer = (uint8_t*)malloc(ctx->buffer_size);
    if (!ctx->work_buffer) { free(ctx); return NULL; }

    if (vault_path) {
        int fd = open(vault_path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) { free(ctx->work_buffer); free(ctx); return NULL; }
        struct stat st; fstat(fd, &st);
        ctx->mmap_size = (uint64_t)st.st_size;
        if (ctx->mmap_size > 0) {
            ctx->mmap_base = mmap(NULL, ctx->mmap_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_NORESERVE, fd, 0);
            if (ctx->mmap_base != MAP_FAILED)
                madvise(ctx->mmap_base, ctx->mmap_size, MADV_SEQUENTIAL);
            ctx->header = (POGLS_Header*)ctx->mmap_base;
        }
        close(fd);
    }
    return ctx;
}

void pogls_destroy(POGLS_Context *ctx)
{
    if (!ctx) return;
    if (ctx->mmap_base && ctx->mmap_base != MAP_FAILED)
        munmap(ctx->mmap_base, ctx->mmap_size);
    free(ctx->work_buffer);
    free(ctx);
}

/* ═══════════════════════════════════════════════════════════════════════
   DEBUG  — ไม่แสดง theta แล้ว แสดง node_idx แทน
   ═══════════════════════════════════════════════════════════════════════ */

void pogls_print_address(const POGLS_AngularAddress *addr)
{
    uint32_t n_idx = fibo_addr_to_node_a((uint32_t)addr->address);
    printf("  node=%u  |  n=%u  |  A=%llu  |  topo=%u (%u verts)\n",
           n_idx, addr->n,
           (unsigned long long)addr->address,
           addr->topo_level, addr->vertex_count);
}

void pogls_print_context(const POGLS_Context *ctx)
{
    printf("╔══════ POGLS V3.6 Context ══════╗\n");
    printf("  Mode:        %u\n",     ctx->mode);
    printf("  n_bits:      %u  (2^n = %llu addresses)\n",
           ctx->n_bits, (1ULL << ctx->n_bits));
    printf("  Buffer:      %zu bytes\n", ctx->buffer_size);
    printf("  mmap size:   %llu bytes\n", (unsigned long long)ctx->mmap_size);
    printf("  Float:       none\n");
    printf("╚════════════════════════════════╝\n");
}
