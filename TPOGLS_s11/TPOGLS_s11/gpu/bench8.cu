/*
 * bench8.cu — POGLS38  GPU FULL PIPELINE
 *
 * Architecture: CPU = control plane only
 *   - GPU generates addr (no H2D)
 *   - Persistent kernel reads work queue (no relaunch)
 *   - Compact result: 8B per entry (hilbert:4 + lane:1 + audit:1 + pad:2)
 *   - CPU reads results async (no sync per batch)
 *
 * Result struct (was 24B+ → now 8B):
 *   struct Entry { uint32_t hilbert; uint8_t lane; uint8_t audit; uint16_t pad; }
 *   bandwidth: -67% vs naive
 *
 * Work queue protocol:
 *   host writes: queue[slot] = {base, stride, N, rep_id}
 *   GPU reads:   atomic_add on head → claim slot → process → mark done
 *   host checks: result[rep_id].done == 1
 */
#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime.h>

#define PHI_UP   1696631u
#define PHI_MASK ((1u<<20)-1u)
#define MAX_QUEUE 64

__device__ __constant__ uint8_t d_lut[16] = {0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};

__device__ __forceinline__ uint8_t fast_iso(uint64_t a) {
    uint32_t lo = (uint32_t)(a & 0xFFFFFFFFu);
    uint32_t q  = (uint32_t)(((uint64_t)lo * 4042322161ULL) >> 36);
    return (lo - q * 17u == 1u) ? 0u : 1u;
}

/* ── compact 8B result ── */
struct __align__(8) Entry {
    uint32_t hilbert;
    uint8_t  lane;
    uint8_t  audit;
    uint16_t pad;
};

/* ── work item ── */
struct WorkItem {
    uint64_t base;
    uint64_t sv;        /* stride value */
    uint32_t N;
    uint32_t rep_id;
    volatile int done;
    int      slot_broadcast;  /* used by thread 0 to share slot with block */
    int      _pad[2];
};

/* ══════════════════════════════════════════════════════
 * PERSISTENT KERNEL + WORK QUEUE
 * One launch, runs until stop signal
 * ══════════════════════════════════════════════════════ */
__global__ __launch_bounds__(256, 2)
void kernel_persistent_queue(
    Entry          * __restrict__ out,
    WorkItem       * __restrict__ queue,
    volatile int   * __restrict__ stop_flag,
    volatile int   * __restrict__ queue_head,  /* atomic counter */
    int              total_work)
{
    uint32_t gstride = gridDim.x * blockDim.x;
    uint32_t tid_g   = blockIdx.x * blockDim.x + threadIdx.x;

    while (!(*stop_flag)) {
        /* claim next work item */
        int slot = -1;
        if (threadIdx.x == 0) {
            int h = atomicAdd((int*)queue_head, 1);
            slot = (h < total_work) ? h : -1;
            queue[blockIdx.x % MAX_QUEUE].slot_broadcast = slot;
        }
        __syncthreads();
        slot = queue[blockIdx.x % MAX_QUEUE].slot_broadcast;
        if (slot < 0) break;

        WorkItem *wi = &queue[slot];
        uint64_t base = wi->base;
        uint64_t sv   = wi->sv;
        uint32_t N    = wi->N;

        /* process this work item */
        for (uint32_t i = tid_g; i < N; i += gstride) {
            uint64_t a  = base + sv * (uint64_t)i;
            uint32_t t  = (uint32_t)(((a & PHI_MASK)*(uint64_t)PHI_UP)>>20);
            uint16_t x  = t & 0x3FFu, y = (t>>10) & 0x3FFu;
            uint32_t rx = x, ry = y;
            rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
            rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
            ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
            ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
            uint32_t mor = rx | (ry << 1);
            uint32_t hil = ((mor>>4)<<4) | d_lut[mor & 0xFu];
            out[i].hilbert = hil;
            out[i].lane    = (uint8_t)(hil % 54u);
            out[i].audit   = fast_iso(a);
            out[i].pad     = 0;
        }
        __threadfence();
        if (threadIdx.x == 0)
            wi->done = 1;
    }
}

/* simple generate kernel for comparison */
__global__ __launch_bounds__(256, 2)
void kernel_gen_entry(
    Entry    * __restrict__ out,
    uint64_t base, uint64_t sv,
    uint32_t N, uint32_t reps)
{
    uint32_t gstride = gridDim.x * blockDim.x;
    uint32_t start   = blockIdx.x * blockDim.x + threadIdx.x;
    for (uint32_t rep = 0; rep < reps; rep++)
        for (uint32_t i = start; i < N; i += gstride) {
            uint64_t a  = base + sv * (uint64_t)i;
            uint32_t t  = (uint32_t)(((a & PHI_MASK)*(uint64_t)PHI_UP)>>20);
            uint16_t x  = t & 0x3FFu, y = (t>>10) & 0x3FFu;
            uint32_t rx = x, ry = y;
            rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
            rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
            ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
            ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
            uint32_t mor = rx | (ry << 1);
            uint32_t hil = ((mor>>4)<<4) | d_lut[mor & 0xFu];
            out[i].hilbert = hil;
            out[i].lane    = (uint8_t)(hil % 54u);
            out[i].audit   = fast_iso(a);
        }
}

