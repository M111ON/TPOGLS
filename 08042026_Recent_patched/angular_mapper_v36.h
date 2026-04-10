/*
 * angular_mapper_v36.h — stub header for angular_mapper_v36.c (V3.6)
 * Declarations only — implementation in core/angular_mapper_v36.c
 */
#ifndef ANGULAR_MAPPER_V36_H
#define ANGULAR_MAPPER_V36_H

#include "core/pogls_v3.h"   /* POGLS_AngularAddress */
#include <stdint.h>

/* uint8_t gear/world — matches angular_mapper_v36.c implementation */
POGLS_AngularAddress pogls_node_to_address(uint32_t n_idx, uint8_t gear, uint8_t world, uint32_t n);

#endif /* ANGULAR_MAPPER_V36_H */
