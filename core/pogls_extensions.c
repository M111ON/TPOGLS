/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║     POGLS V3.0 — Extensions                                         ║
 * ║                                                                      ║
 * ║  1. Legacy Wrapper   — V2 compatibility (Smart Header guard)        ║
 * ║  2. Warp Map Decoder — Zero Waste ROI scanner (human-readable)      ║
 * ║  3. File Chunker     — Large file slicing for ComfyUI               ║
 * ║  4. Honeycomb Slot   — Reserved no-op (Stage 2+ placeholder)        ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */

#define _GNU_SOURCE
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "pogls_v3.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* sha256 stub — ในการใช้งานจริงใช้ OpenSSL หรือ mbedTLS */
static void sha256_stub(const uint8_t* data, uint64_t len, uint8_t out[32]) {
    /* Simple checksum สำหรับ prototype — ใช้ XOR folding */
    memset(out, 0, 32);
    for (uint64_t i = 0; i < len; i++) {
        out[i % 32] ^= data[i];
        out[(i + 1) % 32] ^= (uint8_t)(i >> 8);
    }
    /* Mark ว่าเป็น stub — byte 31 = 0xAB */
    out[31] = 0xAB;
}

/* ═══════════════════════════════════════════════════════════════════════
   1. LEGACY WRAPPER — V2 Compatibility
   ---------------------------------------------------------------
   V2 ระบบ (16B) ที่ส่งมาขอข้อมูล จะได้รับแค่ coord_4byte กลับไป
   payload 128B และ warp map 72B จะถูกซ่อนไว้
   
   Smart Header (byte 4 = version) ทำหน้าที่เป็น "Immigration Guard":
     version 0x01 = Legacy 16B → ส่งแค่ 4 bytes
     version 0x03 = V3        → ส่งเต็ม 256B
   ═══════════════════════════════════════════════════════════════════════ */

int pogls_is_v3_block(POGLS_Context* ctx, uint64_t block_index) {
    if (!ctx || !ctx->header) return 0;
    /* ตรวจ magic + version จาก Smart Header ของ vault */
    if (memcmp(ctx->header->magic, "POGL", 4) != 0) return 0;
    return (ctx->header->version == POGLS_VERSION_V3);
}

POGLS_LegacyView pogls_legacy_read(POGLS_Context* ctx, uint64_t block_index) {
    POGLS_LegacyView view;
    memset(&view, 0, sizeof(view));

    if (!ctx) {
        view.is_compatible = 0;
        return view;
    }

    view.version_seen = ctx->header ? ctx->header->version : 0;

    if (!pogls_is_v3_block(ctx, block_index)) {
        /* V2 block — อ่านได้ตรงๆ */
        POGLS_Shadow* s = pogls_shadow_read(ctx, block_index);
        if (s) {
            view.coord_4byte  = (uint32_t)(s->coord_scaled);
            view.is_compatible = 1;
        }
        return view;
    }

    /* V3 block — ส่งเฉพาะ coord_scaled จาก Shadow (4 bytes) */
    POGLS_Shadow* s = pogls_shadow_read(ctx, block_index);
    if (s) {
        /* Legacy ได้เห็นแค่ coord_scaled (4B) — payload ซ่อนอยู่ใน Deep */
        view.coord_4byte   = (uint32_t)(s->coord_scaled);
        view.is_compatible = 1;
        view.version_seen  = POGLS_VERSION_V3;
    }

    return view;
}

/* ═══════════════════════════════════════════════════════════════════════
   2. WARP MAP DECODER — Enhanced (Human-Readable)
   ---------------------------------------------------------------
   เพิ่มเติมจาก angular_mapper.c:
   - นับ active regions
   - print visual map (สำหรับ debug)
   ═══════════════════════════════════════════════════════════════════════ */

int pogls_warp_count_active(POGLS_Deep* block) {
    if (!block) return 0;
    int count = 0;
    for (int i = 0; i < WARP_MAP_SIZE; i++) {
        if (block->warp_map[i] != 0) count++;
    }
    return count;
}

void pogls_warp_print_map(POGLS_Deep* block) {
    if (!block) return;

    int active = pogls_warp_count_active(block);
    int pct    = (active * 100) / WARP_MAP_SIZE;

    printf("┌── Warp Map (%d/%d active = %d%% payload used) ──┐\n",
           active, WARP_MAP_SIZE, pct);

    /* Print เป็น grid 9×8 (72 bytes) */
    printf("│ ");
    for (int i = 0; i < WARP_MAP_SIZE; i++) {
        printf("%c", block->warp_map[i] ? '█' : '·');
        if ((i + 1) % 9 == 0) printf(" │\n│ ");
    }
    printf("\n");
    printf("└── Zero Waste: %d%% of payload SKIPPED ──────────┘\n",
           100 - pct);
}

/* ═══════════════════════════════════════════════════════════════════════
   3. FILE CHUNKER — Large File Slicing (ComfyUI Option A)
   ---------------------------------------------------------------
   600GB file → 6,000 chunks × 100MB
   แต่ละ chunk ได้ Angular Address ของตัวเอง:
     chunk 0 → θ = 0 / total_chunks × 2π → A = floor(θ × 2^n)
     chunk 1 → θ = 1 / total_chunks × 2π → ...
   ═══════════════════════════════════════════════════════════════════════ */

