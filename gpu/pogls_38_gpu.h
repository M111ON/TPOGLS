/*
 * pogls_38_gpu.h — POGLS38  GPU Pipeline (Colab T4 target)
 * Target: T4 SM 7.5, 2560 cores
 * World 3 block: dim(8,9)=72 threads, warp-aligned at n=4 (288=9×32)
 * 34n+1 isolation: mod17=1 always → no CPU lock needed
 */
#ifndef POGLS_38_GPU_H
#define POGLS_38_GPU_H
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef POGLS_HAVE_CUDA
  #include <cuda_runtime.h>
  #define L38_GPU_AVAILABLE 1
#else
  #define L38_GPU_AVAILABLE 0
  typedef void* cudaStream_t;
#endif
#ifndef PHI_SCALE
  #define PHI_SCALE  (1u<<20)
  #define PHI_UP     1696631u
  #define PHI_DOWN    648055u
#endif
#define L38_W3_DIM_X        8u
#define L38_W3_DIM_Y        9u
#define L38_W3_BASE        72u
#define L38_W3_STRIDE      34u
#define L38_W3_PHASE        1u
#define L38_W3_N4_THREADS 288u   /* 9×32 warps  — T4 sweet spot        */
#define L38_W3_N8_THREADS 576u   /* 18×32 warps                         */
#define L38_GPU_BATCH_T4  (128u*1024u)

/* W3 address: stride=34, isolation=34n+1 mod17=1 always
 * offset = sub-index within a 72-element GPU block
 * addr_base = 34*block_n + 1  (always mod17=1)
 * offset is applied in GPU thread space, not address space */
static inline uint64_t l38_w3_addr(uint32_t block_n, uint32_t sub_offset)
{ /* base addr is isolation-guaranteed, sub_offset used as value key */
  (void)sub_offset;
  return (uint64_t)L38_W3_STRIDE * block_n + L38_W3_PHASE; }

static inline int l38_w3_isolated(uint64_t addr)
{ return (addr % 17u) == 1u; }

typedef struct __attribute__((packed)) {
    uint64_t angular_addr;
    uint32_t morton;
    uint32_t hilbert;
    uint8_t  lane;
    uint8_t  slice_id;
    uint8_t  audit;
    uint8_t  world_flags;
    uint32_t phi_route;
} L38W3Coord;  /* 24B */
typedef char _l38_w3c[(sizeof(L38W3Coord)==24)?1:-1];

static inline uint32_t _l38_ms(uint16_t x){
    uint32_t v=x;
    v=(v|(v<<8))&0x00FF00FFu;v=(v|(v<<4))&0x0F0F0F0Fu;
    v=(v|(v<<2))&0x33333333u;v=(v|(v<<1))&0x55555555u;
    return v;
}
static inline uint32_t _l38_hilbert(uint32_t m){
    static const uint8_t lut[16]={0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};
    return ((m>>4)<<4)|lut[m&0xF];
}
static inline void l38_w3_process_cpu(L38W3Coord *c){
    uint64_t a=c->angular_addr;
    uint32_t t=(uint32_t)(((a&(PHI_SCALE-1u))*(uint64_t)PHI_UP)>>20);
    uint16_t x=(uint16_t)(t&0x3FFu), y=(uint16_t)((t>>10)&0x3FFu);
    c->morton=_l38_ms(x)|(_l38_ms(y)<<1);
    c->hilbert=_l38_hilbert(c->morton);
    c->phi_route=(uint32_t)(((a&(PHI_SCALE-1u))*(uint64_t)PHI_UP)%PHI_SCALE);
    c->lane=(uint8_t)(c->hilbert%54u);
    c->slice_id=2u; c->world_flags=0x04u;
    c->audit=l38_w3_isolated(a)?0u:1u;
}
static inline void l38_w3_batch_cpu(L38W3Coord *c, uint32_t n)
{ for(uint32_t i=0;i<n;i++) l38_w3_process_cpu(&c[i]); }

#ifdef POGLS_HAVE_CUDA
__device__ static const uint8_t _d_lut[16]={0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};
__global__ void l38_w3_kernel(L38W3Coord *c, uint32_t n){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=n)return;
    uint64_t a=c[i].angular_addr;
    uint32_t sc=1u<<20;
    uint32_t t=(uint32_t)(((a&(sc-1u))*1696631ULL)>>20);
    uint16_t x=(uint16_t)(t&0x3FFu),y=(uint16_t)((t>>10)&0x3FFu);
    uint32_t rx=x,ry=y;
    rx=(rx|(rx<<8))&0x00FF00FFu;rx=(rx|(rx<<4))&0x0F0F0F0Fu;
    rx=(rx|(rx<<2))&0x33333333u;rx=(rx|(rx<<1))&0x55555555u;
    ry=(ry|(ry<<8))&0x00FF00FFu;ry=(ry|(ry<<4))&0x0F0F0F0Fu;
    ry=(ry|(ry<<2))&0x33333333u;ry=(ry|(ry<<1))&0x55555555u;
    c[i].morton=rx|(ry<<1);
    c[i].hilbert=((c[i].morton>>4)<<4)|_d_lut[c[i].morton&0xF];
    c[i].phi_route=(uint32_t)(((a&(sc-1u))*1696631ULL)%sc);
    c[i].lane=(uint8_t)(c[i].hilbert%54u);
    c[i].slice_id=2u; c[i].world_flags=0x04u;
    c[i].audit=((a%17u)==1u)?0u:1u;
}
static inline void l38_w3_dispatch(L38W3Coord *d,uint32_t n,
                                    uint8_t wn,cudaStream_t st){
    uint32_t tpb=(wn>=8)?L38_W3_N8_THREADS:L38_W3_N4_THREADS;
    if(tpb>1024u)tpb=1024u;
    uint32_t blk=(n+tpb-1)/tpb;
    l38_w3_kernel<<<blk,tpb,0,st>>>(d,n);
}
#endif

