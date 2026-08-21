/* PipeWire */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_BUFFER_LATEST_PRIVATE_H
#define PIPEWIREAO_BUFFER_LATEST_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <spa/utils/defs.h>
#include <spa/buffer/buffer.h>
#include <spa/node/io.h>

#include <pipewire/buffer-latest.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_BUFFER_LATEST_MAX_BUFFERS 64u

struct pw_buffer_latest;

struct pw_buffer_latest_poller {
	struct pw_buffer_latest *latest;
	struct spa_io_buffers_latest *io;
	uint32_t slot;
	uint32_t reserved;
};

#define PW_BUFFER_LATEST_POLLER_INIT \
	((struct pw_buffer_latest_poller) { NULL, NULL, SPA_ID_INVALID, 0 })

struct pw_buffer_latest *pw_buffer_latest_new(enum spa_direction direction,
		const void *log_object, void *data);
void pw_buffer_latest_destroy(struct pw_buffer_latest *latest);
void *pw_buffer_latest_get_data(const struct pw_buffer_latest *latest);

bool pw_buffer_latest_is_enabled(const struct pw_buffer_latest *latest);
bool pw_buffer_latest_has_links(const struct pw_buffer_latest *latest);
bool pw_buffer_latest_worker_is_active(const struct pw_buffer_latest *latest);
uint64_t pw_buffer_latest_active_mask(const struct pw_buffer_latest *latest);
uint32_t pw_buffer_latest_claimed_buffer(const struct pw_buffer_latest *latest);

int pw_buffer_latest_begin_worker_retirement(struct pw_buffer_latest *latest);
void pw_buffer_latest_end_worker_retirement(struct pw_buffer_latest *latest);
int pw_buffer_latest_worker_begin(struct pw_buffer_latest *latest);
int pw_buffer_latest_worker_end(struct pw_buffer_latest *latest);

void pw_buffer_latest_set_buffers(struct pw_buffer_latest *latest,
		struct spa_buffer **buffers, uint32_t n_buffers);
void pw_buffer_latest_clear_buffers(struct pw_buffer_latest *latest);
bool pw_buffer_latest_same_buffer_pool(const struct pw_buffer_latest *latest,
		struct spa_buffer **buffers, uint32_t n_buffers);
int pw_buffer_latest_service_retirements(struct pw_buffer_latest *latest);

int pw_buffer_latest_set_io(struct pw_buffer_latest *latest, uint32_t id,
		void *data, size_t size);

int pw_buffer_latest_dequeue(struct pw_buffer_latest *latest, uint32_t *buffer_id,
		uint64_t *submission_sequence);
int pw_buffer_latest_try_dequeue(struct pw_buffer_latest *latest,
		uint32_t *buffer_id, uint64_t *submission_sequence);
int pw_buffer_latest_queue(struct pw_buffer_latest *latest, uint32_t buffer_id);
int pw_buffer_latest_return(struct pw_buffer_latest *latest, uint32_t buffer_id);

int pw_buffer_latest_begin_progressive(struct pw_buffer_latest *latest,
		uint32_t buffer_id);
int pw_buffer_latest_end_progressive(struct pw_buffer_latest *latest,
		uint32_t buffer_id);

int pw_buffer_latest_get_fd(struct pw_buffer_latest *latest);
int pw_buffer_latest_get_stats(struct pw_buffer_latest *latest,
		struct pw_buffer_latest_stats *stats, size_t stats_size);

int pw_buffer_latest_poller_init(struct pw_buffer_latest_poller *poller,
		struct pw_buffer_latest *latest);
int pw_buffer_latest_poller_try_dequeue(
		struct pw_buffer_latest_poller *poller, uint32_t *buffer_id,
		uint64_t *submission_sequence);
void pw_buffer_latest_poller_clear(struct pw_buffer_latest_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIREAO_BUFFER_LATEST_PRIVATE_H */
