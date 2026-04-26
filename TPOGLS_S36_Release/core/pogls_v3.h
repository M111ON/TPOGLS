/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║          POGLS V3.0 - Parallel Orbs Geometry Logic System           ║
 * ║          Core Header: Structures, Constants, Declarations           ║
 * ║                                                                      ║
 * ║  Design Law: A = floor( θ × 2^n )                                   ║
 * ║  Zero Waste I/O | Angular Addressing | 2^n Binary Alignment         ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 *  Memory Layout Philosophy:
 *    - Shadow Lane (16B = 2^4): Speed path. Indexing, snapshots.
 *    - Deep Lane  (256B = 2^8): High-fidelity. Geometry DNA, Warp Map, Parity.
 *    - ALL offsets computed via bit-shift. NO multiply/divide inside loops.
 *    - mmap ONLY. No malloc inside processing loops.
 */

#ifndef POGLS_V3_H
#define POGLS_V3_H

#include <stdint.h>
#include <stddef.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS & MAGIC
   ═══════════════════════════════════════════════════════════════════════ */

#define POGLS_MAGIC         "POGL"          /* 4-byte file identity       */
#define POGLS_VERSION_V3    0x03            /* V3 = Hybrid Dual-Lane      */

/* Lane shifts — used for ALL offset arithmetic */
#define SHIFT_SHADOW        4               /* 1 << 4 = 16B  (Shadow)    */
#define SHIFT_DEEP          8               /* 1 << 8 = 256B (Deep)      */
#define SHADOW_BLOCK_SIZE   (1 << SHIFT_SHADOW)   /* 16   */
#define DEEP_BLOCK_SIZE     (1 << SHIFT_DEEP)     /* 256  */

/* Angular addressing: A = floor( θ × 2^n ) */
#define ANGULAR_N_DEFAULT   20              /* 2^20 = 1,048,576 addresses */
#define ANGULAR_FULL_CIRCLE 6.283185307179586   /* 2π                    */

/* Adaptive buffer sizes (2^n) by operation mode */
#define BUF_TIME_TRAVEL     (1 << 16)      /*  64KB — Seek-heavy         */
#define BUF_REALTIME        (1 << 18)      /* 256KB — Flow-optimized     */
#define BUF_DEEP_EDIT       (1 << 20)      /*   1MB — Sweet spot         */
#define BUF_WARP            (1 << 22)      /*   4MB — Max throughput     */

/* Dirty bit flags for Checker Beam on-demand validation */
#define DIRTY_NONE          0x00
#define DIRTY_COORD         0x01
#define DIRTY_PAYLOAD       0x02
#define DIRTY_TOPO          0x04
#define DIRTY_FULL          0xFF

/* Topology levels: Icosphere vertex counts */
#define TOPO_SEED           0              /* Level 0: 2^0  → 12  verts  */
#define TOPO_PREVIEW        1              /* Level 1: 2^2  → 42  verts  */
#define TOPO_STANDARD       2              /* Level 2: 2^4  → 162 verts  */
#define TOPO_HIGH_FIDELITY  3              /* Level 3: 2^6  → 642 verts  */
#define TOPO_ULTRA          4              /* Level 4: 2^8  → 2562 verts */

/* Warp Map size inside Deep Block */
#define WARP_MAP_SIZE       72             /* 72 bytes = 576 ROI bits    */
#define PAYLOAD_SIZE        128            /* Geometry DNA               */
#define PARITY_SIZE         32             /* Stability Parity guard     */

/* Operation modes (Adaptive Lane selector) */
typedef enum {
    MODE_TIME_TRAVEL   = 0,  /* Shadow only, beam OFF                    */
    MODE_REALTIME      = 1,  /* Shadow only, beam SPARSE (anchors)       */
    MODE_DEEP_EDIT     = 2,  /* Hybrid 256B, beam ACTIVE (Warp Map)      */
    MODE_WARP          = 3,  /* Deep only, beam FULL SCAN                */
} POGLS_Mode;

