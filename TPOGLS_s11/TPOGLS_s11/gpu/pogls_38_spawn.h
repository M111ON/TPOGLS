/*
 * pogls_38_spawn.h — POGLS38  Resource-Aware Head Spawner
 * ══════════════════════════════════════════════════════════════════════
 *
 * Design decision (สำคัญ):
 *   "ตอนแยกร่างจำกัดไว้ 2 หัวก่อน"
 *
 *   POGLS38 split = detach 1 face → spawn ≤ 2 worker heads
 *   ไม่ใช่ 16 ทันที ไม่ใช่ 32 เด็ดขาด
 *
 *   ทำไม 2:
 *     - safe บน single-core machine
 *     - World B ยังไม่มี GPU → CPU ต้องแบ่งกับ user
 *     - แยกร่าง = เพิ่ม depth ไม่ใช่ เพิ่ม width
 *
 * ══════════════════════════════════════════════════════════════════════
 * Resource check (OS):
 *   /proc/loadavg  → cpu_load_1min (float, cheap read)
 *   /proc/meminfo  → MemAvailable (uint64, cheap read)
 *   nproc (cached) → logical CPU count
 *
 * Spawn decision:
 *   if (load_per_core < LOAD_THRESH &&
 *       mem_available > MEM_THRESH &&
 *       cooldown_ticks == 0)
 *       → allowed to spawn
 *
 * Cooldown:
 *   after spawn/kill → cooldown = COOLDOWN_GATE18 × gate_18 ticks
 *   = 9 × 18 = 162 events before next spawn attempt
 *   (162 = NODE_MAX — one full sphere scan worth of events)
 *
 * ══════════════════════════════════════════════════════════════════════
 * State machine per spawned head:
 *
 *   IDLE → SPAWNING → ACTIVE → COOLING → IDLE
 *                          ↓
 *                       KILLING → IDLE
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_SPAWN_H
#define POGLS_38_SPAWN_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "pogls_38_hydra.h"

/* ══════════════════════════════════════════════════════════════════════
 * LIMITS (frozen for split-world phase)
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_SPAWN_MIN_HEADS    1u    /* always keep 1                    */
#define L38_SPAWN_MAX_SPLIT    2u    /* แยกร่าง: max 2 heads            */
#define L38_SPAWN_MAX_FULL    16u    /* full run: max 16 (HS_HEADS)      */

/* Cooldown = 162 gate_18 ticks = 1 full sphere scan
 * เปรียบเทียบ: เหมือน cooldown skill ใน game
 * หลัง spawn/kill ต้องรอ 162 events ก่อน spawn ใหม่ได้ */
#define L38_SPAWN_COOLDOWN_GATE18  9u   /* × 18 = 162 events              */
#define L38_SPAWN_COOLDOWN_TICKS   (L38_SPAWN_COOLDOWN_GATE18 * L38_HS_HEADS)

/* Resource thresholds */
#define L38_LOAD_THRESH_PCT   70u   /* %: load_per_core > 70% → no spawn */
#define L38_MEM_THRESH_MB    256u   /* MB: available < 256MB → no spawn  */

/* ══════════════════════════════════════════════════════════════════════
 * OS RESOURCE READER
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    float    load_1min;      /* /proc/loadavg first field              */
    uint64_t mem_avail_kb;   /* /proc/meminfo MemAvailable             */
    uint32_t ncpu;           /* logical CPU count (cached)             */
    uint8_t  read_ok;        /* 1 = last read succeeded                */
} L38OSResources;

/* Read /proc/loadavg cheaply — one fopen/fscanf/fclose */
static inline int _l38_read_load(float *out)
{
    FILE *f = fopen("/proc/loadavg","r");
    if (!f) { *out = 0.0f; return 0; }
    int r = fscanf(f, "%f", out);
    fclose(f);
    return r == 1;
}

/* Read MemAvailable from /proc/meminfo */
static inline int _l38_read_memavail(uint64_t *out_kb)
{
    FILE *f = fopen("/proc/meminfo","r");
    if (!f) { *out_kb = UINT64_MAX; return 0; }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, " %llu", (unsigned long long *)out_kb);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    *out_kb = UINT64_MAX;
    return 0;
}

