/*
 * bench3.cu — POGLS38 GPU3
 * Fix 1: true double-buffer (compute stream + copy stream แยกกัน)
 * Fix 2: SoA layout (coalesced memory access)
 * Fix 3: tpb sweep
 * Fix 4: iso check ถูกต้อง (track which buffer แต่ละ addr อยู่)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <cuda_runtime.h>

/* ── SoA (Structure of Arrays) ── */
typedef struct {
    uint64_t *addr;
    uint32_t *morton;
    uint32_t *hilbert;
    uint32_t *phi;
    uint8_t  *lane;
    uint8_t  *audit;
} CoordSoA;

__device__ const uint8_t lut[16]={0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};

/* SoA kernel: ทุก field access coalesced */
__global__ void kernel_soa(
    const uint64_t * __restrict__ addr_in,
    uint32_t * __restrict__ morton_out,
    uint32_t * __restrict__ hilbert_out,
    uint32_t * __restrict__ phi_out,
    uint8_t  * __restrict__ lane_out,
    uint8_t  * __restrict__ audit_out,
    uint32_t n)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    uint64_t a = addr_in[i];
    uint32_t sc = 1u << 20;
    uint32_t t  = (uint32_t)(((a & (sc-1u)) * 1696631ULL) >> 20);
    uint16_t x  = t & 0x3FFu, y = (t >> 10) & 0x3FFu;
    uint32_t rx = x, ry = y;
    rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
    rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
    ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
    ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
    uint32_t mor = rx | (ry << 1);
    uint32_t hil = ((mor >> 4) << 4) | lut[mor & 0xF];
    morton_out[i]  = mor;
    hilbert_out[i] = hil;
    phi_out[i]     = (uint32_t)(((a & (sc-1u)) * 1696631ULL) % sc);
    lane_out[i]    = (uint8_t)(hil % 54u);
    audit_out[i]   = ((a % 17u) == 1u) ? 0u : 1u;
}

/* ── malloc all SoA fields on device ── */
static int soa_malloc(CoordSoA *s, uint32_t N) {
    if (cudaMalloc(&s->addr,    N*sizeof(uint64_t)) != cudaSuccess) return -1;
    if (cudaMalloc(&s->morton,  N*sizeof(uint32_t)) != cudaSuccess) return -1;
    if (cudaMalloc(&s->hilbert, N*sizeof(uint32_t)) != cudaSuccess) return -1;
    if (cudaMalloc(&s->phi,     N*sizeof(uint32_t)) != cudaSuccess) return -1;
    if (cudaMalloc(&s->lane,    N*sizeof(uint8_t))  != cudaSuccess) return -1;
    if (cudaMalloc(&s->audit,   N*sizeof(uint8_t))  != cudaSuccess) return -1;
    return 0;
}
static void soa_free(CoordSoA *s) {
    cudaFree(s->addr); cudaFree(s->morton); cudaFree(s->hilbert);
    cudaFree(s->phi);  cudaFree(s->lane);   cudaFree(s->audit);
}

/*
 * bench_true_dbl — true double-buffer: SEPARATE compute + copy streams
 *
 * copy_stream:    ──[copy A]──[copy B]──[copy C]──
 * compute_stream: ──────[kernel A]──[kernel B]──...
 * event sync:           e_copy e_compute เชื่อมทั้งสอง
 */
