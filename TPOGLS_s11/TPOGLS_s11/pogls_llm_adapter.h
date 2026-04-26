/*
 * pogls_llm_adapter.h — LLM Orchestration Adapter for POGLS38
 * ══════════════════════════════════════════════════════════════
 *
 * Maps POGLS38 wire/lane concept → LLM specialist dispatch
 *
 * lane % 3 routing (mod17 family):
 *   lane % 3 == 0  → GENERAL  (Qwen2.5-0.5B)
 *   lane % 3 == 1  → CODER    (Qwen2.5-Coder-1.5B)
 *   lane % 3 == 2  → MATH     (Qwen2.5-Math or fallback)
 *
 * Parallel dispatch: fire all needed specialists simultaneously
 * via non-blocking HTTP, collect with timeout (mirrors HEAD_A/B pattern)
 *
 * Dependencies: libcurl
 * Build: gcc ... -lcurl
 * ══════════════════════════════════════════════════════════════
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <curl/curl.h>

/* ── Config ────────────────────────────────────────────────── */
#define LLM_MAX_SPECIALISTS   4
#define LLM_MAX_PROMPT_LEN    4096
#define LLM_MAX_RESPONSE_LEN  8192
#define LLM_TIMEOUT_SEC       10

/* ── Specialist IDs (maps to lane % 3) ─────────────────────── */
typedef enum {
    LLM_GENERAL = 0,   /* lane % 3 == 0 */
    LLM_CODER   = 1,   /* lane % 3 == 1 */
    LLM_MATH    = 2,   /* lane % 3 == 2 */
} LLMSpecialistID;

/* ── Endpoint registry ──────────────────────────────────────── */
typedef struct {
    char     url[256];          /* Cloudflare tunnel URL */
    char     model_name[64];    /* model identifier */
    uint8_t  active;
} LLMEndpoint;

/* ── Request context (per parallel worker) ──────────────────── */
typedef struct {
    LLMEndpoint    *endpoint;
    char            prompt[LLM_MAX_PROMPT_LEN];
    char            response[LLM_MAX_RESPONSE_LEN];
    uint32_t        max_tokens;
    double          latency_ms;
    int             success;
    pthread_t       thread;
} LLMRequest;

/* ── Adapter context ────────────────────────────────────────── */
typedef struct {
    LLMEndpoint     specialists[LLM_MAX_SPECIALISTS];
    uint32_t        specialist_count;
    uint64_t        total_requests;
    uint64_t        cache_hits;       /* TailsDNA cache */
    /* simple fingerprint cache: last 16 queries */
    uint32_t        cache_fp[16];
    char            cache_resp[16][LLM_MAX_RESPONSE_LEN];
    uint8_t         cache_head;
} LLMAdapterCtx;

/* ── curl write callback ─────────────────────────────────────── */
typedef struct { char *buf; size_t len; size_t cap; } CurlBuf;

static size_t _curl_write(void *data, size_t sz, size_t nmemb, void *userp) {
    CurlBuf *b = (CurlBuf *)userp;
    size_t add = sz * nmemb;
    if (b->len + add + 1 > b->cap) return 0; /* overflow guard */
    memcpy(b->buf + b->len, data, add);
    b->len += add;
    b->buf[b->len] = '\0';
    return add;
}

/* ── Simple FNV-1a fingerprint ──────────────────────────────── */
static inline uint32_t _llm_fp(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* ── Cache lookup (TailsDNA analog) ────────────────────────── */
static const char* _llm_cache_get(LLMAdapterCtx *ctx, const char *prompt) {
    uint32_t fp = _llm_fp(prompt);
    for (int i = 0; i < 16; i++) {
        if (ctx->cache_fp[i] == fp && ctx->cache_resp[i][0])
            return ctx->cache_resp[i];
    }
    return NULL;
}

static void _llm_cache_put(LLMAdapterCtx *ctx, const char *prompt, const char *resp) {
    uint32_t fp = _llm_fp(prompt);
    uint8_t idx = ctx->cache_head & 15u;
    ctx->cache_fp[idx] = fp;
    strncpy(ctx->cache_resp[idx], resp, LLM_MAX_RESPONSE_LEN - 1);
    ctx->cache_head++;
}

/* ── Single HTTP inference call ─────────────────────────────── */
static int llm_infer(LLMEndpoint *ep, const char *prompt,
                     uint32_t max_tokens, char *out, size_t out_cap)
{
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    /* Build JSON body */
    char body[LLM_MAX_PROMPT_LEN + 256];
    snprintf(body, sizeof(body),
        "{\"model\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"max_tokens\":%u}",
        ep->model_name, prompt, max_tokens);

    char raw[LLM_MAX_RESPONSE_LEN] = {0};
    CurlBuf cb = { raw, 0, sizeof(raw) - 1 };

    char endpoint[320];
    snprintf(endpoint, sizeof(endpoint), "%s/v1/chat/completions", ep->url);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)LLM_TIMEOUT_SEC);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return 0;

    /* Extract content from JSON — find "content":" */
    char *p = strstr(raw, "\"content\":\"");
    if (!p) return 0;
    p += 11;
    char *end = p;
    while (*end && *end != '"') end++;
    size_t len = (size_t)(end - p);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/* ── Parallel worker ─────────────────────────────────────────── */