/* Read nproc via /proc/cpuinfo */
static inline uint32_t _l38_read_ncpu(void)
{
    FILE *f = fopen("/proc/cpuinfo","r");
    if (!f) return 1;
    uint32_t n = 0;
    char line[128];
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "processor", 9) == 0) n++;
    fclose(f);
    return n > 0 ? n : 1;
}

static inline void l38_os_read(L38OSResources *r)
{
    r->read_ok = 1;
    if (!_l38_read_load(&r->load_1min))    { r->load_1min = 0.0f; r->read_ok = 0; }
    if (!_l38_read_memavail(&r->mem_avail_kb)) { r->mem_avail_kb = UINT64_MAX; r->read_ok = 0; }
    if (r->ncpu == 0) r->ncpu = _l38_read_ncpu();  /* cached after first read */
}

/* load per core as percentage (0..100+) */
static inline uint32_t l38_load_pct(const L38OSResources *r)
{
    if (r->ncpu == 0) return 100u;
    return (uint32_t)(r->load_1min / (float)r->ncpu * 100.0f);
}

/* ══════════════════════════════════════════════════════════════════════
 * SPAWN DECISION
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    L38_SPAWN_OK        = 0,   /* cleared to spawn                      */
    L38_SPAWN_BUSY_CPU  = 1,   /* load too high                         */
    L38_SPAWN_LOW_MEM   = 2,   /* memory too low                        */
    L38_SPAWN_COOLDOWN  = 3,   /* cooldown not expired                  */
    L38_SPAWN_AT_MAX    = 4,   /* already at max_heads                  */
    L38_SPAWN_NO_DEMAND = 5,   /* queue pressure too low to justify     */
} l38_spawn_verdict_t;

static inline const char *l38_spawn_verdict_str(l38_spawn_verdict_t v) {
    switch(v) {
    case L38_SPAWN_OK:        return "OK";
    case L38_SPAWN_BUSY_CPU:  return "CPU_BUSY";
    case L38_SPAWN_LOW_MEM:   return "LOW_MEM";
    case L38_SPAWN_COOLDOWN:  return "COOLDOWN";
    case L38_SPAWN_AT_MAX:    return "AT_MAX";
    case L38_SPAWN_NO_DEMAND: return "NO_DEMAND";
    default:                  return "?";
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * HEAD LIFECYCLE STATE
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    L38_HEAD_IDLE     = 0,
    L38_HEAD_SPAWNING = 1,
    L38_HEAD_ACTIVE   = 2,
    L38_HEAD_COOLING  = 3,   /* post-work, draining before kill         */
    L38_HEAD_KILLING  = 4,
} l38_head_state_t;

/* ══════════════════════════════════════════════════════════════════════
 * SPAWN CONTROLLER
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* config */
    uint8_t   max_heads;       /* L38_SPAWN_MAX_SPLIT (2) or MAX_FULL   */
    uint8_t   min_heads;       /* L38_SPAWN_MIN_HEADS (1)               */
    uint8_t   load_thresh_pct; /* L38_LOAD_THRESH_PCT (70)              */
    uint8_t   _pad;
    uint32_t  mem_thresh_mb;   /* L38_MEM_THRESH_MB (256)               */

    /* state */
    uint32_t  active_heads;    /* currently active                      */
    uint32_t  active_mask;     /* bit N = head N is active              */
    uint32_t  cooldown_ticks;  /* gate_18 ticks remaining               */
    uint32_t  gate18_count;    /* total gate_18 events seen             */

    /* per-head state */
    uint8_t   head_state[L38_HS_HEADS];  /* l38_head_state_t            */
    uint32_t  head_work[L38_HS_HEADS];   /* tasks processed per head     */

    /* OS resources (refreshed on each spawn check) */
    L38OSResources os;

    /* stats */
    uint32_t  total_spawns;
    uint32_t  total_kills;
    uint32_t  spawn_denied_cpu;
    uint32_t  spawn_denied_mem;
    uint32_t  spawn_denied_cooldown;

} L38SpawnCtrl;

