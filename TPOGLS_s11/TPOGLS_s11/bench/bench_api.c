#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pogls_api.h"

#define MB (1024*1024)
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec*1000.0 + t.tv_nsec/1e6;
}

static void bench(const char *label, const uint8_t *src, size_t len, int reps)
{
    size_t bound = pogls_encode_bound(len);
    uint8_t *blob = malloc(bound), *out = malloc(len+64);
    size_t blen = bound, olen;

    blen = bound; pogls_encode(src, len, blob, &blen); /* warmup */

    double t0 = now_ms();
    for (int i = 0; i < reps; i++) { blen = bound; pogls_encode(src, len, blob, &blen); }
    double enc_ms = (now_ms()-t0)/reps;

    t0 = now_ms();
    for (int i = 0; i < reps; i++) { olen = len+64; pogls_decode(blob, blen, out, &olen); }
    double dec_ms = (now_ms()-t0)/reps;

    double mb = (double)len/MB;
    printf("  %-24s │ enc %7.2f MB/s │ dec %7.2f MB/s │ blob %5.1fx orig\n",
           label, mb/(enc_ms/1000.0), mb/(dec_ms/1000.0), (double)blen/(double)len);
    free(blob); free(out);
}

int main(void) {
    puts("════════════════════════════════════════════════════════════════════");
    puts("  POGLS Throughput Benchmark");
    puts("════════════════════════════════════════════════════════════════════");

    size_t sizes[] = {4*1024, 64*1024, 1*MB};
    const char *sl[] = {"4KB","64KB","1MB"};

    for (int s = 0; s < 3; s++) {
        size_t len = sizes[s];
        int reps = (len<=4096)?2000:(len<=65536)?300:30;
        printf("\n── %s (%d reps) ──\n", sl[s], reps);

        uint8_t *t = malloc(len);
        for (size_t i=0;i<len;i++) t[i]=(uint8_t)('a'+(i%26));
        bench("text-like (low entropy)", t, len, reps); free(t);

        uint8_t *r = malloc(len); uint32_t v=0xCAFEBABEu;
        for (size_t i=0;i<len;i+=4){v=v*1664525u+1013904223u;memcpy(r+i,&v,4);}
        bench("random (high entropy)",   r, len, reps); free(r);

        uint8_t *p = malloc(len); v=0x9E3779B9u;
        for (size_t i=0;i<len;i+=4){v=v*0x9E3779B9u+1;memcpy(p+i,&v,4);}
        bench("PHI-seeded",              p, len, reps); free(p);

        uint8_t *z = calloc(1,len);
        bench("zero-run (max compress)", z, len, reps); free(z);
    }
    puts("\n════════════════════════════════════════════════════════════════════");
    puts("  blob ratio < 1.0x = compressed. >1.0x = overhead dominates (small files)");
    return 0;
}