static void* _llm_worker(void *arg) {
    LLMRequest *req = (LLMRequest *)arg;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    req->success = llm_infer(req->endpoint, req->prompt,
                             req->max_tokens, req->response,
                             LLM_MAX_RESPONSE_LEN);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    req->latency_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                    + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    return NULL;
}

/* ── Init ────────────────────────────────────────────────────── */
static inline void llm_adapter_init(LLMAdapterCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static inline void llm_adapter_add(LLMAdapterCtx *ctx,
                                   const char *url, const char *model) {
    if (ctx->specialist_count >= LLM_MAX_SPECIALISTS) return;
    LLMEndpoint *ep = &ctx->specialists[ctx->specialist_count++];
    strncpy(ep->url, url, 255);
    strncpy(ep->model_name, model, 63);
    ep->active = 1;
}

/* ══════════════════════════════════════════════════════════════
 * llm_dispatch_lane
 *
 * POGLS38 integration point:
 *   Takes lane value from wire pipeline → route to specialist
 *   Cache check first (TailsDNA analog)
 *   Returns response in out buffer
 *
 * lane % 3 → specialist index
 * ══════════════════════════════════════════════════════════════ */
static inline int llm_dispatch_lane(LLMAdapterCtx *ctx,
                                    uint8_t lane,
                                    const char *prompt,
                                    uint32_t max_tokens,
                                    char *out, size_t out_cap)
{
    ctx->total_requests++;

    /* Cache hit → skip inference */
    const char *cached = _llm_cache_get(ctx, prompt);
    if (cached) {
        ctx->cache_hits++;
        strncpy(out, cached, out_cap - 1);
        return 1;
    }

    /* Route by lane % 3 */
    uint32_t idx = lane % 3u;
    if (idx >= ctx->specialist_count) idx = 0;

    LLMEndpoint *ep = &ctx->specialists[idx];
    if (!ep->active) return 0;

    int ok = llm_infer(ep, prompt, max_tokens, out, out_cap);
    if (ok) _llm_cache_put(ctx, prompt, out);
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * llm_dispatch_parallel
 *
 * Dispatch to multiple specialists simultaneously
 * Maps to HEAD_A/HEAD_B parallel write pattern in SplitWorld
 *
 * specialists[]  — array of specialist indices to query
 * n_specialists  — how many
 * prompts[]      — prompt per specialist (or same for all)
 * results[]      — output array (must be pre-allocated)
 *
 * Returns: bitmask of successful responses
 * ══════════════════════════════════════════════════════════════ */
static inline uint32_t llm_dispatch_parallel(LLMAdapterCtx *ctx,
                                              uint32_t *specialist_ids,
                                              const char **prompts,
                                              uint32_t n,
                                              uint32_t max_tokens,
                                              char results[][LLM_MAX_RESPONSE_LEN])
{
    if (n > LLM_MAX_SPECIALISTS) n = LLM_MAX_SPECIALISTS;

    LLMRequest reqs[LLM_MAX_SPECIALISTS] = {0};

    /* Launch all threads */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = specialist_ids[i];
        if (idx >= ctx->specialist_count) continue;
        reqs[i].endpoint   = &ctx->specialists[idx];
        reqs[i].max_tokens = max_tokens;
        strncpy(reqs[i].prompt, prompts[i], LLM_MAX_PROMPT_LEN - 1);
        pthread_create(&reqs[i].thread, NULL, _llm_worker, &reqs[i]);
    }

    /* Collect — total time = slowest worker */
    uint32_t success_mask = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!reqs[i].endpoint) continue;
        pthread_join(reqs[i].thread, NULL);
        if (reqs[i].success) {
            success_mask |= (1u << i);
            strncpy(results[i], reqs[i].response, LLM_MAX_RESPONSE_LEN - 1);
            printf("[LLM] specialist[%u] %.1fms: %s\n",
                   i, reqs[i].latency_ms, reqs[i].response);
        }
    }
    return success_mask;
}

static inline void llm_adapter_stats(const LLMAdapterCtx *ctx) {
    uint64_t t = ctx->total_requests ? ctx->total_requests : 1;
    printf("\n[LLM Adapter Stats]\n");
    printf("  Total requests : %llu\n", (unsigned long long)ctx->total_requests);
    printf("  Cache hits     : %llu (%llu%%)\n",
           (unsigned long long)ctx->cache_hits,
           (unsigned long long)(ctx->cache_hits * 100 / t));
    printf("  Specialists    : %u\n", ctx->specialist_count);
}