#define L38_SPAWN_MAGIC  0x53505743u  /* "SPWC" */

static inline void l38_spawn_init(L38SpawnCtrl *sc,
                                    uint8_t       max_heads,
                                    uint8_t       min_heads)
{
    memset(sc, 0, sizeof(*sc));
    sc->max_heads        = (max_heads == 0) ? L38_SPAWN_MAX_SPLIT : max_heads;
    sc->min_heads        = (min_heads == 0) ? L38_SPAWN_MIN_HEADS : min_heads;
    sc->load_thresh_pct  = L38_LOAD_THRESH_PCT;
    sc->mem_thresh_mb    = L38_MEM_THRESH_MB;

    /* start with min_heads active */
    for (uint8_t i = 0; i < sc->min_heads; i++) {
        sc->active_mask    |= (1u << i);
        sc->head_state[i]   = L38_HEAD_ACTIVE;
    }
    sc->active_heads = sc->min_heads;

    /* cache ncpu immediately */
    sc->os.ncpu = _l38_read_ncpu();
}

/* gate_18 tick — decrement cooldown, refresh OS */
static inline void l38_spawn_gate18(L38SpawnCtrl *sc)
{
    sc->gate18_count++;
    if (sc->cooldown_ticks > 0) sc->cooldown_ticks--;
}

/* check if spawn is allowed — reads OS on call */
static inline l38_spawn_verdict_t l38_spawn_check(L38SpawnCtrl *sc,
                                                     const L38Hydra *h)
{
    /* already at max */
    if (sc->active_heads >= sc->max_heads)
        return L38_SPAWN_AT_MAX;

    /* cooldown */
    if (sc->cooldown_ticks > 0)
        return L38_SPAWN_COOLDOWN;

    /* read OS resources (skip if caller pre-populated for testing) */
    if (!sc->os.read_ok) l38_os_read(&sc->os);
    /* refresh on next call after reading */
    else sc->os.read_ok = 0;  /* consume once, re-read next time */

    /* CPU load check */
    uint32_t load = l38_load_pct(&sc->os);
    if (load > sc->load_thresh_pct) {
        sc->spawn_denied_cpu++;
        return L38_SPAWN_BUSY_CPU;
    }

    /* memory check */
    uint64_t mem_mb = sc->os.mem_avail_kb / 1024u;
    if (mem_mb < sc->mem_thresh_mb) {
        sc->spawn_denied_mem++;
        return L38_SPAWN_LOW_MEM;
    }

    /* demand check: is there actually work to justify a new head?
     * require average queue depth > threshold across active heads */
    uint32_t total_d = l38_hydra_total_depth(h);
    uint32_t demand_thresh = sc->active_heads * 32u;  /* 32 tasks/head */
    if (total_d < demand_thresh)
        return L38_SPAWN_NO_DEMAND;

    return L38_SPAWN_OK;
}

/* spawn: activate next idle head, set cooldown */
static inline int l38_spawn_head(L38SpawnCtrl *sc)
{
    if (sc->active_heads >= sc->max_heads) return -1;

    /* find first idle head */
    for (uint32_t i = 0; i < L38_HS_HEADS; i++) {
        if (sc->head_state[i] == L38_HEAD_IDLE) {
            sc->active_mask   |= (1u << i);
            sc->head_state[i]  = L38_HEAD_SPAWNING;
            sc->active_heads++;
            sc->total_spawns++;
            sc->cooldown_ticks = L38_SPAWN_COOLDOWN_TICKS;
            return (int)i;   /* caller wires the head */
        }
    }
    return -1;  /* no idle slot */
}

/* confirm spawned head is running */
static inline void l38_spawn_confirm(L38SpawnCtrl *sc, uint32_t head_id)
{
    if (head_id < L38_HS_HEADS)
        sc->head_state[head_id] = L38_HEAD_ACTIVE;
}