/* Checker Beam scan policy (maps to POGLS_Mode) */
typedef enum {
    BEAM_OFF    = 0,
    BEAM_SPARSE = 1,
    BEAM_ACTIVE = 2,
    BEAM_FULL   = 3,
} POGLS_BeamPolicy;

/* ═══════════════════════════════════════════════════════════════════════
   LAYER 1: SMART HEADER (24 Bytes — The Identity Gate)
   ═══════════════════════════════════════════════════════════════════════
   First thing read. Decides: Shadow-only? Hybrid? Legacy fallback?
   ═══════════════════════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) POGLS_Header {
    char     magic[4];          /* [0x00] "POGL" identifier             */
    uint8_t  version;           /* [0x04] 0x01=Legacy 16B, 0x03=V3      */
    uint8_t  adaptive_level;    /* [0x05] Topology level (2^n scale)    */
    uint16_t ratio_bits;        /* [0x06] x:2^n multiplier ratio        */
    uint64_t total_blocks;      /* [0x08] Total modular blocks in system */
    uint64_t root_offset;       /* [0x10] Byte offset of first block    */
} POGLS_Header;
/* Size check: 4+1+1+2+8+8 = 24 bytes ✓ */

/* ═══════════════════════════════════════════════════════════════════════
   LAYER 2: SHADOW BLOCK (16 Bytes = 2^4 — The Speed Lane)
   ═══════════════════════════════════════════════════════════════════════
   Used for: Quick View, Time Travel snapshots, Delta indexing.
   Offset arithmetic: index << SHIFT_SHADOW
   ═══════════════════════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) POGLS_Shadow {
    int32_t  coord_scaled;      /* [0x00] Angular address (A = floor(θ × 2^n)) scaled */
    uint32_t vector_flags;      /* [0x04] Direction + state bitmask     */
    uint64_t deep_link;         /* [0x08] Offset → Deep Block (0=none)  */
} POGLS_Shadow;
/* Size check: 4+4+8 = 16 bytes ✓ */

/* ═══════════════════════════════════════════════════════════════════════
   LAYER 3: DEEP BLOCK (256 Bytes = 2^8 — The Geometry DNA Lane)
   ═══════════════════════════════════════════════════════════════════════
   Used for: Deep Edit, Warp, High-Fidelity geometry operations.
   Offset arithmetic: index << SHIFT_DEEP

   Binary Layout:
     0x00 - 0x17  (24B)   → Smart Header (embedded per-block copy)
     0x18 - 0x5F  (72B)   → Warp Map     (ROI navigator bit-field)
     0x60 - 0xDF (128B)   → Latent Payload (Geometry DNA)
     0xE0 - 0xFF  (32B)   → Stability Parity (Checksum + Beam state)
   Total: 24+72+128+32 = 256 bytes ✓
   ═══════════════════════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) POGLS_Deep {
    uint8_t  warp_map[WARP_MAP_SIZE];    /* [0x00] ROI density navigator */
    uint8_t  payload[PAYLOAD_SIZE];      /* [0x48] Geometry DNA data     */
    uint8_t  parity[PARITY_SIZE];        /* [0xC8] Stability guard       */
} POGLS_Deep;
/* Size check: 72+128+32 = 232. Combined with inline header = 256 ✓ */

/* ═══════════════════════════════════════════════════════════════════════
   FUTURE-PROOF: HONEYCOMB SLOT (Reserved for Decentralized Stage)
   ---------------------------------------------------------------
   ตอนนี้: reserved ทั้งหมด ไม่มี logic ใดๆ
   Stage 2+: fill neighbor_coords → XOR parity สำหรับ self-healing

   เหตุผลที่จองไว้ตอนนี้: format ไม่แตก ไม่ต้อง migrate ทีหลัง
   ขนาด: 64B (2^6) — aligned ตาม 2^n law
   ═══════════════════════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) {
    uint8_t  parity_xor[32];       /* XOR ของ neighbors — Stage 2 fill  */
    uint8_t  neighbor_count;       /* 0 = inactive (ตอนนี้ทั้งหมด)     */
    uint8_t  resilience_level;     /* 0=off 1=2-way 2=3-way 3=6-way     */
    uint8_t  node_id[8];           /* Decentralized node identity future */
    uint8_t  reserved[22];         /* Padding — อย่าแตะจนถึง Stage 2    */
} POGLS_HoneycombSlot;
/* Size: 32+1+1+8+22 = 64 bytes (2^6) ✓ */

