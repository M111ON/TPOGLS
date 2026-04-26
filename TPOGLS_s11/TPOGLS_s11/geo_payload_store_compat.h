#ifndef GEO_PAYLOAD_STORE_COMPAT_H
#define GEO_PAYLOAD_STORE_COMPAT_H
/* Block geo_config.h (guard name = GEO_CONFIG_H) to prevent GEO_SLOTS=576
 * from conflicting with geo_cache.h GEO_SLOTS=144.
 * Provide only the 2 constants geo_payload_store.h actually needs. */
#define GEO_CONFIG_H
#define GEO_TE_CYCLE  144u
#define GEO_SPOKES    6u
#include "geo_payload_store.h"
#undef GEO_CONFIG_H
#endif