/* kill: begin shutdown of a head (enter cooling) */
static inline int l38_kill_head(L38SpawnCtrl *sc, uint32_t head_id)
{
    if (sc->active_heads <= sc->min_heads) return -1;  /* keep minimum */
    if (head_id >= L38_HS_HEADS) return -1;
    if (sc->head_state[head_id] != L38_HEAD_ACTIVE) return -1;

    sc->head_state[head_id] = L38_HEAD_COOLING;
    sc->cooldown_ticks      = L38_SPAWN_COOLDOWN_TICKS;
    return 0;
}

/* confirm head drained + killed */
static inline void l38_kill_confirm(L38SpawnCtrl *sc, uint32_t head_id)
{
    if (head_id >= L38_HS_HEADS) return;
    sc->active_mask   &= ~(1u << head_id);
    sc->head_state[head_id] = L38_HEAD_IDLE;
    sc->active_heads--;
    sc->total_kills++;
}

/* auto-scale: check conditions and spawn/kill as appropriate */
static inline l38_spawn_verdict_t l38_autoscale(L38SpawnCtrl *sc,
                                                   L38Hydra      *h)
{
    l38_spawn_verdict_t v = l38_spawn_check(sc, h);

    if (v == L38_SPAWN_OK) {
        int hid = l38_spawn_head(sc);
        if (hid >= 0) l38_spawn_confirm(sc, (uint32_t)hid);
        return L38_SPAWN_OK;
    }

    /* ALWAYS check kill when system is under pressure
     * (even if AT_MAX — need to shed load regardless) */
    int should_kill = (v == L38_SPAWN_BUSY_CPU || v == L38_SPAWN_LOW_MEM);
    if (!should_kill && (v == L38_SPAWN_AT_MAX || v == L38_SPAWN_COOLDOWN)) {
        /* even at max/cooldown: check real load */
        if (!sc->os.read_ok) l38_os_read(&sc->os);
        else sc->os.read_ok = 0;
        uint32_t load = l38_load_pct(&sc->os);
        uint64_t mem_mb = sc->os.mem_avail_kb / 1024u;
        should_kill = (load > sc->load_thresh_pct || mem_mb < sc->mem_thresh_mb);
    }

    if (should_kill && sc->active_heads > sc->min_heads && sc->cooldown_ticks == 0) {
        /* find least busy active head (not head 0) */
        uint32_t least_work = UINT32_MAX, kill_id = 0;
        int found = 0;
        for (uint32_t i = 1; i < L38_HS_HEADS; i++) {
            if (sc->head_state[i] != L38_HEAD_ACTIVE) continue;
            if (l38_hs_depth(&h->queues[i]) == 0 &&
                sc->head_work[i] < least_work) {
                least_work = sc->head_work[i];
                kill_id    = i;
                found      = 1;
            }
        }
        if (found) {
            l38_kill_head(sc, kill_id);
            if (l38_hs_depth(&h->queues[kill_id]) == 0)
                l38_kill_confirm(sc, kill_id);
        }
    }

    return v;
}

/* print status */
static inline void l38_spawn_status(const L38SpawnCtrl *sc)
{
    printf("SpawnCtrl: active=%u/%u  cooldown=%u  gate18=%u\n",
           sc->active_heads, sc->max_heads,
           sc->cooldown_ticks, sc->gate18_count);
    printf("  OS: load=%.2f (%u%%)  mem=%llu MB  ncpu=%u\n",
           sc->os.load_1min,
           l38_load_pct(&sc->os),
           (unsigned long long)(sc->os.mem_avail_kb/1024),
           sc->os.ncpu);
    printf("  stats: spawns=%u kills=%u denied(cpu=%u mem=%u cd=%u)\n",
           sc->total_spawns, sc->total_kills,
           sc->spawn_denied_cpu, sc->spawn_denied_mem,
           sc->spawn_denied_cooldown);
    printf("  heads:");
    for (uint32_t i=0;i<L38_HS_HEADS;i++) {
        if (sc->head_state[i] != L38_HEAD_IDLE) {
            const char *s[] = {"IDLE","SPAWN","ACTIVE","COOL","KILL"};
            printf(" %u:%s", i, s[sc->head_state[i]]);
        }
    }
    printf("\n");
}

#endif /* POGLS_38_SPAWN_H */
