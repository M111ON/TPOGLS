#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <cuda_runtime.h>

typedef struct __attribute__((packed)) {
    uint64_t addr; uint32_t morton,hilbert;
    uint8_t lane,slice,audit,flags; uint32_t phi;
} Coord;

__device__ const uint8_t lut[16]={0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};
__global__ void kernel(Coord *c, uint32_t n){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=n)return;
    uint64_t a=c[i].addr; uint32_t sc=1u<<20;
    uint32_t t=(uint32_t)(((a&(sc-1u))*1696631ULL)>>20);
    uint16_t x=t&0x3FFu,y=(t>>10)&0x3FFu;
    uint32_t rx=x,ry=y;
    rx=(rx|(rx<<8))&0x00FF00FFu;rx=(rx|(rx<<4))&0x0F0F0F0Fu;
    rx=(rx|(rx<<2))&0x33333333u;rx=(rx|(rx<<1))&0x55555555u;
    ry=(ry|(ry<<8))&0x00FF00FFu;ry=(ry|(ry<<4))&0x0F0F0F0Fu;
    ry=(ry|(ry<<2))&0x33333333u;ry=(ry|(ry<<1))&0x55555555u;
    c[i].morton=rx|(ry<<1);
    c[i].hilbert=((c[i].morton>>4)<<4)|lut[c[i].morton&0xF];
    c[i].phi=(uint32_t)(((a&(sc-1u))*1696631ULL)%sc);
    c[i].lane=(uint8_t)(c[i].hilbert%54u);
    c[i].audit=((a%17u)==1u)?0u:1u;
}

/* ─── FIX: sync streams BEFORE recording t1 ─── */
double bench_streams(int n_streams, uint32_t N, uint32_t tpb){
    if(tpb>1024)tpb=1024;
    tpb=((tpb+31)/32)*32;
    Coord **h=(Coord**)malloc(n_streams*sizeof(Coord*));
    Coord **hb=(Coord**)malloc(n_streams*sizeof(Coord*));
    void **d=(void**)malloc(n_streams*sizeof(void*));
    void **db=(void**)malloc(n_streams*sizeof(void*));
    cudaStream_t *st=(cudaStream_t*)malloc(n_streams*sizeof(cudaStream_t));
    for(int s=0;s<n_streams;s++){
        h[s]=(Coord*)malloc(N*sizeof(Coord));
        hb[s]=(Coord*)malloc(N*sizeof(Coord));
        cudaMalloc(&d[s],N*sizeof(Coord));
        cudaMalloc(&db[s],N*sizeof(Coord));
        cudaStreamCreate(&st[s]);
        for(uint32_t i=0;i<N;i++){
            h[s][i].addr=(uint64_t)34*(i+s*N)+1;
            hb[s][i].addr=(uint64_t)34*(i+s*N+n_streams*N)+1;
        }
    }
    uint32_t blk=(N+tpb-1)/tpb;

    /* warmup — ทุก stream */
    for(int s=0;s<n_streams;s++){
        cudaMemcpyAsync(d[s],h[s],N*sizeof(Coord),cudaMemcpyHostToDevice,st[s]);
        kernel<<<blk,tpb,0,st[s]>>>((Coord*)d[s],N);
    }
    for(int s=0;s<n_streams;s++) cudaStreamSynchronize(st[s]);

    cudaEvent_t t0,t1;
    cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0); /* ← start AFTER warmup, on default stream */

    /* double-buffer: 10 reps */
    for(int rep=0;rep<10;rep++){
        for(int s=0;s<n_streams;s++){
            kernel<<<blk,tpb,0,st[s]>>>((Coord*)d[s],N);
            cudaMemcpyAsync(db[s],hb[s],N*sizeof(Coord),cudaMemcpyHostToDevice,st[s]);
            void *tmp=d[s]; d[s]=db[s]; db[s]=tmp;
        }
    }

    /* ── FIX: sync first, THEN record t1 ── */
    for(int s=0;s<n_streams;s++) cudaStreamSynchronize(st[s]);
    cudaEventRecord(t1);
    cudaEventSynchronize(t1); /* wait for t1 to complete */

    float ms=0; cudaEventElapsedTime(&ms,t0,t1);
    double total=(double)N*n_streams*10;
    double mops=total/(ms/1000.0)/1e6;

    /* verify isolation */
    cudaMemcpy(h[0],d[0],N*sizeof(Coord),cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for(uint32_t i=0;i<N;i++){if(h[0][i].audit==0)ok++;else fail++;}
    printf("  streams=%-2d tpb=%-4u batch=%-6uK : %7.0f M/s  ms=%.2f  iso=%llu/%llu\n",
           n_streams,tpb,N/1024,mops,ms,
           (unsigned long long)ok,(unsigned long long)(ok+fail));

    for(int s=0;s<n_streams;s++){
        free(h[s]);free(hb[s]);cudaFree(d[s]);cudaFree(db[s]);
        cudaStreamDestroy(st[s]);
    }
    free(h);free(hb);free(d);free(db);free(st);
    cudaEventDestroy(t0);cudaEventDestroy(t1);
    return mops;
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    printf("GPU: %s SM%d.%d\n\n",p.name,p.major,p.minor);
    printf("=== GPU2 Fixed: sync-before-record timing ===\n\n");
    uint32_t tpb=288;

    printf("[Streams: soft limit test — batch=128K]\n");
    bench_streams(1,128*1024,tpb);
    bench_streams(2,128*1024,tpb);
    bench_streams(3,128*1024,tpb);

    printf("\n[Batch inflation: 1 stream]\n");
    bench_streams(1,128*1024,tpb);
    bench_streams(1,256*1024,tpb);
    bench_streams(1,512*1024,tpb);

    printf("\n[Combined: 3 streams x 256K]\n");
    bench_streams(3,256*1024,tpb);

    printf("\n[Combined: 3 streams x 512K]\n");
    bench_streams(3,512*1024,tpb);

    printf("\niso=ok/total (ok must = total)\n");
    return 0;
}