uint64_t pogls_chunk_count(uint64_t file_size, uint64_t chunk_size) {
    if (chunk_size == 0) chunk_size = CHUNK_DEFAULT_SIZE;
    return (file_size + chunk_size - 1) / chunk_size;  /* ceil division */
}

POGLS_Chunk pogls_make_chunk(uint64_t index, uint64_t byte_offset,
                              uint64_t byte_size, uint64_t total_file_size,
                              uint32_t n) {
    POGLS_Chunk chunk;
    memset(&chunk, 0, sizeof(chunk));

    chunk.chunk_index = index;
    chunk.byte_offset = byte_offset;
    chunk.byte_size   = byte_size;
    chunk.is_last     = (byte_offset + byte_size >= total_file_size) ? 1 : 0;

    /* Angular address: แบ่ง [0, 2π) ตามสัดส่วน chunk ใน file
       θ = (index / total_chunks) × 2π
       total_chunks = ceil(total_file_size / chunk_default_size) */
    uint64_t total_chunks = pogls_chunk_count(total_file_size, CHUNK_DEFAULT_SIZE);
    double   two_pi       = 2.0 * M_PI;
    double   theta        = (total_chunks > 1)
                            ? ((double)index / (double)total_chunks) * two_pi
                            : 0.0;

    chunk.angular_address = pogls_compute_address(theta, n);

    /* sha256 ยังไม่มีข้อมูล chunk — จะ fill ใน pogls_chunk_verify */
    memset(chunk.sha256, 0, 32);

    return chunk;
}

int pogls_chunk_verify(const POGLS_Chunk* chunk,
                        const uint8_t* data, uint64_t data_size) {
    if (!chunk || !data) return 0;

    uint8_t computed[32];
    sha256_stub(data, data_size, computed);

    /* ถ้า sha256 เป็น all-zero = ยังไม่เคย set (chunk ใหม่) → return ok */
    uint8_t zero[32];
    memset(zero, 0, 32);
    if (memcmp(chunk->sha256, zero, 32) == 0) return 1;

    return (memcmp(computed, chunk->sha256, 32) == 0) ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   4. HONEYCOMB SLOT — Reserved No-Op (Stage 2+ Placeholder)
   ---------------------------------------------------------------
   ตอนนี้: ทำงานเป็น stub ทั้งหมด
   Stage 2: fill parity_xor[] และ neighbor_count
   ═══════════════════════════════════════════════════════════════════════ */

POGLS_HoneycombSlot pogls_honeycomb_init(void) {
    POGLS_HoneycombSlot slot;
    memset(&slot, 0, sizeof(slot));
    /* neighbor_count = 0 → inactive → ระบบ skip ทั้งหมด */
    slot.neighbor_count   = 0;
    slot.resilience_level = 0;
    return slot;
}

int pogls_honeycomb_is_active(const POGLS_HoneycombSlot* slot) {
    if (!slot) return 0;
    return (slot->neighbor_count > 0) ? 1 : 0;
    /* Stage 2: return 1 เมื่อ fill neighbor_count > 0 */
}

/* ═══════════════════════════════════════════════════════════════════════
   5. CHUNK MAP — แสดง chunk layout ของ file
   ═══════════════════════════════════════════════════════════════════════ */

void pogls_chunk_print_map(uint64_t file_size, uint64_t chunk_size,
                            uint32_t n_bits) {
    if (chunk_size == 0) chunk_size = CHUNK_DEFAULT_SIZE;

    uint64_t total = pogls_chunk_count(file_size, chunk_size);
    double   pct   = 100.0 * chunk_size / file_size;
    double   gb    = file_size / (1024.0 * 1024.0 * 1024.0);

    printf("┌── File Chunk Map ───────────────────────────────┐\n");
    printf("│  File size:    %.2f GB                           \n", gb);
    printf("│  Chunk size:   %llu MB                          \n",
           (unsigned long long)(chunk_size >> 20));
    printf("│  Total chunks: %llu                             \n",
           (unsigned long long)total);
    printf("│  Each chunk:   %.2f%% of file                   \n", pct);
    printf("│  n_bits:       %u (2^%u = %llu addresses)       \n",
           n_bits, n_bits, (1ULL << n_bits));
    printf("├─────────────────────────────────────────────────┤\n");

    /* แสดงแค่ 5 chunks แรกและสุดท้าย */
    uint64_t show = (total < 6) ? total : 5;
    for (uint64_t i = 0; i < show; i++) {
        uint64_t offset = i * chunk_size;
        uint64_t size   = (offset + chunk_size > file_size)
                          ? (file_size - offset) : chunk_size;
        POGLS_Chunk c   = pogls_make_chunk(i, offset, size, file_size, n_bits);
        printf("│  [%4llu]  offset=%8llu MB  A=%10llu\n",
               (unsigned long long)i,
               (unsigned long long)(offset >> 20),
               (unsigned long long)c.angular_address);
    }
    if (total > 6) {
        printf("│   ...  (%llu chunks hidden)\n",
               (unsigned long long)(total - 6));
        uint64_t i      = total - 1;
        uint64_t offset = i * chunk_size;
        uint64_t size   = file_size - offset;
        POGLS_Chunk c   = pogls_make_chunk(i, offset, size, file_size, n_bits);
        printf("│  [%4llu]  offset=%8llu MB  A=%10llu  ← last\n",
               (unsigned long long)i,
               (unsigned long long)(offset >> 20),
               (unsigned long long)c.angular_address);
    }
    printf("└─────────────────────────────────────────────────┘\n");
}
