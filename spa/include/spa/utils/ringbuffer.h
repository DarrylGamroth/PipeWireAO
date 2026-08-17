/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_RINGBUFFER_H
#define SPA_RINGBUFFER_H

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \defgroup spa_ringbuffer Ringbuffer
 * Ring buffer implementation
 */

/**
 * \addtogroup spa_ringbuffer
 * \{
 */

struct spa_ringbuffer;

#include <spa/utils/defs.h>

#ifndef SPA_API_RINGBUFFER
 #ifdef SPA_API_IMPL
  #define SPA_API_RINGBUFFER SPA_API_IMPL
 #else
  #define SPA_API_RINGBUFFER static inline
 #endif
#endif

/**
 * A ringbuffer type.
 */
struct spa_ringbuffer {
	uint32_t readindex;	/*< the current read index */
	uint32_t writeindex;	/*< the current write index */
};

#define SPA_RINGBUFFER_INIT()	((struct spa_ringbuffer) { 0, 0 })

/** A single-writer index isolated from independently written cache lines. */
struct SPA_ALIGNED(SPA_CACHE_LINE_SIZE) spa_ringbuffer_shared_index {
	uint32_t value;
	uint8_t padding[SPA_CACHE_LINE_SIZE - sizeof(uint32_t)];
};

/**
 * Ringbuffer indices suitable for cross-core shared memory.
 *
 * The consumer alone writes readindex and the producer alone writes
 * writeindex. Each index occupies its own cache line so that independent
 * progress does not introduce false sharing. The compact spa_ringbuffer is
 * retained for ABI compatibility and layouts that do not need this isolation.
 */
struct SPA_ALIGNED(SPA_CACHE_LINE_SIZE) spa_ringbuffer_shared {
	struct spa_ringbuffer_shared_index readindex;
	struct spa_ringbuffer_shared_index writeindex;
};

#define SPA_RINGBUFFER_SHARED_INIT() ((struct spa_ringbuffer_shared) { \
	{ 0, { 0 } }, { 0, { 0 } } \
})

SPA_STATIC_ASSERT(sizeof(struct spa_ringbuffer_shared_index) == SPA_CACHE_LINE_SIZE,
		"shared ringbuffer index must occupy one cache line");
SPA_STATIC_ASSERT(offsetof(struct spa_ringbuffer_shared, readindex) == 0u,
		"shared ringbuffer consumer index must start on a cache line");
SPA_STATIC_ASSERT(offsetof(struct spa_ringbuffer_shared, writeindex) == SPA_CACHE_LINE_SIZE,
		"shared ringbuffer producer index must occupy a separate cache line");

/**
 * Initialize a spa_ringbuffer with \a size.
 *
 * \param rbuf a spa_ringbuffer
 */
SPA_API_RINGBUFFER void spa_ringbuffer_init(struct spa_ringbuffer *rbuf)
{
	*rbuf = SPA_RINGBUFFER_INIT();
}

/**
 * Sets the pointers so that the ringbuffer contains \a size bytes.
 *
 * \param rbuf a spa_ringbuffer
 * \param size the target size of \a rbuf
 */
SPA_API_RINGBUFFER void spa_ringbuffer_set_avail(struct spa_ringbuffer *rbuf, uint32_t size)
{
	rbuf->readindex = 0;
	rbuf->writeindex = size;
}

/** Initialize cache-line-isolated shared ringbuffer indices. */
SPA_API_RINGBUFFER void spa_ringbuffer_shared_init(struct spa_ringbuffer_shared *rbuf)
{
	*rbuf = SPA_RINGBUFFER_SHARED_INIT();
}

/** Configure the cache-line-isolated ringbuffer with size available. */
SPA_API_RINGBUFFER void spa_ringbuffer_shared_set_avail(
		struct spa_ringbuffer_shared *rbuf, uint32_t size)
{
	rbuf->readindex.value = 0;
	rbuf->writeindex.value = size;
}

/**
 * Get the read index and available bytes for reading.
 *
 * \param rbuf a  spa_ringbuffer
 * \param index the value of readindex, should be taken modulo the size of the
 *         ringbuffer memory to get the offset in the ringbuffer memory
 * \return number of available bytes to read. values < 0 mean
 *         there was an underrun. values > rbuf->size means there
 *         was an overrun.
 */
SPA_API_RINGBUFFER int32_t spa_ringbuffer_get_read_index(struct spa_ringbuffer *rbuf, uint32_t *index)
{
	*index = __atomic_load_n(&rbuf->readindex, __ATOMIC_RELAXED);
	return (int32_t) (__atomic_load_n(&rbuf->writeindex, __ATOMIC_ACQUIRE) - *index);
}

