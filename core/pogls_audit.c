
#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include "pogls_audit.h"

static uint64_t now_ms(void){struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);return(uint64_t)ts.tv_sec*1000ULL+(uint64_t)(ts.tv_nsec/1000000);}
static void fold8(const uint8_t i[16],uint8_t o[8]){for(int k=0;k<8;k++)o[k]=i[k]^i[k+8];}
static void hash_deep(const POGLS_Deep*b,uint8_t o[16]){memset(o,0,16);for(int i=0;i<PAYLOAD_SIZE;i++)o[i%16]^=b->payload[i];for(int i=0;i<WARP_MAP_SIZE;i++)o[(i+7)%16]^=b->warp_map[i];}
static void xor16(uint8_t a[16],const uint8_t x[16]){for(int i=0;i<16;i++)a[i]^=x[i];}

POGLS_AuditContext* pogls_audit_init(const void*c,uint64_t cs,uint32_t nb,const POGLS_AuditConfig*cfg){
    if(!c||!cfg||cs==0)return NULL;
    POGLS_AuditContext*ctx=calloc(1,sizeof(*ctx));if(!ctx)return NULL;
    memcpy(ctx->magic,AUDIT_MAGIC,4);ctx->version=AUDIT_VERSION;ctx->health=AUDIT_HEALTH_OK;
    ctx->core_mmap=c;ctx->core_size=cs;ctx->n_bits=nb;
    if(pogls_audit_build_tiles(ctx,cfg)!=0){free(ctx);return NULL;}
    return ctx;
}
void pogls_audit_destroy(POGLS_AuditContext*ctx){if(!ctx)return;memset(ctx,0,sizeof(*ctx));free(ctx);}

int pogls_audit_build_tiles(POGLS_AuditContext*ctx,const POGLS_AuditConfig*cfg){
    if(!ctx||!cfg||cfg->tile_count==0||cfg->tile_count>AUDIT_MAX_TILES)return -1;
    uint64_t tot=(1ULL<<ctx->n_bits),tsz=tot/cfg->tile_count;
    double ratio=cfg->overlap_ratio>0.0?cfg->overlap_ratio:AUDIT_OVERLAP_RATIO;
    uint64_t olap=(uint64_t)(tsz*ratio);if(!olap)olap=1;
    for(uint32_t i=0;i<cfg->tile_count;i++){
        POGLS_AuditTile*t=&ctx->tiles[i];memset(t,0,sizeof(*t));
        t->addr_start=i*tsz;t->addr_end=(i+1<cfg->tile_count)?(i+1)*tsz:tot;
        t->overlap_start=t->addr_end-olap;
        t->overlap_end=(t->addr_end+olap<tot)?t->addr_end+olap:tot;
        t->state=(uint8_t)TILE_IDLE;
    }
    ctx->tile_count=cfg->tile_count;return 0;
}