static void iso_check(uint64_t base, uint64_t sv,
                      const Entry *d_out, uint32_t N, const char *tag) {
    Entry *h = (Entry*)malloc(N*sizeof(Entry));
    cudaMemcpy(h, d_out, N*sizeof(Entry), cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for(uint32_t i=0;i<N;i++){
        uint64_t a = base + sv*i;
        uint8_t ex = ((a%17u)==1u)?0u:1u;
        if(h[i].audit==ex) ok++; else fail++;
    }
    printf("    iso: %llu/%llu%s\n",(unsigned long long)ok,
           (unsigned long long)(ok+fail), fail?"  ❌":"  ✓");
    free(h);
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    int n_sm = p.multiProcessorCount;
    printf("GPU: %s SM%d.%d  SMs=%d\n\n",p.name,p.major,p.minor,n_sm);

    const uint32_t N   = 2*1024*1024;
    const uint32_t TPB = 256;
    const uint64_t BASE=1, SV=34;
    int dev; cudaGetDevice(&dev);

    Entry *d_out; cudaMalloc(&d_out, N*sizeof(Entry));
    printf("Entry size: %zu B (was 24B+ → compact 8B, -67%% bandwidth)\n\n",
           sizeof(Entry));

    /* ── A. Compact struct baseline ── */
    printf("=== A. Compact 8B struct (generate-in-place) ===\n");
    {
        int waves[]={1,2,4};
        for(int w=0;w<3;w++){
            uint32_t blk = (uint32_t)n_sm * waves[w];
            kernel_gen_entry<<<blk,TPB>>>(d_out,BASE,SV,N,2);
            cudaDeviceSynchronize();
            cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
            cudaEventRecord(t0);
            kernel_gen_entry<<<blk,TPB>>>(d_out,BASE,SV,N,20);
            cudaEventRecord(t1); cudaEventSynchronize(t1);
            float ms; cudaEventElapsedTime(&ms,t0,t1);
            double mops = (double)N*20/(ms/1000.0)/1e6;
            printf("  CompactGen SM×%d (%3u blk): %7.0f M/s  ms=%.2f\n",
                   waves[w],(uint32_t)n_sm*waves[w],mops,ms);
            if(w==1) iso_check(BASE,SV,d_out,N,"compact");
            cudaEventDestroy(t0); cudaEventDestroy(t1);
        }
    }

    /* ── B. Persistent kernel + work queue ── */
    printf("\n=== B. Persistent kernel + work queue ===\n");
    {
        const int NJOBS = 20;
        WorkItem *wq; cudaMallocManaged(&wq, MAX_QUEUE*sizeof(WorkItem));
        volatile int *stop_f; cudaMallocManaged((void**)&stop_f, sizeof(int));
        volatile int *q_head; cudaMallocManaged((void**)&q_head, sizeof(int));
        *stop_f = 0; *q_head = 0;
        for(int j=0;j<NJOBS;j++){
            wq[j].base=BASE; wq[j].sv=SV; wq[j].N=N;
            wq[j].rep_id=j; wq[j].done=0;
        }
        cudaMemPrefetchAsync(wq, MAX_QUEUE*sizeof(WorkItem), dev);
        cudaDeviceSynchronize();

        uint32_t blk = (uint32_t)n_sm * 2;
        /* warmup */
        *q_head = 0;
        for(int j=0;j<2;j++) wq[j].done=0;
        kernel_persistent_queue<<<blk,TPB>>>(d_out,wq,(int*)stop_f,(int*)q_head,2);
        cudaDeviceSynchronize();

        /* reset */
        *q_head = 0; *stop_f = 0;
        for(int j=0;j<NJOBS;j++) wq[j].done=0;
        cudaDeviceSynchronize();

        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        kernel_persistent_queue<<<blk,TPB>>>(d_out,wq,(int*)stop_f,(int*)q_head,NJOBS);
        *stop_f = 1;
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        double mops = (double)N*NJOBS/(ms/1000.0)/1e6;
        printf("  PersistentQueue SM×2 (%d jobs): %7.0f M/s  ms=%.2f\n",
               NJOBS, mops, ms);
        iso_check(BASE,SV,d_out,N,"persistent-q");
        cudaEventDestroy(t0); cudaEventDestroy(t1);
        cudaFree(wq); cudaFree((void*)stop_f); cudaFree((void*)q_head);
    }

    /* ── C. Sustained 200 reps (thermal ceiling) ── */
    printf("\n=== C. Sustained 200 reps — thermal ceiling ===\n");
    {
        uint32_t blk = (uint32_t)n_sm * 2;
        kernel_gen_entry<<<blk,TPB>>>(d_out,BASE,SV,N,2);
        cudaDeviceSynchronize();
        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        kernel_gen_entry<<<blk,TPB>>>(d_out,BASE,SV,N,200);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        double mops = (double)N*200/(ms/1000.0)/1e6;
        printf("  CompactGen SM×2  200 reps:     %7.0f M/s  ms=%.2f  total=%.1fB\n",
               mops, ms, (double)N*200/1e9);
        iso_check(BASE,SV,d_out,N,"thermal-200");
    }

    printf("\n=== Journey complete ===\n");
    printf("  bench2  (start)           200 M/s\n");
    printf("  bench3  (SoA)            9900 M/s\n");
    printf("  bench4  (managed+pre)   17748 M/s\n");
    printf("  bench5  (warp spec)     21800 M/s\n");
    printf("  bench6  (gen-in-place)  28501 M/s  peak\n");
    printf("  bench7  (launch_bounds)   ??? M/s\n");
    printf("  bench8  (compact+queue)   ??? M/s\n");
    printf("\nPRODUCTION DECISION:\n");
    printf("  CPU = control only (write work queue)\n");
    printf("  GPU = generate + process + store (full lifecycle)\n");
    printf("  PCIe = results only (8B/entry, async)\n");

    cudaFree(d_out);
    return 0;
}
