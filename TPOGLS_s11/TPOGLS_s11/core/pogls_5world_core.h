#ifndef POGLS_5WORLD_CORE_H
#define POGLS_5WORLD_CORE_H

#include <stdint.h>
#include "pogls_fibo_addr.h"

#define P5WORLD_XOR_FULL 0xFFFFFFFFFFFFFFFFULL

typedef struct {
    uint32_t addr_icosa;
    uint32_t addr_dodeca;
    uint32_t addr_fibo;
    uint32_t addr_n_icosa;
    uint32_t addr_n_dodeca;

    uint64_t c_icosa;
    uint64_t i_icosa;
    uint64_t c_dodeca;
    uint64_t i_dodeca;
    uint64_t c_fibo;
    uint64_t i_fibo;
    uint64_t c_n_icosa;
    uint64_t i_n_icosa;
    uint64_t c_n_dodeca;
    uint64_t i_n_dodeca;
} P5WorldState;

typedef struct {
    uint8_t self_ok;
    uint8_t neg_self_ok;
    uint8_t dual_ok;
    uint8_t neg_dual_ok;
    uint8_t invert_cross_ok;
    uint8_t aggregate_ok;
    uint8_t all_required_ok;
} P5WorldCheck;

static inline uint64_t p5world_not64(uint64_t x)
{
    return ~x;
}

static inline uint32_t p5world_dual_map(uint32_t addr)
{
    return (uint32_t)(((addr << 10) | (addr >> 10)) & PHI_MASK);
}

static inline uint32_t p5world_invert_addr(uint32_t addr)
{
    return (uint32_t)(~addr) & PHI_MASK;
}

static inline uint8_t p5world_route_bit(uint32_t addr)
{
    return (uint8_t)(addr & 1u);
}

static inline uint32_t p5world_addr_fibo_scaled(uint32_t key, uint8_t level)
{
    uint32_t base = fibo_addr_a(key);
    uint32_t shift = (level > 20u) ? 20u : level;
    return base >> shift;
}

static inline P5WorldState p5world_build(uint32_t key,
                                         uint8_t fibo_level,
                                         uint64_t c_icosa,
                                         uint64_t c_dodeca,
                                         uint64_t c_fibo)
{
    P5WorldState s;

    s.addr_icosa    = fibo_addr_a(key);
    s.addr_dodeca   = p5world_dual_map(s.addr_icosa);
    s.addr_fibo     = p5world_addr_fibo_scaled(key, fibo_level);
    s.addr_n_icosa  = p5world_invert_addr(s.addr_icosa);
    s.addr_n_dodeca = p5world_invert_addr(s.addr_dodeca);

    s.c_icosa = c_icosa;
    s.i_icosa = p5world_not64(c_icosa);

    s.c_dodeca = c_dodeca;
    s.i_dodeca = p5world_not64(c_dodeca);

    s.c_fibo = c_fibo;
    s.i_fibo = p5world_not64(c_fibo);

    s.c_n_icosa = p5world_not64(s.c_icosa);
    s.i_n_icosa = p5world_not64(s.i_icosa);

    s.c_n_dodeca = p5world_not64(s.c_dodeca);
    s.i_n_dodeca = p5world_not64(s.i_dodeca);

    return s;
}

static inline P5WorldCheck p5world_verify(const P5WorldState *s)
{
    P5WorldCheck r;

    r.self_ok =
        ((s->c_icosa ^ s->i_icosa) == P5WORLD_XOR_FULL) &&
        ((s->c_dodeca ^ s->i_dodeca) == P5WORLD_XOR_FULL) &&
        ((s->c_fibo ^ s->i_fibo) == P5WORLD_XOR_FULL);

    r.neg_self_ok =
        ((s->c_n_icosa ^ s->i_n_icosa) == P5WORLD_XOR_FULL) &&
        ((s->c_n_dodeca ^ s->i_n_dodeca) == P5WORLD_XOR_FULL);

    r.dual_ok = (s->addr_dodeca == p5world_dual_map(s->addr_icosa));
    r.neg_dual_ok = (s->addr_n_dodeca == p5world_invert_addr(s->addr_dodeca)) &&
                    (s->addr_n_icosa == p5world_invert_addr(s->addr_icosa));

    r.invert_cross_ok =
        (s->c_n_icosa == p5world_not64(s->c_icosa)) &&
        (s->i_n_icosa == p5world_not64(s->i_icosa)) &&
        (s->c_n_dodeca == p5world_not64(s->c_dodeca)) &&
        (s->i_n_dodeca == p5world_not64(s->i_dodeca));

    r.aggregate_ok = ((s->c_icosa ^ s->c_dodeca ^ s->c_fibo ^
                       s->c_n_icosa ^ s->c_n_dodeca) == 0ULL);

    r.all_required_ok = r.self_ok && r.neg_self_ok && r.dual_ok &&
                        r.neg_dual_ok && r.invert_cross_ok;

    return r;
}

#endif