int pogls_audit_scan_tile(POGLS_AuditContext*ctx,uint32_t idx,const POGLS_BranchHeader*branch){
    if(!ctx||idx>=ctx->tile_count||!branch)return -1;
    POGLS_AuditTile*t=&ctx->tiles[idx];
    t->state=TILE_SCANNING;t->branch_id=branch->branch_id;
    t->anomaly_flags=ANOMALY_NONE;t->blocks_scanned=0;t->blocks_anomalous=0;
    memset(t->tile_hash,0,16);memset(t->overlap_hash,0,8);
    uint64_t ts=now_ms(),bstart=t->addr_start<<SHIFT_DEEP,bend=t->addr_end<<SHIFT_DEEP;
    if(bend>ctx->core_size)bend=ctx->core_size;
    if(bstart>=bend){t->state=TILE_CLEAN;return 0;}
    const uint8_t*core=(const uint8_t*)ctx->core_mmap;
    uint8_t bh[16],oa[16];memset(oa,0,16);
    for(uint64_t off=bstart;off+DEEP_BLOCK_SIZE<=bend;off+=DEEP_BLOCK_SIZE){
        if(off<branch->zone_offset_start||off>=branch->zone_offset_end)continue;
        const POGLS_Deep*blk=(const POGLS_Deep*)(core+off);t->blocks_scanned++;
        hash_deep(blk,bh);xor16(t->tile_hash,bh);
        int we=1,pd=0;
        for(int w=0;w<WARP_MAP_SIZE;w++)if(blk->warp_map[w]){we=0;break;}
        for(int p=0;p<PAYLOAD_SIZE;p++)if(blk->payload[p]){pd=1;break;}
        if(we&&pd){t->anomaly_flags|=ANOMALY_WARP_CORRUPT;t->blocks_anomalous++;}
        uint64_t addr=off>>SHIFT_DEEP;
        if(addr>=t->overlap_start&&addr<t->overlap_end)xor16(oa,bh);
    }
    fold8(oa,t->overlap_hash);
    t->state=(uint8_t)(audit_tile_has_anomaly(t)?TILE_ANOMALY:TILE_CLEAN);
    t->scanned_at_ms=now_ms();t->scan_duration_ms=(uint32_t)(t->scanned_at_ms-ts);
    return audit_tile_has_anomaly(t)?1:0;
}

int pogls_audit_check_overlap(POGLS_AuditContext*ctx,uint32_t idx){
    if(!ctx||idx>=ctx->tile_count)return -1;
    POGLS_AuditTile*t=&ctx->tiles[idx];uint8_t*prev=ctx->prev_overlap_hash[idx];
    int az=1;for(int i=0;i<8;i++)if(prev[i]){az=0;break;}
    if(az){memcpy(prev,t->overlap_hash,8);return 0;}
    if(memcmp(t->overlap_hash,prev,8)!=0){
        t->anomaly_flags|=ANOMALY_OVERLAP_DELTA;t->state=(uint8_t)TILE_ANOMALY;
        memcpy(prev,t->overlap_hash,8);return 1;
    }
    memcpy(prev,t->overlap_hash,8);return 0;
}

int pogls_audit_scan_pass(POGLS_AuditContext*ctx,const POGLS_BranchHeader*branch){
    if(!ctx||!branch)return -1;
    uint64_t t0=now_ms();int tot=0;
    for(uint32_t i=0;i<ctx->tile_count;i++){
        if(pogls_audit_scan_tile(ctx,i,branch)>0)tot++;
        if(pogls_audit_check_overlap(ctx,i)>0)tot++;
    }
    ctx->total_scans++;ctx->total_anomalies+=(uint64_t)tot;
    ctx->last_scan_at_ms=now_ms();ctx->last_scan_ms=(uint32_t)(ctx->last_scan_at_ms-t0);
    return tot;
}

