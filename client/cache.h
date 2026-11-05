#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the stored reply for a request, or NULL. */
const void *cache_find(const void *request, size_t size, size_t *reply_size);
/* ttl_ms 0 keeps the reply until the cache is full. */
void cache_store(const void *request, size_t size, const void *reply, size_t reply_size, unsigned ttl_ms);

#ifdef __cplusplus
}
#endif