/* ═══════════════════════════════════════════════════════════════════════
   LEGACY COMPATIBILITY: V2 READ RESULT
   Smart Header ส่งเฉพาะ 4-byte coord ให้ V2 ซ่อน payload 128B ไว้
   ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t coord_4byte;           /* สิ่งที่ V2 จะเห็น (4 bytes)      */
    uint8_t  version_seen;          /* version ของ block จริง            */
    uint8_t  is_compatible;         /* 1 = V2 อ่านได้, 0 = V3 only       */
} POGLS_LegacyView;

/* ═══════════════════════════════════════════════════════════════════════
   COMFYUI FILE CHUNK (Large File Slicing — Option A)
   ไฟล์ใหญ่ถูกหั่นเป็น chunks แต่ละ chunk มี Angular Address ของตัวเอง
   ═══════════════════════════════════════════════════════════════════════ */
#define CHUNK_DEFAULT_SIZE  (100ULL * 1024 * 1024)  /* 100MB default     */

typedef struct __attribute__((packed)) {
    uint64_t chunk_index;           /* ลำดับ chunk (0-based)             */
    uint64_t byte_offset;           /* ตำแหน่งใน original file          */
    uint64_t byte_size;             /* ขนาดจริงของ chunk นี้             */
    uint64_t angular_address;       /* A = floor(θ × 2^n) ของ chunk     */
    uint8_t  sha256[32];            /* Integrity hash ของ chunk          */
    uint8_t  is_last;               /* 1 = chunk สุดท้ายของ file        */
    uint8_t  reserved[7];           /* 2^n alignment padding             */
} POGLS_Chunk;
/* Size: 8×4 + 32 + 1 + 7 = 72 bytes */

/* ═══════════════════════════════════════════════════════════════════════
   ANGULAR ADDRESS RECORD
   Result of A = floor(θ × 2^n) — the primary addressing unit.
   ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    double   theta;             /* Original angle in radians [0, 2π)    */
    uint32_t n;                 /* Resolution parameter                 */
    uint64_t address;           /* A = floor(θ × 2^n) — THE address     */
    uint32_t topo_level;        /* Topology level at time of mapping    */
    uint32_t vertex_count;      /* Icosphere vertex count for topo_lvl  */
} POGLS_AngularAddress;

/* ═══════════════════════════════════════════════════════════════════════
   GEOMETRY POINT (3D coordinate in angular address space)
   ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    POGLS_AngularAddress addr_x;   /* X-axis angular address            */
    POGLS_AngularAddress addr_y;   /* Y-axis angular address            */
    POGLS_AngularAddress addr_z;   /* Z-axis angular address            */
    uint8_t  dirty_bits;           /* Checker Beam trigger flags        */
    uint8_t  topo_level;           /* Current topology resolution level */
    uint16_t reserved;             /* 2^n alignment padding             */
} POGLS_GeoPoint;