/** Get the consumer index and available units from a shared ringbuffer. */
SPA_API_RINGBUFFER int32_t spa_ringbuffer_shared_get_read_index(
		struct spa_ringbuffer_shared *rbuf, uint32_t *index)
{
	*index = __atomic_load_n(&rbuf->readindex.value, __ATOMIC_RELAXED);
	return (int32_t) (__atomic_load_n(&rbuf->writeindex.value,
			__ATOMIC_ACQUIRE) - *index);
}

/**
 * Read \a len bytes from \a rbuf starting \a offset. \a offset must be taken
 * modulo \a size and len should be smaller than \a size.
 *
 * \param rbuf a struct \ref spa_ringbuffer
 * \param buffer memory to read from
 * \param size the size of \a buffer
 * \param offset offset in \a buffer to read from
 * \param data destination memory
 * \param len number of bytes to read
 */
SPA_API_RINGBUFFER void
spa_ringbuffer_read_data(struct spa_ringbuffer *rbuf SPA_UNUSED,
			 const void *buffer, uint32_t size,
			 uint32_t offset, void *data, uint32_t len)
{
	uint32_t l0 = SPA_MIN(len, size - offset), l1 = len - l0;
	spa_memcpy(data, SPA_PTROFF(buffer, offset, void), l0);
	if (SPA_UNLIKELY(l1 > 0))
		spa_memcpy(SPA_PTROFF(data, l0, void), buffer, l1);
}

/**
 * Update the read pointer to \a index.
 *
 * \param rbuf a spa_ringbuffer
 * \param index new index
 */
SPA_API_RINGBUFFER void spa_ringbuffer_read_update(struct spa_ringbuffer *rbuf, int32_t index)
{
	__atomic_store_n(&rbuf->readindex, index, __ATOMIC_RELEASE);
}

/** Publish consumer progress on a shared ringbuffer. */
SPA_API_RINGBUFFER void spa_ringbuffer_shared_read_update(
		struct spa_ringbuffer_shared *rbuf, int32_t index)
{
	__atomic_store_n(&rbuf->readindex.value, index, __ATOMIC_RELEASE);
}

/**
 * Get the write index and the number of bytes inside the ringbuffer.
 *
 * \param rbuf a  spa_ringbuffer
 * \param index the value of writeindex, should be taken modulo the size of the
 *         ringbuffer memory to get the offset in the ringbuffer memory
 * \return the fill level of \a rbuf. values < 0 mean
 *         there was an underrun. values > rbuf->size means there
 *         was an overrun. Subtract from the buffer size to get
 *         the number of bytes available for writing.
 */
SPA_API_RINGBUFFER int32_t spa_ringbuffer_get_write_index(struct spa_ringbuffer *rbuf, uint32_t *index)
{
	*index = __atomic_load_n(&rbuf->writeindex, __ATOMIC_RELAXED);
	return (int32_t) (*index - __atomic_load_n(&rbuf->readindex, __ATOMIC_ACQUIRE));
}

/** Get the producer index and fill level from a shared ringbuffer. */
SPA_API_RINGBUFFER int32_t spa_ringbuffer_shared_get_write_index(
		struct spa_ringbuffer_shared *rbuf, uint32_t *index)
{
	*index = __atomic_load_n(&rbuf->writeindex.value, __ATOMIC_RELAXED);
	return (int32_t) (*index - __atomic_load_n(&rbuf->readindex.value,
			__ATOMIC_ACQUIRE));
}

/**
 * Write \a len bytes to \a buffer starting \a offset. \a offset must be taken
 * modulo \a size and len should be smaller than \a size.
 *
 * \param rbuf a spa_ringbuffer
 * \param buffer memory to write to
 * \param size the size of \a buffer
 * \param offset offset in \a buffer to write to
 * \param data source memory
 * \param len number of bytes to write
 */
SPA_API_RINGBUFFER void
spa_ringbuffer_write_data(struct spa_ringbuffer *rbuf SPA_UNUSED,
			  void *buffer, uint32_t size,
			  uint32_t offset, const void *data, uint32_t len)
{
	uint32_t l0 = SPA_MIN(len, size - offset), l1 = len - l0;
	spa_memcpy(SPA_PTROFF(buffer, offset, void), data, l0);
	if (SPA_UNLIKELY(l1 > 0))
		spa_memcpy(buffer, SPA_PTROFF(data, l0, void), l1);
}

/**
 * Update the write pointer to \a index
 *
 * \param rbuf a spa_ringbuffer
 * \param index new index
 */
SPA_API_RINGBUFFER void spa_ringbuffer_write_update(struct spa_ringbuffer *rbuf, int32_t index)
{
	__atomic_store_n(&rbuf->writeindex, index, __ATOMIC_RELEASE);
}

/** Publish producer progress on a shared ringbuffer. */
SPA_API_RINGBUFFER void spa_ringbuffer_shared_write_update(
		struct spa_ringbuffer_shared *rbuf, int32_t index)
{
	__atomic_store_n(&rbuf->writeindex.value, index, __ATOMIC_RELEASE);
}

/**
 * \}
 */


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_RINGBUFFER_H */