double bench_true_dbl(uint32_t N, uint32_t tpb, int reps) {
    if (tpb > 1024) tpb = 1024;
    tpb = ((tpb+31)/32)*32;

    /* pinned host memory (faster PCIe transfer) */
    uint64_t *h_addr_A, *h_addr_B;
    cudaMallocHost(&h_addr_A, N*sizeof(uint64_t));
    cudaMallocHost(&h_addr_B, N*sizeof(uint64_t));
    for (uint32_t i=0;i<N;i++) {
        h_addr_A[i] = (uint64_t)34*i + 1;
        h_addr_B[i] = (uint64_t)34*i + 2;
    }

    /* device double-buffer: buf[0] and buf[1] */
    CoordSoA dev[2];
    soa_malloc(&dev[0], N);
    soa_malloc(&dev[1], N);

    cudaStream_t s_copy, s_compute;
    cudaStreamCreate(&s_copy);
    cudaStreamCreate(&s_compute);

    /* event: copy_done[i] → compute can start on buf[i] */
    cudaEvent_t copy_done[2], compute_done[2];
    for (int i=0;i<2;i++) {
        cudaEventCreate(&copy_done[i]);
        cudaEventCreate(&compute_done[i]);
    }

    uint32_t blk = (N + tpb - 1) / tpb;

    /* prime: copy A into buf[0] */
    cudaMemcpyAsync(dev[0].addr, h_addr_A, N*sizeof(uint64_t),
                    cudaMemcpyHostToDevice, s_copy);
    cudaEventRecord(copy_done[0], s_copy);

    /* warmup */
    cudaStreamWaitEvent(s_compute, copy_done[0]);
    kernel_soa<<<blk,tpb,0,s_compute>>>(dev[0].addr, dev[0].morton,
        dev[0].hilbert, dev[0].phi, dev[0].lane, dev[0].audit, N);
    cudaStreamSynchronize(s_compute);

    /* timing start */
    cudaEvent_t t0, t1;
    cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0);

    for (int rep = 0; rep < reps; rep++) {
        int cur  = rep & 1;
        int next = cur ^ 1;
        uint64_t *next_src = (rep & 1) ? h_addr_A : h_addr_B;

        /* copy stream: load next buffer (while compute works on cur) */
        cudaMemcpyAsync(dev[next].addr, next_src, N*sizeof(uint64_t),
                        cudaMemcpyHostToDevice, s_copy);
        cudaEventRecord(copy_done[next], s_copy);

        /* compute stream: wait for cur buffer to be ready, then kernel */
        cudaStreamWaitEvent(s_compute, copy_done[cur]);
        kernel_soa<<<blk,tpb,0,s_compute>>>(dev[cur].addr, dev[cur].morton,
            dev[cur].hilbert, dev[cur].phi, dev[cur].lane, dev[cur].audit, N);
        cudaEventRecord(compute_done[cur], s_compute);
    }

    /* sync both streams */
    cudaStreamSynchronize(s_copy);
    cudaStreamSynchronize(s_compute);
    cudaEventRecord(t1);
    cudaEventSynchronize(t1);

    float ms = 0;
    cudaEventElapsedTime(&ms, t0, t1);
    double mops = (double)N * reps / (ms / 1000.0) / 1e6;

    /* verify: read last compute buffer */
    int last_cur = (reps - 1) & 1;
    uint8_t *h_audit = (uint8_t*)malloc(N);
    cudaMemcpy(h_audit, dev[last_cur].audit, N, cudaMemcpyDeviceToHost);
    uint64_t *check_src = ((reps-1)&1) ? h_addr_B : h_addr_A;
    uint64_t ok=0,fail=0;
    for (uint32_t i=0;i<N;i++) {
        uint8_t expected = ((check_src[i] % 17u) == 1u) ? 0u : 1u;
        if (h_audit[i] == expected) ok++; else fail++;
    }
    free(h_audit);

    printf("  TRUE_DBL tpb=%-4u batch=%-6uK reps=%-3d : %7.0f M/s  ms=%.2f  iso=%llu/%llu\n",
           tpb, N/1024, reps, mops, ms,
           (unsigned long long)ok, (unsigned long long)(ok+fail));

    cudaFreeHost(h_addr_A); cudaFreeHost(h_addr_B);
    soa_free(&dev[0]); soa_free(&dev[1]);
    cudaStreamDestroy(s_copy); cudaStreamDestroy(s_compute);
    for (int i=0;i<2;i++) {
        cudaEventDestroy(copy_done[i]);
        cudaEventDestroy(compute_done[i]);
    }
    cudaEventDestroy(t0); cudaEventDestroy(t1);
    return mops;
}

/* simple single-stream SoA bench (baseline) */
double bench_simple_soa(uint32_t N, uint32_t tpb, int reps) {
    if (tpb > 1024) tpb = 1024;
    tpb = ((tpb+31)/32)*32;
    uint64_t *h_addr;
    cudaMallocHost(&h_addr, N*sizeof(uint64_t));
    for (uint32_t i=0;i<N;i++) h_addr[i]=(uint64_t)34*i+1;
    CoordSoA dev; soa_malloc(&dev, N);
    cudaMemcpy(dev.addr, h_addr, N*sizeof(uint64_t), cudaMemcpyHostToDevice);
    uint32_t blk=(N+tpb-1)/tpb;
    /* warmup */
    kernel_soa<<<blk,tpb>>>(dev.addr,dev.morton,dev.hilbert,dev.phi,dev.lane,dev.audit,N);
    cudaDeviceSynchronize();
    cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0);
    for(int r=0;r<reps;r++)
        kernel_soa<<<blk,tpb>>>(dev.addr,dev.morton,dev.hilbert,dev.phi,dev.lane,dev.audit,N);
    cudaEventRecord(t1); cudaEventSynchronize(t1);
    float ms=0; cudaEventElapsedTime(&ms,t0,t1);
    double mops=(double)N*reps/(ms/1000.0)/1e6;
    /* verify */
    uint8_t *h_audit=(uint8_t*)malloc(N);
    cudaMemcpy(h_audit,dev.audit,N,cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for(uint32_t i=0;i<N;i++){uint8_t ex=((h_addr[i]%17u)==1u)?0u:1u;
                                if(h_audit[i]==ex)ok++;else fail++;}
    free(h_audit);
    printf("  SIMPLE   tpb=%-4u batch=%-6uK reps=%-3d : %7.0f M/s  ms=%.2f  iso=%llu/%llu\n",
           tpb,N/1024,reps,mops,ms,
           (unsigned long long)ok,(unsigned long long)(ok+fail));
    cudaFreeHost(h_addr); soa_free(&dev);
    cudaEventDestroy(t0); cudaEventDestroy(t1);
    return mops;
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    printf("GPU: %s SM%d.%d  SMs=%d\n\n",p.name,p.major,p.minor,p.multiProcessorCount);

    printf("=== GPU3: SoA + True double-buffer ===\n\n");

    printf("[A] tpb sweep (pure compute, no copy, 512K batch)\n");
    uint32_t tpbs[]={128,192,256,288,320,384,512,1024};
    for(int i=0;i<8;i++) bench_simple_soa(512*1024, tpbs[i], 20);

    printf("\n[B] True double-buffer vs simple (best tpb, 256K)\n");
    bench_simple_soa(256*1024,256,20);
    bench_true_dbl(256*1024,256,20);

    printf("\n[C] True double-buffer scale\n");
    bench_true_dbl(128*1024,256,20);
    bench_true_dbl(256*1024,256,20);
    bench_true_dbl(512*1024,256,20);

    printf("\niso=ok/total (all must be ok=total)\n");
    return 0;
}
