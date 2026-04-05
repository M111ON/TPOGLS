/*
 * pogls_compute_lut.c — ComputeLUT build: Rubik perm/inv + Morton spread + NodeLUT
 */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#include "pogls_compute_lut.h"
#include <string.h>

ComputeLUT g_clut;

/* Morton spread: interleave bits of x into even positions */
static uint32_t morton_spread8(uint8_t x)
{
    uint32_t v = x;
    v = (v | (v << 8)) & 0x00FF00FF;
    v = (v | (v << 4)) & 0x0F0F0F0F;
    v = (v | (v << 2)) & 0x33333333;
    v = (v | (v << 1)) & 0x55555555;
    return v;
}

/* 18-move Rubik cycle helper */
static uint8_t cycle4_perm(uint8_t s, int a, int b, int c, int d)
{
    uint8_t bits_a = (s >> a) & 1;
    uint8_t bits_b = (s >> b) & 1;
    uint8_t bits_c = (s >> c) & 1;
    uint8_t bits_d = (s >> d) & 1;
    s &= ~((1<<a)|(1<<b)|(1<<c)|(1<<d));
    s |= (bits_d << a) | (bits_a << b) | (bits_b << c) | (bits_c << d);
    return s;
}
static uint8_t cycle4_inv(uint8_t s, int a, int b, int c, int d)
{
    return cycle4_perm(s, d, c, b, a);
}

static const int move_bits[6][4] = {
    {0,1,2,3}, {4,5,6,7}, {0,4,5,1}, {2,3,7,6}, {1,5,6,2}, {0,3,7,4}
};

void clut_init(ComputeLUT *c)
{
    /* Rubik perm / inv tables */
    for (int m = 0; m < CLUT_RUBIK_MOVES; m++) {
        int face = m % 6;
        int type = m / 6;   /* 0=CW, 1=CCW, 2=double */
        for (int s = 0; s < CLUT_RUBIK_STATES; s++) {
            uint8_t ps = (uint8_t)s;
            uint8_t is = (uint8_t)s;
            int reps = (type == 2) ? 2 : 1;
            for (int r = 0; r < reps; r++) {
                ps = cycle4_perm(ps, move_bits[face][0], move_bits[face][1],
                                     move_bits[face][2], move_bits[face][3]);
                is = cycle4_inv(is, move_bits[face][0], move_bits[face][1],
                                    move_bits[face][2], move_bits[face][3]);
            }
            if (type == 1) {
                /* CCW = 3× CW */
                ps = (uint8_t)s;
                for (int r = 0; r < 3; r++)
                    ps = cycle4_perm(ps, move_bits[face][0], move_bits[face][1],
                                         move_bits[face][2], move_bits[face][3]);
                is = cycle4_perm((uint8_t)s,
                                  move_bits[face][0], move_bits[face][1],
                                  move_bits[face][2], move_bits[face][3]);
            }
            c->rubik_perm[m][s] = ps;
            c->rubik_inv [m][s] = is;
        }
    }

    /* Morton spread LUT */
    for (int i = 0; i < CLUT_MORTON_SIZE; i++)
        c->morton_lut[i] = morton_spread8((uint8_t)i);

    /* NodeLUT: addr[19:12] → node_id, wrap into NODE_MAX=162 */
    for (int i = 0; i < CLUT_NODE_SIZE; i++)
        c->node_lut[i] = (uint8_t)(i % 162);
}

void clut_node_build(ComputeLUT *c, const uint8_t *node_map, uint32_t size)
{
    for (int i = 0; i < CLUT_NODE_SIZE; i++)
        c->node_lut[i] = (node_map && i < (int)size)
                         ? node_map[i] : (uint8_t)(i % 162);
}

int clut_selftest(const ComputeLUT *c)
{
    /* 4× CW on same face = identity */
    for (int m = 0; m < 6; m++) {
        for (int s = 0; s < CLUT_RUBIK_STATES; s++) {
            uint8_t st = (uint8_t)s;
            for (int r = 0; r < 4; r++)
                st = c->rubik_perm[m][st];
            if (st != (uint8_t)s) return -1;
        }
    }
    /* morton(0,0)=0, morton(1,0)=1, morton(0,1)=2 */
    if (c->morton_lut[0] != 0) return -2;
    if (c->morton_lut[1] != 1) return -3;
    /* node_lut wraps at 162 */
    if (c->node_lut[162] != 0) return -4;
    return 0;
}