int pogls_audit_signal_push(POGLS_AuditContext*ctx,const POGLS_AuditSignal*sig){
    if(!ctx||!sig||audit_queue_full(ctx))return -1;
    ctx->signal_queue[ctx->signal_head]=*sig;
    ctx->signal_head=(ctx->signal_head+1)%AUDIT_MAX_SIGNAL_QUEUE;return 0;
}
int pogls_audit_signal_pop(POGLS_AuditContext*ctx,POGLS_AuditSignal*out){
    if(!ctx||!out||audit_queue_empty(ctx))return -1;
    *out=ctx->signal_queue[ctx->signal_tail];
    ctx->signal_tail=(ctx->signal_tail+1)%AUDIT_MAX_SIGNAL_QUEUE;return 0;
}
static int emit(POGLS_AuditContext*ctx,audit_signal_type_t type,uint64_t bid,uint64_t sid,const uint8_t th[16]){
    POGLS_AuditSignal s;memset(&s,0,sizeof(s));
    s.type=type;s.target_snapshot_id=sid;s.branch_id=bid;
    s.audit_health=ctx->health;s.signal_at_ms=now_ms();
    if(th)memcpy(s.tile_hash,th,16);return pogls_audit_signal_push(ctx,&s);
}
int pogls_audit_emit_certify(POGLS_AuditContext*ctx,uint64_t bid,uint64_t sid,const uint8_t th[16]){return emit(ctx,AUDIT_SIG_CERTIFY,bid,sid,th);}
int pogls_audit_emit_invalidate(POGLS_AuditContext*ctx,uint64_t bid,uint64_t sid){return emit(ctx,AUDIT_SIG_INVALIDATE_AUTO,bid,sid,NULL);}
int pogls_audit_emit_anomaly(POGLS_AuditContext*ctx,uint64_t bid,uint64_t sid,uint8_t flags){(void)flags;return emit(ctx,AUDIT_SIG_ANOMALY,bid,sid,NULL);}
int pogls_audit_emit_health(POGLS_AuditContext*ctx,audit_health_t h){
    pogls_audit_set_health(ctx,h);
    POGLS_AuditSignal s;memset(&s,0,sizeof(s));
    s.type=AUDIT_SIG_HEALTH_UPDATE;s.audit_health=h;s.signal_at_ms=now_ms();
    return pogls_audit_signal_push(ctx,&s);
}
int pogls_audit_log_incident(int fd,const POGLS_AuditIncident*inc){
    if(fd<0||!inc)return -1;
    return(write(fd,inc,sizeof(*inc))==(ssize_t)sizeof(*inc))?0:-1;
}
void pogls_audit_set_health(POGLS_AuditContext*ctx,audit_health_t h){if(ctx)ctx->health=h;}

static const char*tstr(uint8_t s){switch((tile_state_t)s){case TILE_IDLE:return"IDLE    ";case TILE_SCANNING:return"SCANNING";case TILE_CLEAN:return"CLEAN   ";case TILE_ANOMALY:return"ANOMALY!";case TILE_CERTIFIED:return"CERT    ";default:return"UNKNOWN ";}};
static const char*hstr(audit_health_t h){switch(h){case AUDIT_HEALTH_OK:return"OK";case AUDIT_HEALTH_DEGRADED:return"DEGRADED";case AUDIT_HEALTH_OFFLINE:return"OFFLINE";default:return"UNKNOWN";}};

void pogls_audit_print_tile(const POGLS_AuditTile*t,uint32_t idx){
    if(!t)return;
    printf("| Tile[%3u] %s | addr[%8llu-%8llu]",idx,tstr(t->state),(unsigned long long)t->addr_start,(unsigned long long)t->addr_end);
    if(t->anomaly_flags){printf(" 0x%02X",t->anomaly_flags);if(t->anomaly_flags&ANOMALY_HASH_MISMATCH)printf("[HASH]");if(t->anomaly_flags&ANOMALY_OVERLAP_DELTA)printf("[OVERLAP]");if(t->anomaly_flags&ANOMALY_WARP_CORRUPT)printf("[WARP]");if(t->anomaly_flags&ANOMALY_DEEP_UNREADABLE)printf("[UNREAD]");}
    printf(" blk=%u/%u %ums\n",t->blocks_anomalous,t->blocks_scanned,t->scan_duration_ms);
}
void pogls_audit_print_health(const POGLS_AuditContext*ctx){
    if(!ctx)return;
    printf("+-- AUDIT Health ---+\n| Status: %-10s | Tiles: %-4u | n_bits: %-4u |\n| Scans: %-10llu | Anomalies: %-10llu | Last: %ums |\n+-------------------+\n",
           hstr(ctx->health),ctx->tile_count,ctx->n_bits,(unsigned long long)ctx->total_scans,(unsigned long long)ctx->total_anomalies,ctx->last_scan_ms);
}
void pogls_audit_print_summary(const POGLS_AuditContext*ctx){
    if(!ctx)return;pogls_audit_print_health(ctx);
    for(uint32_t i=0;i<ctx->tile_count;i++)if(ctx->tiles[i].state!=(uint8_t)TILE_IDLE)pogls_audit_print_tile(&ctx->tiles[i],i);
}