/* ═══════════════════════════════════════════════════════════════════════
   RUNTIME CONTEXT (holds mmap pointer + operational state)
   NO dynamic allocation post-init. Buffer pre-allocated at calibration.
   ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    void*         mmap_base;       /* mmap base address of vault file   */
    uint64_t      mmap_size;       /* Total mapped size in bytes        */
    POGLS_Header* header;          /* Points into mmap_base[0]          */
    POGLS_Mode    mode;            /* Current operational mode          */
    POGLS_BeamPolicy beam_policy;  /* Current Checker Beam policy       */
    uint32_t      n_bits;          /* Angular resolution: 2^n           */
    size_t        buffer_size;     /* Pre-allocated buffer (2^n bytes)  */
    uint8_t*      work_buffer;     /* Single pre-allocated work buffer  */
    uint32_t      topo_level;      /* Current topology level            */
    uint64_t      shadow_count;    /* Number of Shadow blocks active    */
    uint64_t      deep_count;      /* Number of Deep blocks active      */
} POGLS_Context;

/* ═══════════════════════════════════════════════════════════════════════
   FUNCTION DECLARATIONS
   ═══════════════════════════════════════════════════════════════════════ */

/* Angular Mapper — Core addressing engine */
POGLS_AngularAddress pogls_angle_to_address(double theta, uint32_t n);
double               pogls_address_to_angle(uint64_t address, uint32_t n);
POGLS_AngularAddress pogls_xyz_to_address(double val, double range_min, double range_max, uint32_t n);
uint64_t             pogls_compute_address(double theta, uint32_t n);

/* Topology helpers */
uint32_t  pogls_topo_vertex_count(uint32_t topo_level);
uint32_t  pogls_topo_bit_precision(uint32_t topo_level);
POGLS_Mode pogls_select_mode(uint32_t topo_level, int is_editing);

/* Shadow Lane — fast path */
uint64_t  pogls_shadow_offset(uint64_t index);
void      pogls_shadow_write(POGLS_Context* ctx, uint64_t index,
                             int32_t coord_scaled, uint32_t flags,
                             uint64_t deep_link);
POGLS_Shadow* pogls_shadow_read(POGLS_Context* ctx, uint64_t index);

/* Deep Lane — geometry DNA */
uint64_t      pogls_deep_offset(uint64_t index);
POGLS_Deep*   pogls_deep_read(POGLS_Context* ctx, uint64_t index);
void          pogls_warp_decode(POGLS_Deep* block, void (*process_fn)(uint8_t*, uint8_t));

/* Checker Beam — on-demand validation */
int  pogls_beam_check(POGLS_Context* ctx, POGLS_GeoPoint* point);
void pogls_mark_dirty(POGLS_GeoPoint* point, uint8_t flags);

/* Context lifecycle */
POGLS_Context* pogls_init(const char* vault_path, POGLS_Mode mode, uint32_t n_bits);
void           pogls_destroy(POGLS_Context* ctx);

/* Utility */
uint32_t pogls_adaptive_buffer_size(POGLS_Mode mode);
void     pogls_print_address(const POGLS_AngularAddress* addr);
void     pogls_print_context(const POGLS_Context* ctx);

/* Legacy Wrapper — V2 compatibility */
POGLS_LegacyView pogls_legacy_read(POGLS_Context* ctx, uint64_t block_index);
int              pogls_is_v3_block(POGLS_Context* ctx, uint64_t block_index);

/* Warp Map Decoder — Zero Waste ROI processor */
int  pogls_warp_count_active(POGLS_Deep* block);
void pogls_warp_print_map(POGLS_Deep* block);

/* File Chunker — Large File Slicing (ComfyUI Option A) */
uint64_t     pogls_chunk_count(uint64_t file_size, uint64_t chunk_size);
POGLS_Chunk  pogls_make_chunk(uint64_t index, uint64_t offset,
                               uint64_t size, uint64_t total_size, uint32_t n);
int          pogls_chunk_verify(const POGLS_Chunk* chunk,
                                 const uint8_t* data, uint64_t data_size);

/* Honeycomb Slot — reserved interface (Stage 2+, no-op now) */
POGLS_HoneycombSlot pogls_honeycomb_init(void);  /* returns zeroed slot  */
int pogls_honeycomb_is_active(const POGLS_HoneycombSlot* slot); /* 0 now */

#endif /* POGLS_V3_H */
