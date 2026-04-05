#define _POSIX_C_SOURCE 200112L

#include "pogls_hydra_scheduler.h"
#include "pogls_node_lut.h"
#include "pogls_detach.h"

#include <string.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════════
   GLOBAL QUEUE TABLE
   ═══════════════════════════════════════════════════════════════════════ */

HydraQueue hydra_queue[HS_HEADS];

/* ═══════════════════════════════════════════════════════════════════════
   INIT
   ═══════════════════════════════════════════════════════════════════════ */

void hs_init(void)
{
    for (int i = 0; i < HS_HEADS; i++) {
        atomic_store(&hydra_queue[i].head, 0);
        atomic_store(&hydra_queue[i].tail, 0);
        memset(hydra_queue[i].tasks, 0,
               sizeof(HydraTask) * HS_QUEUE_SIZE);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   WORK STEALING
   scan other heads; steal from the busiest one found
   ═══════════════════════════════════════════════════════════════════════ */

int hs_steal(int h, HydraTask *out)
{
    for (int i = 0; i < HS_HEADS; i++) {
        if (i == h) continue;

        HydraQueue *q    = &hydra_queue[i];
        uint32_t    head = atomic_load_explicit(&q->head,
                                                 memory_order_acquire);
        uint32_t    tail = atomic_load_explicit(&q->tail,
                                                 memory_order_acquire);

        /* steal only if victim has > 1 item (avoid race on last item) */
        if ((tail - head) <= 1) continue;

        uint32_t new_head = head + 1;
        if (atomic_compare_exchange_strong_explicit(
                &q->head, &head, new_head,
                memory_order_acq_rel,
                memory_order_relaxed)) {
            *out = q->tasks[head % HS_QUEUE_SIZE];
            return 1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   EXECUTE TASK
   ═══════════════════════════════════════════════════════════════════════ */

void hs_execute(HydraWorkerCtx *ctx, const HydraTask *t)
{
    switch ((hs_op_t)t->op) {

        case HS_OP_NODE_WRITE: {
            uint32_t node = node_lut_addr((uint32_t)(t->addr >> 0));
            if (node < NODE_MAX) {
                ctx->node_state->attention[node]++;
                ctx->node_state->density[node]++;
                ctx->node_state->timestamp[node] = ctx->now_ms;
                /* activate frontier */
                ctx->frontier->w[node >> 6] |= (1ULL << (node & 63));
            }
            break;
        }

        case HS_OP_DIFFUSE: {
            uint32_t node = t->node_id;
            if (node < NODE_MAX && ctx->edge_masks) {
                /* one-step diffusion from this node only */
                const NodeMask *e = &ctx->edge_masks[node];
                ctx->frontier->w[0] |= e->w[0];
                ctx->frontier->w[1] |= e->w[1];
                ctx->frontier->w[2] |= e->w[2];
                ctx->frontier->w[3] |= e->w[3];
            }
            break;
        }

        case HS_OP_DETACH:
            /* signal: caller should handle via detach_create()
             * scheduler doesn't hold DetachFrame table — pass via value */
            /* no-op at this layer; WAL already logged via ws_detach() */
            break;

        case HS_OP_DENSITY:
            /* adaptive density check — handled below via hs_density_update */
            if (ctx->density && t->node_id < HS_NODE_MAX_DENSE)
                hs_density_update(ctx->density, t->node_id);
            break;

        default:
            break;
    }

    ctx->tasks_executed++;
}

/* ═══════════════════════════════════════════════════════════════════════
   WORKER LOOP
   ═══════════════════════════════════════════════════════════════════════ */

void *hs_worker_loop(void *arg)
{
    HydraWorkerCtx *ctx = (HydraWorkerCtx *)arg;
    HydraTask       task;

    while (!*ctx->stop) {

        /* refresh timestamp once per loop (not per task — avoid syscall) */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ctx->now_ms = (uint64_t)ts.tv_sec * 1000ULL
                    + (uint64_t)(ts.tv_nsec / 1000000);

        /* 1. pop from own queue */
        if (hs_pop(ctx->head_id, &task)) {
            hs_execute(ctx, &task);
            continue;
        }

        /* 2. steal from another head */
        if (hs_steal(ctx->head_id, &task)) {
            ctx->tasks_stolen++;
            hs_execute(ctx, &task);
            continue;
        }

        /* 3. both empty — spin-wait with backoff */
        hs_cpu_relax();
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
   CPU AFFINITY
   ═══════════════════════════════════════════════════════════════════════ */

int hs_bind_cpu(int hid)
{
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(hid % (int)sizeof(cpu_set_t) * 8, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)hid;
    return 0;   /* no-op on non-Linux */
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
   LAUNCH / SHUTDOWN
   ═══════════════════════════════════════════════════════════════════════ */

int hs_launch(pthread_t threads[HS_HEADS],
              HydraWorkerCtx ctx_array[HS_HEADS])
{
    for (int i = 0; i < HS_HEADS; i++) {
        ctx_array[i].head_id        = i;
        ctx_array[i].tasks_executed = 0;
        ctx_array[i].tasks_stolen   = 0;
        /* caller must set: queues, node_state, frontier,
                           edge_masks, density, stop       */

        int rc = pthread_create(&threads[i], NULL,
                                hs_worker_loop, &ctx_array[i]);
        if (rc != 0) return rc;
    }
    return 0;
}

void hs_shutdown(pthread_t threads[HS_HEADS],
                 HydraWorkerCtx ctx_array[HS_HEADS])
{
    /* signal all workers */
    for (int i = 0; i < HS_HEADS; i++)
        if (ctx_array[i].stop)
            *(ctx_array[i].stop) = 1;

    /* join */
    for (int i = 0; i < HS_HEADS; i++)
        pthread_join(threads[i], NULL);
}

/* ═══════════════════════════════════════════════════════════════════════
   ADAPTIVE NODE DENSITY
   ═══════════════════════════════════════════════════════════════════════ */

void hs_density_init(HydraDensityMap *dm)
{
    memset(dm, 0, sizeof(*dm));

    /* build free-list: slots NODE_MAX..HS_NODE_MAX_DENSE-1 available */
    uint32_t top = 0;
    for (int i = HS_NODE_MAX_DENSE - 1; i >= NODE_MAX; i--)
        dm->free_stack[top++] = (uint16_t)i;

    atomic_store(&dm->free_top, top);
    dm->node_count = NODE_MAX;

    /* base nodes 0..NODE_MAX-1 are leaf by default (zero-init) */
}

uint16_t hs_density_alloc(HydraDensityMap *dm)
{
    uint32_t top = atomic_fetch_sub(&dm->free_top, 1);
    if (top == 0) {
        atomic_fetch_add(&dm->free_top, 1); /* undo */
        return UINT16_MAX;                  /* full */
    }
    return dm->free_stack[top - 1];
}

void hs_density_free(HydraDensityMap *dm, uint16_t id)
{
    uint32_t top              = atomic_fetch_add(&dm->free_top, 1);
    dm->free_stack[top]       = id;
    dm->density[id]           = 0;
    dm->kind[id]              = (uint8_t)HS_NODE_LEAF;
}

void hs_density_update(HydraDensityMap *dm, uint16_t id)
{
    if (id >= HS_NODE_MAX_DENSE) return;
    if (dm->kind[id] != (uint8_t)HS_NODE_LEAF) return;

    uint32_t d = dm->density[id];

    /* SPLIT — node is overloaded */
    if (d > HS_RETOPO_SPLIT_THRESH) {
        uint16_t c1 = hs_density_alloc(dm);
        uint16_t c2 = hs_density_alloc(dm);
        if (c1 == UINT16_MAX || c2 == UINT16_MAX) {
            /* no slots — undo and skip */
            if (c1 != UINT16_MAX) hs_density_free(dm, c1);
            return;
        }
        dm->parent[c1]  = id;
        dm->parent[c2]  = id;
        dm->density[c1] = d >> 1;
        dm->density[c2] = d >> 1;
        dm->kind[c1]    = (uint8_t)HS_NODE_LEAF;
        dm->kind[c2]    = (uint8_t)HS_NODE_LEAF;
        dm->kind[id]    = (uint8_t)HS_NODE_INTERNAL;
        dm->density[id] = 0;
        return;
    }

    /* MERGE — node is underloaded, collapse into parent */
    if (d < HS_RETOPO_MERGE_THRESH && id >= NODE_MAX) {
        uint16_t p = dm->parent[id];
        if (p < HS_NODE_MAX_DENSE) {
            /* accumulate density to parent */
            dm->density[p] += d;
            dm->kind[p]     = (uint8_t)HS_NODE_LEAF;
        }
        hs_density_free(dm, id);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   FULL ENGINE WRITE PATH
   addr → node_map → schedule → diffuse → density_update
   ═══════════════════════════════════════════════════════════════════════ */

void hs_engine_write(HydraWorkerCtx *ctx, uint64_t addr, uint64_t value)
{
    /* 1. address → node */
    uint32_t node = node_lut_addr((uint32_t)addr);

    /* 2. update node state inline (this is the owning head) */
    if (node < NODE_MAX && ctx->node_state) {
        ctx->node_state->attention[node]++;
        ctx->node_state->density[node]++;
        ctx->node_state->timestamp[node] = ctx->now_ms;
        ctx->frontier->w[node >> 6] |= (1ULL << (node & 63));
    }

    /* 3. schedule write task for WAL path (other heads or io thread) */
    HydraTask wt = {
        .node_id = (uint16_t)node,
        .op      = (uint16_t)HS_OP_NODE_WRITE,
        .addr    = addr,
        .value   = value,
    };
    hs_push(hs_route_addr(addr), &wt);

    /* 4. schedule diffuse task */
    HydraTask dt = {
        .node_id = (uint16_t)node,
        .op      = (uint16_t)HS_OP_DIFFUSE,
    };
    hs_push(hs_route_node(node), &dt);

    /* 5. adaptive density update */
    if (ctx->density && node < HS_NODE_MAX_DENSE) {
        ctx->density->density[node]++;

        /* schedule density check if threshold approaching */
        uint32_t d = ctx->density->density[node];
        if (d > HS_RETOPO_SPLIT_THRESH || d < HS_RETOPO_MERGE_THRESH) {
            HydraTask dens = {
                .node_id = (uint16_t)node,
                .op      = (uint16_t)HS_OP_DENSITY,
            };
            hs_push(hs_route_node(node), &dens);
        }
    }
}
