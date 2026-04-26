#pragma once
#include <stdint.h>

#define NODE_MAX 1024
#define NODE_MASK_WORDS (NODE_MAX / 64)

typedef struct { uint64_t w[NODE_MASK_WORDS]; } FrontierMask;
typedef struct { FrontierMask neighbors[NODE_MAX]; } NodeState;
typedef struct { int dummy; } EntangleTaskQueue;
typedef struct { uint32_t node_id; } EntangleTask;

void nodemask_or(FrontierMask *dst, const FrontierMask *src);
int entangle_task_pop(EntangleTaskQueue *q, EntangleTask *t);
void node_update(NodeState *ns, uint32_t nid, uint64_t ms, FrontierMask *fr);