typedef struct {
    L38W3Coord *host_buf;
    uint32_t    capacity, count;
    uint8_t     world3_n;
    int         has_gpu;
    void       *dev_buf, *stream;
    uint64_t    total_coords, total_batches;
    uint64_t    gpu_batches,  cpu_batches;
    uint64_t    isolation_ok, isolation_fail;
} L38GpuCtx;

static inline int l38_gpu_init(L38GpuCtx *ctx, uint32_t cap, uint8_t wn){
    if(!ctx)return -1;
    memset(ctx,0,sizeof(*ctx));
    ctx->capacity=cap?cap:L38_GPU_BATCH_T4;
    ctx->world3_n=(wn>=4)?wn:4u;
    ctx->host_buf=(L38W3Coord*)malloc(ctx->capacity*sizeof(L38W3Coord));
    if(!ctx->host_buf)return -2;
#ifdef POGLS_HAVE_CUDA
    int dc=0;
    if(cudaGetDeviceCount(&dc)==cudaSuccess&&dc>0){
        cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
        printf("[GPU] %s SM%d.%d %.0fMB\n",p.name,p.major,p.minor,p.totalGlobalMem/1e6);
        cudaMalloc(&ctx->dev_buf,ctx->capacity*sizeof(L38W3Coord));
        cudaStreamCreate((cudaStream_t*)&ctx->stream);
        ctx->has_gpu=1;
    }
#endif
    if(!ctx->has_gpu) printf("[W3] No CUDA — CPU fallback\n");
    return 0;
}
static inline void l38_gpu_submit(L38GpuCtx *ctx, uint64_t addr){
    if(!ctx||!ctx->host_buf||ctx->count>=ctx->capacity)return;
    ctx->host_buf[ctx->count++].angular_addr=addr;
}
static inline void l38_gpu_flush(L38GpuCtx *ctx){
    if(!ctx||ctx->count==0)return;
    uint32_t n=ctx->count;
#ifdef POGLS_HAVE_CUDA
    if(ctx->has_gpu&&ctx->dev_buf){
        cudaMemcpyAsync(ctx->dev_buf,ctx->host_buf,n*sizeof(L38W3Coord),
                        cudaMemcpyHostToDevice,(cudaStream_t)ctx->stream);
        l38_w3_dispatch((L38W3Coord*)ctx->dev_buf,n,ctx->world3_n,
                         (cudaStream_t)ctx->stream);
        cudaStreamSynchronize((cudaStream_t)ctx->stream);
        cudaMemcpy(ctx->host_buf,ctx->dev_buf,n*sizeof(L38W3Coord),
                   cudaMemcpyDeviceToHost);
        ctx->gpu_batches++;
    }else
#endif
    { l38_w3_batch_cpu(ctx->host_buf,n); ctx->cpu_batches++; }
    for(uint32_t i=0;i<n;i++){
        if(ctx->host_buf[i].audit==0) ctx->isolation_ok++;
        else ctx->isolation_fail++;
    }
    ctx->total_coords+=n; ctx->total_batches++;
    ctx->count=0;
}
static inline void l38_gpu_free(L38GpuCtx *ctx){
    if(!ctx)return;
    free(ctx->host_buf);
#ifdef POGLS_HAVE_CUDA
    if(ctx->dev_buf)cudaFree(ctx->dev_buf);
    if(ctx->stream)cudaStreamDestroy((cudaStream_t)ctx->stream);
#endif
}
static inline void l38_gpu_stats(const L38GpuCtx *ctx){
    if(!ctx)return;
    printf("W3-GPU: coords=%llu batches=%llu(gpu=%llu cpu=%llu) n=%u block=%u(%u warps)\n",
           (unsigned long long)ctx->total_coords,
           (unsigned long long)ctx->total_batches,
           (unsigned long long)ctx->gpu_batches,
           (unsigned long long)ctx->cpu_batches,
           ctx->world3_n, 72u*ctx->world3_n, 72u*ctx->world3_n/32u);
    printf("  isolation: ok=%llu fail=%llu (fail must=0)\n",
           (unsigned long long)ctx->isolation_ok,
           (unsigned long long)ctx->isolation_fail);
}
#endif
