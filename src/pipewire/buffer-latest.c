/* PipeWire */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <spa/utils/atomic.h>

#include "pipewire/log.h"
#include "buffer-latest-private.h"

PW_LOG_TOPIC_STATIC(log_buffer_latest, "pw.buffer-latest");
#define PW_LOG_TOPIC_DEFAULT log_buffer_latest

#define MAX_LATEST_LINKS SPA_IO_BUFFERS_LATEST_MAX_LINKS

#define BUFFER_DEQUEUED		(1u << 0)
#define BUFFER_PROGRESSIVE	(1u << 1)
#define BUFFER_REUSABLE		(1u << 2)

#define LINK_EMPTY	0u
#define LINK_INSTALLING	1u
#define LINK_ACTIVE	2u
#define LINK_RETIRING	3u
#define LINK_RETIRED	4u
#define LINK_UPDATING	5u

struct latest_buffer {
	struct spa_buffer *storage;
	uint32_t flags;
	uint64_t leases;
	uint64_t submission_sequence;
};

struct latest_link {
	struct spa_io_buffers_latest *io;
	uint32_t state;
	uint32_t readers;
	uint32_t id;
	int notify_fd;
	uint8_t padding[SPA_CACHE_LINE_SIZE - sizeof(void *) -
			3u * sizeof(uint32_t) - sizeof(int)];
};

SPA_STATIC_ASSERT(sizeof(struct latest_link) == SPA_CACHE_LINE_SIZE,
		"latest link state must occupy one cache line");

struct pw_buffer_latest {
	enum spa_direction direction;
	const void *log_object;
	void *data;
	struct latest_link *links;
	uint64_t active_mask;
	uint64_t retired_mask;
	uint32_t enabled;
	uint32_t worker_active;
	uint32_t worker_retiring;
	uint32_t input_claimed;
	uint32_t completion_hint;
	uint32_t scan_hint;
	uint32_t n_buffers;
	uint64_t submission_sequence;
	struct pw_buffer_latest_stats stats;
	struct latest_buffer buffers[PW_BUFFER_LATEST_MAX_BUFFERS];
};

struct latest_link_view {
	struct spa_io_buffers_latest *io;
	int notify_fd;
	uint32_t slot;
};

static void reset_output_buffers(struct pw_buffer_latest *latest, bool active)
{
	uint32_t i;

	latest->scan_hint = 0;
	memset(&latest->stats, 0, sizeof(latest->stats));
	for (i = 0; i < latest->n_buffers; i++) {
		struct latest_buffer *buffer = &latest->buffers[i];

		buffer->flags = active ? BUFFER_REUSABLE : 0;
		buffer->leases = 0;
		buffer->submission_sequence = 0;
	}
}

struct pw_buffer_latest *pw_buffer_latest_new(enum spa_direction direction,
		const void *log_object, void *data)
{
	struct pw_buffer_latest *latest;
	size_t size = sizeof(*latest) + SPA_CACHE_LINE_SIZE - 1u +
			sizeof(struct latest_link) * MAX_LATEST_LINKS;
	uint32_t i;

	if (direction != SPA_DIRECTION_INPUT && direction != SPA_DIRECTION_OUTPUT) {
		errno = EINVAL;
		return NULL;
	}
	if ((latest = calloc(1, size)) == NULL)
		return NULL;
	latest->direction = direction;
	latest->log_object = log_object;
	latest->data = data;
	latest->links = SPA_PTR_ALIGN(SPA_PTROFF(latest, sizeof(*latest), void),
			SPA_CACHE_LINE_SIZE, struct latest_link);
	spa_assert(SPA_IS_ALIGNED(latest->links, SPA_CACHE_LINE_SIZE));
	SPA_ATOMIC_STORE(latest->input_claimed, SPA_ID_INVALID);
	for (i = 0; i < MAX_LATEST_LINKS; i++) {
		latest->links[i].id = SPA_ID_INVALID;
		latest->links[i].notify_fd = -1;
	}
	return latest;
}

void *pw_buffer_latest_get_data(const struct pw_buffer_latest *latest)
{
	return latest == NULL ? NULL : latest->data;
}

void pw_buffer_latest_destroy(struct pw_buffer_latest *latest)
{
	free(latest);
}

bool pw_buffer_latest_is_enabled(const struct pw_buffer_latest *latest)
{
	return latest != NULL && SPA_ATOMIC_LOAD(latest->enabled);
}

void pw_buffer_latest_enable(struct pw_buffer_latest *latest)
{
	if (latest == NULL || SPA_ATOMIC_XCHG(latest->enabled, true))
		return;
	if (latest->direction == SPA_DIRECTION_OUTPUT)
		reset_output_buffers(latest, true);
}

bool pw_buffer_latest_has_links(const struct pw_buffer_latest *latest)
{
	return latest != NULL && SPA_ATOMIC_LOAD(latest->active_mask) != 0;
}

bool pw_buffer_latest_worker_is_active(const struct pw_buffer_latest *latest)
{
	return latest != NULL && SPA_ATOMIC_LOAD(latest->worker_active);
}

uint64_t pw_buffer_latest_active_mask(const struct pw_buffer_latest *latest)
{
	return latest == NULL ? 0 : SPA_ATOMIC_LOAD(latest->active_mask);
}

uint32_t pw_buffer_latest_claimed_buffer(const struct pw_buffer_latest *latest)
{
	return latest == NULL ? SPA_ID_INVALID : SPA_ATOMIC_LOAD(latest->input_claimed);
}

int pw_buffer_latest_begin_worker_retirement(struct pw_buffer_latest *latest)
{
	if (latest == NULL)
		return -EINVAL;
	SPA_ATOMIC_STORE(latest->worker_retiring, true);
	if (SPA_ATOMIC_LOAD(latest->worker_active)) {
		SPA_ATOMIC_STORE(latest->worker_retiring, false);
		return -EBUSY;
	}
	return 0;
}

void pw_buffer_latest_end_worker_retirement(struct pw_buffer_latest *latest)
{
	if (latest != NULL)
		SPA_ATOMIC_STORE(latest->worker_retiring, false);
}

int pw_buffer_latest_worker_begin(struct pw_buffer_latest *latest)
{
	if (latest == NULL)
		return -EINVAL;
	if (SPA_UNLIKELY(SPA_ATOMIC_LOAD(latest->worker_retiring)))
		return -EPIPE;
	if (SPA_UNLIKELY(!SPA_ATOMIC_CAS(latest->worker_active, false, true)))
		return -EBUSY;
	if (SPA_UNLIKELY(SPA_ATOMIC_LOAD(latest->worker_retiring))) {
		SPA_ATOMIC_STORE(latest->worker_active, false);
		return -EPIPE;
	}
	return 0;
}

int pw_buffer_latest_worker_end(struct pw_buffer_latest *latest)
{
	if (latest == NULL)
		return -EINVAL;
	return SPA_ATOMIC_CAS(latest->worker_active, true, false) ? 0 : -EINVAL;
}

void pw_buffer_latest_set_buffers(struct pw_buffer_latest *latest,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	uint32_t i;

	spa_assert(latest != NULL);
	spa_assert(n_buffers <= PW_BUFFER_LATEST_MAX_BUFFERS);
	spa_assert(n_buffers == 0 || buffers != NULL);
	latest->n_buffers = n_buffers;
	for (i = 0; i < n_buffers; i++)
		latest->buffers[i].storage = buffers[i];
	SPA_ATOMIC_STORE(latest->input_claimed, SPA_ID_INVALID);
	reset_output_buffers(latest, latest->direction == SPA_DIRECTION_OUTPUT &&
			SPA_ATOMIC_LOAD(latest->enabled));
}

void pw_buffer_latest_clear_buffers(struct pw_buffer_latest *latest)
{
	if (latest == NULL)
		return;
	latest->n_buffers = 0;
	SPA_ATOMIC_STORE(latest->input_claimed, SPA_ID_INVALID);
	reset_output_buffers(latest, false);
}

bool pw_buffer_latest_same_buffer_pool(const struct pw_buffer_latest *latest,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	uint32_t i, j;

	if (latest == NULL || buffers == NULL || n_buffers != latest->n_buffers)
		return false;
	for (i = 0; i < n_buffers; i++) {
		const struct spa_buffer *current = latest->buffers[i].storage;
		const struct spa_buffer *candidate = buffers[i];

		if (current == candidate)
			continue;
		if (current == NULL || candidate == NULL ||
				current->n_metas != candidate->n_metas ||
				current->n_datas != candidate->n_datas)
			return false;
		for (j = 0; j < current->n_metas; j++)
			if (current->metas[j].type != candidate->metas[j].type ||
					current->metas[j].size != candidate->metas[j].size)
				return false;
		for (j = 0; j < current->n_datas; j++) {
			const struct spa_data *a = &current->datas[j];
			const struct spa_data *b = &candidate->datas[j];
			struct stat a_stat, b_stat;
			bool same_storage;

			if (a->type != b->type || a->flags != b->flags ||
					a->mapoffset != b->mapoffset || a->maxsize != b->maxsize)
				return false;
			same_storage = a->data != NULL && a->data == b->data;
			if (!same_storage && a->fd >= 0 && b->fd >= 0) {
				same_storage = a->fd == b->fd ||
						(fstat(a->fd, &a_stat) == 0 &&
						 fstat(b->fd, &b_stat) == 0 &&
						 a_stat.st_dev == b_stat.st_dev &&
						 a_stat.st_ino == b_stat.st_ino);
			}
			if (!same_storage)
				return false;
		}
	}
	return true;
}

static struct latest_link *find_link(struct pw_buffer_latest *latest, uint32_t id,
		uint32_t *slot)
{
	uint32_t i;

	for (i = 0; i < MAX_LATEST_LINKS; i++) {
		if (SPA_ATOMIC_LOAD(latest->links[i].state) != LINK_EMPTY &&
		    latest->links[i].id == id) {
			if (slot != NULL)
				*slot = i;
			return &latest->links[i];
		}
	}
	return NULL;
}

static bool pin_link(struct pw_buffer_latest *latest, uint32_t slot,
		struct latest_link_view *view)
{
	struct latest_link *link = &latest->links[slot];

	if (SPA_ATOMIC_LOAD(link->state) != LINK_ACTIVE)
		return false;
	SPA_ATOMIC_INC(link->readers);
	if (SPA_ATOMIC_LOAD(link->state) != LINK_ACTIVE) {
		SPA_ATOMIC_DEC(link->readers);
		return false;
	}
	view->io = link->io;
	view->notify_fd = SPA_ATOMIC_LOAD(link->notify_fd);
	view->slot = slot;
	return true;
}

static inline void unpin_link(struct pw_buffer_latest *latest,
		const struct latest_link_view *view)
{
	SPA_ATOMIC_DEC(latest->links[view->slot].readers);
}

static void wait_link_readers(struct pw_buffer_latest *latest,
		struct latest_link *link, const char *operation)
{
	struct timespec start, now;
	uint32_t yields = 0;
	bool check_delay;

	if (SPA_ATOMIC_LOAD(link->readers) == 0)
		return;
	check_delay = clock_gettime(CLOCK_MONOTONIC, &start) == 0;
	while (SPA_ATOMIC_LOAD(link->readers) != 0) {
		sched_yield();
		if (check_delay && ++yields == 1024) {
			yields = 0;
			if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
			    SPA_TIMESPEC_TO_NSEC(&now) - SPA_TIMESPEC_TO_NSEC(&start) >=
					SPA_NSEC_PER_SEC) {
				pw_log_warn("%p: latest link %u %s waiting for %u readers",
						latest->log_object, link->id, operation,
						SPA_ATOMIC_LOAD(link->readers));
				check_delay = false;
			}
		}
	}
}

static void wait_input_release(struct pw_buffer_latest *latest)
{
	struct timespec start, now;
	uint32_t yields = 0;
	bool check_delay;

	if (SPA_ATOMIC_LOAD(latest->input_claimed) == SPA_ID_INVALID)
		return;
	check_delay = clock_gettime(CLOCK_MONOTONIC, &start) == 0;
	while (SPA_ATOMIC_LOAD(latest->input_claimed) != SPA_ID_INVALID) {
		sched_yield();
		if (check_delay && ++yields == 1024) {
			yields = 0;
			if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
			    SPA_TIMESPEC_TO_NSEC(&now) - SPA_TIMESPEC_TO_NSEC(&start) >=
					SPA_NSEC_PER_SEC) {
				pw_log_warn("%p: latest input retirement waiting for buffer %u",
						latest->log_object,
						SPA_ATOMIC_LOAD(latest->input_claimed));
				check_delay = false;
			}
		}
	}
}

static int release_lease(struct pw_buffer_latest *latest, uint32_t slot,
		uint32_t id, bool *became_reusable)
{
	struct latest_buffer *buffer;
	const uint64_t lease = UINT64_C(1) << slot;

	if (SPA_UNLIKELY(id >= latest->n_buffers ||
			(latest->buffers[id].leases & lease) == 0))
		return -EPROTO;
	buffer = &latest->buffers[id];
	buffer->leases &= ~lease;
	if (buffer->leases == 0 && !(buffer->flags & BUFFER_DEQUEUED)) {
		if (SPA_UNLIKELY(buffer->flags & BUFFER_REUSABLE))
			return -EPROTO;
		buffer->flags |= BUFFER_REUSABLE;
		if (became_reusable != NULL)
			*became_reusable = true;
	}
	return 0;
}

int pw_buffer_latest_service_retirements(struct pw_buffer_latest *latest)
{
	uint32_t i, j;
	uint64_t retired;

	if (latest == NULL)
		return -EINVAL;
	retired = SPA_ATOMIC_XCHG(latest->retired_mask, 0);
	if (retired == 0)
		return 0;
	if (latest->direction == SPA_DIRECTION_OUTPUT) {
		for (j = 0; j < latest->n_buffers; j++) {
			struct latest_buffer *buffer = &latest->buffers[j];
			uint64_t released = buffer->leases & retired;

			if (released == 0)
				continue;
			buffer->leases &= ~retired;
			latest->stats.retired_leases += __builtin_popcountll(released);
			if (buffer->leases == 0 && !(buffer->flags & BUFFER_DEQUEUED)) {
				if (SPA_UNLIKELY(buffer->flags & BUFFER_REUSABLE))
					return -EPROTO;
				buffer->flags |= BUFFER_REUSABLE;
			}
		}
	}
	while (retired != 0) {
		i = (uint32_t)__builtin_ctzll(retired);
		retired &= retired - 1;
		SPA_ATOMIC_STORE(latest->links[i].state, LINK_EMPTY);
		latest->stats.subscriber_retirements++;
	}
	return 0;
}

static int update_link(struct pw_buffer_latest *latest,
		const struct spa_io_buffers_latest_link *desc)
{
	struct latest_link *link;
	uint32_t slot;
	uint64_t bit;
	bool active;

	if (desc == NULL || desc->id == SPA_ID_INVALID || desc->reserved != 0 ||
	    (desc->flags & ~SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE) != 0 ||
	    desc->notify_fd < -1)
		return -EINVAL;
	active = SPA_FLAG_IS_SET(desc->flags,
			SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE);
	if (active && desc->io == NULL)
		return -EINVAL;

	link = find_link(latest, desc->id, &slot);
	if (!active) {
		if (link == NULL)
			return 0;
		if (!SPA_ATOMIC_CAS(link->state, LINK_ACTIVE, LINK_RETIRING) &&
		    SPA_ATOMIC_LOAD(link->state) != LINK_RETIRING)
			return -EBUSY;
		bit = UINT64_C(1) << slot;
		__atomic_fetch_and(&latest->active_mask, ~bit, __ATOMIC_SEQ_CST);
		wait_link_readers(latest, link, "retirement");
		if (latest->direction == SPA_DIRECTION_INPUT)
			wait_input_release(latest);
		link->io = NULL;
		SPA_ATOMIC_STORE(link->notify_fd, -1);
		if (latest->direction == SPA_DIRECTION_INPUT) {
			link->id = SPA_ID_INVALID;
			SPA_ATOMIC_STORE(link->state, LINK_EMPTY);
		} else {
			SPA_ATOMIC_STORE(link->state, LINK_RETIRED);
			__atomic_fetch_or(&latest->retired_mask, bit, __ATOMIC_SEQ_CST);
		}
		return 0;
	}

	if (link != NULL) {
		if (SPA_ATOMIC_LOAD(link->state) != LINK_ACTIVE || link->io != desc->io)
			return -EBUSY;
		if (SPA_ATOMIC_LOAD(link->notify_fd) == desc->notify_fd)
			return 0;
		if (!SPA_ATOMIC_CAS(link->state, LINK_ACTIVE, LINK_UPDATING))
			return -EBUSY;
		wait_link_readers(latest, link, "notification update");
		SPA_ATOMIC_STORE(link->notify_fd, desc->notify_fd);
		SPA_ATOMIC_STORE(link->state, LINK_ACTIVE);
		return 0;
	}
	if (latest->direction == SPA_DIRECTION_INPUT &&
			pw_buffer_latest_has_links(latest))
		return -EBUSY;
	for (slot = 0; slot < MAX_LATEST_LINKS; slot++)
		if (SPA_ATOMIC_CAS(latest->links[slot].state,
				LINK_EMPTY, LINK_INSTALLING))
			break;
	if (slot == MAX_LATEST_LINKS)
		return -ENOSPC;
	link = &latest->links[slot];
	pw_buffer_latest_enable(latest);
	link->id = desc->id;
	link->io = desc->io;
	SPA_ATOMIC_STORE(link->notify_fd, desc->notify_fd);
	SPA_ATOMIC_STORE(link->state, LINK_ACTIVE);
	bit = UINT64_C(1) << slot;
	__atomic_fetch_or(&latest->active_mask, bit, __ATOMIC_SEQ_CST);
	return 0;
}

int pw_buffer_latest_set_io(struct pw_buffer_latest *latest, uint32_t id,
		void *data, size_t size)
{
	struct latest_link *old;
	int res;

	if (latest == NULL)
		return -EINVAL;
	switch (id) {
	case SPA_IO_BuffersLatest:
	{
		struct spa_io_buffers_latest_link link;

		old = find_link(latest, 0, NULL);
		link = (struct spa_io_buffers_latest_link) {
			.id = 0,
			.flags = data && size >= sizeof(struct spa_io_buffers_latest)
				? SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE : 0,
			.io = data,
			.notify_fd = old != NULL ? SPA_ATOMIC_LOAD(old->notify_fd) : -1,
		};
		return update_link(latest, &link);
	}
	case SPA_IO_BuffersLatestNotify:
		old = find_link(latest, 0, NULL);
		if (old != NULL && data &&
		    size >= sizeof(struct spa_io_buffers_latest_notify)) {
			const struct spa_io_buffers_latest_notify *notify = data;

			if (notify->reserved != 0 || notify->fd < 0)
				return -EINVAL;
			SPA_ATOMIC_STORE(old->notify_fd, notify->fd);
		} else if (old != NULL) {
			SPA_ATOMIC_STORE(old->notify_fd, -1);
		}
		if (old != NULL)
			wait_link_readers(latest, old, "notification update");
		return 0;
	case SPA_IO_BuffersLatestLink:
		if (data == NULL || size < sizeof(struct spa_io_buffers_latest_link))
			return -EINVAL;
		res = update_link(latest, data);
		return res;
	default:
		return -ENOTSUP;
	}
}

static int drain_completions(struct pw_buffer_latest *latest)
{
	uint32_t returns = 0;
	uint32_t slot = latest->completion_hint < MAX_LATEST_LINKS
		? latest->completion_hint : 0;
	uint64_t remaining = SPA_ATOMIC_LOAD(latest->active_mask);

	while (remaining != 0 && returns < latest->n_buffers) {
		struct latest_link_view link;
		uint64_t candidates = remaining & (~UINT64_C(0) << slot);
		uint32_t id;
		int res;

		if (candidates == 0)
			candidates = remaining;
		slot = (uint32_t)__builtin_ctzll(candidates);
		remaining &= ~(UINT64_C(1) << slot);
		if (pin_link(latest, slot, &link)) {
			res = spa_io_buffers_latest_reclaim_completion(link.io, &id);
			if (res != -EPIPE) {
				if (res < 0) {
					unpin_link(latest, &link);
					return res;
				}
				returns++;
				latest->stats.completions++;
				if ((res = release_lease(latest, slot, id, NULL)) < 0) {
					unpin_link(latest, &link);
					return res;
				}
			}
			unpin_link(latest, &link);
		}
		if (++slot == MAX_LATEST_LINKS)
			slot = 0;
	}
	latest->completion_hint = slot;
	latest->stats.max_completions = SPA_MAX(latest->stats.max_completions,
			returns);
	return 0;
}

static int scan_output(struct pw_buffer_latest *latest, uint32_t *buffer_id)
{
	uint32_t i, id, probes = 0;

	if (latest->n_buffers == 0)
		return -EPIPE;
	id = latest->scan_hint < latest->n_buffers ? latest->scan_hint : 0;
	for (i = 0; i < latest->n_buffers; i++) {
		uint32_t selected = id;
		struct latest_buffer *buffer = &latest->buffers[id];

		probes++;
		latest->stats.buffer_probes++;
		if (buffer->flags & BUFFER_REUSABLE) {
			if (SPA_UNLIKELY((buffer->flags & BUFFER_DEQUEUED) ||
					buffer->leases != 0))
				return -EPROTO;
			buffer->flags &= ~BUFFER_REUSABLE;
			latest->scan_hint = selected + 1 == latest->n_buffers
					? 0 : selected + 1;
			latest->stats.max_buffer_probes = SPA_MAX(
					latest->stats.max_buffer_probes, probes);
			*buffer_id = selected;
			return 0;
		}
		if (++id == latest->n_buffers)
			id = 0;
	}
	latest->stats.max_buffer_probes = SPA_MAX(latest->stats.max_buffer_probes,
			probes);
	return -EPIPE;
}

static int reclaim_submissions(struct pw_buffer_latest *latest, uint32_t *buffer_id)
{
	uint32_t i, withdrawals = 0;
	uint64_t active = SPA_ATOMIC_LOAD(latest->active_mask);

	while (active != 0) {
		struct latest_link_view link;
		struct latest_buffer *buffer;
		uint32_t id;
		uint64_t sequence;
		bool reusable = false;
		int res;

		i = (uint32_t)__builtin_ctzll(active);
		active &= active - 1;
		if (!pin_link(latest, i, &link))
			continue;
		res = spa_io_buffers_latest_withdraw_submission(link.io, &sequence, &id);
		if (res == -EPIPE) {
			unpin_link(latest, &link);
			continue;
		}
		if (res < 0 || id >= latest->n_buffers) {
			unpin_link(latest, &link);
			return -EPROTO;
		}
		withdrawals++;
		latest->stats.submission_withdrawals++;
		buffer = &latest->buffers[id];
		if (SPA_UNLIKELY(sequence != buffer->submission_sequence)) {
			unpin_link(latest, &link);
			return -EPROTO;
		}
		if (buffer->flags & BUFFER_DEQUEUED) {
			if (SPA_UNLIKELY(!(buffer->flags & BUFFER_PROGRESSIVE))) {
				unpin_link(latest, &link);
				return -EPROTO;
			}
			res = spa_io_buffers_latest_submit(link.io, sequence, id, NULL, NULL);
			if (SPA_UNLIKELY(res != 0)) {
				unpin_link(latest, &link);
				return -EPROTO;
			}
			unpin_link(latest, &link);
			continue;
		}
		if ((res = release_lease(latest, i, id, &reusable)) < 0) {
			unpin_link(latest, &link);
			return res;
		}
		unpin_link(latest, &link);
		if (reusable) {
			buffer->flags &= ~BUFFER_REUSABLE;
			latest->scan_hint = id + 1 == latest->n_buffers ? 0 : id + 1;
			latest->stats.submission_reclaims++;
			*buffer_id = id;
			break;
		}
	}
	latest->stats.max_submission_withdrawals = SPA_MAX(
			latest->stats.max_submission_withdrawals, withdrawals);
	return *buffer_id != SPA_ID_INVALID ? 0 : -EPIPE;
}

static int claim_input(struct pw_buffer_latest *latest,
		const struct latest_link_view *link, uint32_t *buffer_id,
		uint64_t *submission_sequence)
{
	uint32_t id;
	uint64_t sequence;
	int res;

	res = spa_io_buffers_latest_receive(link->io, &sequence, &id);
	if (res == -EPIPE)
		return 0;
	if (SPA_UNLIKELY(res < 0 || id >= latest->n_buffers))
		return -EPROTO;
	if (SPA_UNLIKELY(latest->buffers[id].flags & BUFFER_DEQUEUED))
		return -EPROTO;
	latest->buffers[id].flags |= BUFFER_DEQUEUED;
	latest->buffers[id].submission_sequence = sequence;
	SPA_ATOMIC_STORE(latest->input_claimed, id);
	*buffer_id = id;
	if (submission_sequence != NULL)
		*submission_sequence = sequence;
	return 1;
}

int pw_buffer_latest_try_dequeue(struct pw_buffer_latest *latest,
		uint32_t *buffer_id, uint64_t *submission_sequence)
{
	struct latest_link_view link;
	uint64_t active;
	uint32_t slot;
	int res;

	if (SPA_UNLIKELY(latest == NULL || buffer_id == NULL))
		return -EINVAL;
	*buffer_id = SPA_ID_INVALID;
	if (submission_sequence != NULL)
		*submission_sequence = 0;
	if (SPA_UNLIKELY(!SPA_ATOMIC_LOAD(latest->enabled) ||
			latest->direction != SPA_DIRECTION_INPUT))
		return -ENOTSUP;
	if (SPA_UNLIKELY(SPA_ATOMIC_LOAD(latest->input_claimed) != SPA_ID_INVALID))
		return -EBUSY;
	active = SPA_ATOMIC_LOAD(latest->active_mask);
	if (SPA_UNLIKELY(active == 0))
		return 0;
	slot = (uint32_t)__builtin_ctzll(active);
	if (!pin_link(latest, slot, &link))
		return 0;
	res = claim_input(latest, &link, buffer_id, submission_sequence);
	unpin_link(latest, &link);
	return res;
}

int pw_buffer_latest_dequeue(struct pw_buffer_latest *latest, uint32_t *buffer_id,
		uint64_t *submission_sequence)
{
	int res;

	if (SPA_UNLIKELY(latest == NULL || buffer_id == NULL))
		return -EINVAL;
	*buffer_id = SPA_ID_INVALID;
	if (latest->direction == SPA_DIRECTION_INPUT)
		return pw_buffer_latest_try_dequeue(latest, buffer_id,
				submission_sequence);
	res = pw_buffer_latest_try_dequeue_reusable(latest, buffer_id);
	if (res != 0)
		return res;
	if ((res = reclaim_submissions(latest, buffer_id)) < 0) {
		if (res == -EPIPE)
			latest->stats.pool_exhaustions++;
		return res;
	}
	latest->buffers[*buffer_id].flags |= BUFFER_DEQUEUED;
	return 1;
}

int pw_buffer_latest_try_dequeue_reusable(struct pw_buffer_latest *latest,
		uint32_t *buffer_id)
{
	int res;

	if (SPA_UNLIKELY(latest == NULL || buffer_id == NULL))
		return -EINVAL;
	*buffer_id = SPA_ID_INVALID;
	if (SPA_UNLIKELY(latest->direction != SPA_DIRECTION_OUTPUT ||
			!SPA_ATOMIC_LOAD(latest->enabled)))
		return -ENOTSUP;
	if ((res = pw_buffer_latest_service_retirements(latest)) < 0)
		return res;
	latest->stats.dequeue_attempts++;
	if ((res = drain_completions(latest)) < 0)
		return res;
	if ((res = scan_output(latest, buffer_id)) != 0)
		return res == -EPIPE ? 0 : res;
	latest->buffers[*buffer_id].flags |= BUFFER_DEQUEUED;
	return 1;
}

int pw_buffer_latest_try_reclaim_submission(struct pw_buffer_latest *latest,
		uint32_t *buffer_id)
{
	int res;

	if (SPA_UNLIKELY(latest == NULL || buffer_id == NULL))
		return -EINVAL;
	*buffer_id = SPA_ID_INVALID;
	if (SPA_UNLIKELY(latest->direction != SPA_DIRECTION_OUTPUT ||
			!SPA_ATOMIC_LOAD(latest->enabled)))
		return -ENOTSUP;
	if ((res = pw_buffer_latest_service_retirements(latest)) < 0)
		return res;
	if (!pw_buffer_latest_has_links(latest))
		return -EPIPE;
	res = reclaim_submissions(latest, buffer_id);
	if (res == -EPIPE)
		return 0;
	if (res < 0)
		return res;
	latest->buffers[*buffer_id].flags |= BUFFER_DEQUEUED;
	return 1;
}

static inline void signal_notify(struct pw_buffer_latest *latest,
		const struct latest_link_view *link)
{
	uint64_t count = 1;

	if (link->notify_fd >= 0 &&
	    SPA_UNLIKELY(write(link->notify_fd, &count, sizeof(count)) < 0) &&
	    errno != EAGAIN && errno != EINTR) {
		pw_log_trace_fp("%p: latest-buffer notification failed: %m",
				latest->log_object);
	}
}

static uint64_t next_submission_sequence(struct pw_buffer_latest *latest)
{
	uint64_t sequence = latest->submission_sequence + 1u;

	if (SPA_UNLIKELY(sequence == 0 ||
			sequence > SPA_IO_BUFFERS_LATEST_SEQUENCE_MAX))
		sequence = 1;
	latest->submission_sequence = sequence;
	return sequence;
}

static int publish_output(struct pw_buffer_latest *latest, uint32_t buffer_id)
{
	struct latest_buffer *published;
	uint32_t i, visits = 0;
	uint64_t active, sequence;
	int res;

	if (SPA_UNLIKELY(buffer_id >= latest->n_buffers))
		return -EINVAL;
	published = &latest->buffers[buffer_id];
	if (SPA_UNLIKELY(published->leases != 0))
		return -EPROTO;
	if ((res = pw_buffer_latest_service_retirements(latest)) < 0)
		return res;
	sequence = next_submission_sequence(latest);
	published->submission_sequence = sequence;
	active = SPA_ATOMIC_LOAD(latest->active_mask);
	while (active != 0) {
		struct latest_link_view link;
		uint32_t overflow_id;
		uint64_t overflow_sequence;

		i = (uint32_t)__builtin_ctzll(active);
		active &= active - 1;
		if (!pin_link(latest, i, &link))
			continue;
		visits++;
		latest->stats.subscriber_visits++;
		res = spa_io_buffers_latest_submit(link.io, sequence, buffer_id,
				&overflow_sequence, &overflow_id);
		if (SPA_UNLIKELY(res < 0)) {
			unpin_link(latest, &link);
			return res;
		}
		published->leases |= UINT64_C(1) << i;
		latest->stats.subscriber_deliveries++;
		if (res > 0) {
			if (SPA_UNLIKELY(overflow_id == buffer_id ||
					overflow_id >= latest->n_buffers ||
					overflow_sequence != latest->buffers[
						overflow_id].submission_sequence)) {
				unpin_link(latest, &link);
				return -EPROTO;
			}
			latest->stats.submission_overflows++;
			if ((res = release_lease(latest, i, overflow_id, NULL)) < 0) {
				unpin_link(latest, &link);
				return res;
			}
		}
		signal_notify(latest, &link);
		unpin_link(latest, &link);
	}
	if (visits == 0)
		latest->stats.zero_recipient_publications++;
	latest->stats.publications++;
	latest->stats.max_subscriber_visits = SPA_MAX(
			latest->stats.max_subscriber_visits, visits);
	return 0;
}

int pw_buffer_latest_queue(struct pw_buffer_latest *latest, uint32_t buffer_id)
{
	struct latest_link_view link;
	uint64_t active;
	uint32_t slot;
	int res;

	if (SPA_UNLIKELY(latest == NULL || buffer_id >= latest->n_buffers))
		return -EINVAL;
	if (SPA_UNLIKELY(!(latest->buffers[buffer_id].flags & BUFFER_DEQUEUED) ||
			(latest->buffers[buffer_id].flags & BUFFER_PROGRESSIVE)))
		return -EINVAL;
	if (latest->direction == SPA_DIRECTION_OUTPUT) {
		res = publish_output(latest, buffer_id);
	} else if (SPA_UNLIKELY(SPA_ATOMIC_LOAD(latest->input_claimed) != buffer_id)) {
		res = -EPROTO;
	} else if ((active = SPA_ATOMIC_LOAD(latest->active_mask)) == 0) {
		res = 0;
	} else {
		slot = (uint32_t)__builtin_ctzll(active);
		if (!pin_link(latest, slot, &link))
			res = -EPIPE;
		else {
			res = spa_io_buffers_latest_complete(link.io, buffer_id);
			unpin_link(latest, &link);
		}
	}
	if (res < 0)
		return res;
	latest->buffers[buffer_id].flags &= ~BUFFER_DEQUEUED;
	if (latest->direction == SPA_DIRECTION_OUTPUT &&
			latest->buffers[buffer_id].leases == 0)
		latest->buffers[buffer_id].flags |= BUFFER_REUSABLE;
	if (latest->direction == SPA_DIRECTION_INPUT)
		SPA_ATOMIC_STORE(latest->input_claimed, SPA_ID_INVALID);
	return 0;
}

int pw_buffer_latest_return(struct pw_buffer_latest *latest, uint32_t buffer_id)
{
	struct latest_buffer *buffer;

	if (SPA_UNLIKELY(latest == NULL || buffer_id >= latest->n_buffers))
		return -EINVAL;
	buffer = &latest->buffers[buffer_id];
	if (SPA_UNLIKELY(!(buffer->flags & BUFFER_DEQUEUED) ||
			(buffer->flags & BUFFER_PROGRESSIVE)))
		return -EINVAL;
	if (latest->direction == SPA_DIRECTION_INPUT)
		return pw_buffer_latest_queue(latest, buffer_id);
	if (SPA_UNLIKELY(buffer->leases != 0 || (buffer->flags & BUFFER_REUSABLE)))
		return -EPROTO;
	buffer->flags &= ~BUFFER_DEQUEUED;
	buffer->flags |= BUFFER_REUSABLE;
	return 0;
}

int pw_buffer_latest_begin_progressive(struct pw_buffer_latest *latest,
		uint32_t buffer_id)
{
	int res;

	if (SPA_UNLIKELY(latest == NULL || !pw_buffer_latest_has_links(latest)))
		return -ENOTSUP;
	if (SPA_UNLIKELY(latest->direction != SPA_DIRECTION_OUTPUT ||
			buffer_id >= latest->n_buffers))
		return -EINVAL;
	if (SPA_UNLIKELY(!(latest->buffers[buffer_id].flags & BUFFER_DEQUEUED) ||
			(latest->buffers[buffer_id].flags & BUFFER_PROGRESSIVE)))
		return -EINVAL;
	latest->buffers[buffer_id].flags |= BUFFER_PROGRESSIVE;
	res = publish_output(latest, buffer_id);
	if (SPA_UNLIKELY(res < 0)) {
		latest->buffers[buffer_id].flags &= ~BUFFER_PROGRESSIVE;
		return res;
	}
	return 0;
}

int pw_buffer_latest_end_progressive(struct pw_buffer_latest *latest,
		uint32_t buffer_id)
{
	struct latest_buffer *buffer;

	if (SPA_UNLIKELY(latest == NULL || !SPA_ATOMIC_LOAD(latest->enabled)))
		return -ENOTSUP;
	if (SPA_UNLIKELY(latest->direction != SPA_DIRECTION_OUTPUT ||
			buffer_id >= latest->n_buffers))
		return -EINVAL;
	buffer = &latest->buffers[buffer_id];
	if (SPA_UNLIKELY((buffer->flags & (BUFFER_DEQUEUED | BUFFER_PROGRESSIVE)) !=
			(BUFFER_DEQUEUED | BUFFER_PROGRESSIVE)))
		return -EINVAL;
	buffer->flags &= ~(BUFFER_DEQUEUED | BUFFER_PROGRESSIVE);
	if (buffer->leases == 0) {
		if (SPA_UNLIKELY(buffer->flags & BUFFER_REUSABLE))
			return -EPROTO;
		buffer->flags |= BUFFER_REUSABLE;
	}
	return 0;
}

int pw_buffer_latest_get_fd(struct pw_buffer_latest *latest)
{
	struct latest_link_view link;
	uint64_t active;
	uint32_t slot;
	int fd;

	if (SPA_UNLIKELY(latest == NULL ||
			(active = SPA_ATOMIC_LOAD(latest->active_mask)) == 0))
		return -ENOTSUP;
	if (SPA_UNLIKELY(latest->direction == SPA_DIRECTION_OUTPUT &&
			__builtin_popcountll(active) != 1))
		return -ENOTSUP;
	slot = (uint32_t)__builtin_ctzll(active);
	if (!pin_link(latest, slot, &link))
		return -EPIPE;
	fd = link.notify_fd;
	unpin_link(latest, &link);
	return fd >= 0 ? fd : -ENODEV;
}

int pw_buffer_latest_get_stats(struct pw_buffer_latest *latest,
		struct pw_buffer_latest_stats *stats, size_t stats_size)
{
	if (latest == NULL || stats == NULL)
		return -EINVAL;
	if (stats_size < sizeof(*stats))
		return -ENOSPC;
	if (!SPA_ATOMIC_LOAD(latest->enabled) ||
			latest->direction != SPA_DIRECTION_OUTPUT)
		return -ENOTSUP;
	memcpy(stats, &latest->stats, sizeof(*stats));
	return 0;
}

static void clear_poller_pin(struct pw_buffer_latest_poller *poller)
{
	struct latest_link_view link;

	if (poller->io == NULL)
		return;
	if (SPA_LIKELY(poller->slot < MAX_LATEST_LINKS)) {
		link.slot = poller->slot;
		unpin_link(poller->latest, &link);
	}
	poller->io = NULL;
	poller->slot = SPA_ID_INVALID;
}

int pw_buffer_latest_poller_init(struct pw_buffer_latest_poller *poller,
		struct pw_buffer_latest *latest)
{
	if (poller == NULL || latest == NULL)
		return -EINVAL;
	*poller = PW_BUFFER_LATEST_POLLER_INIT;
	if (!SPA_ATOMIC_LOAD(latest->enabled) ||
			latest->direction != SPA_DIRECTION_INPUT)
		return -ENOTSUP;
	if (SPA_ATOMIC_LOAD(latest->input_claimed) != SPA_ID_INVALID)
		return -EBUSY;
	poller->latest = latest;
	return 0;
}

int pw_buffer_latest_poller_try_dequeue(
		struct pw_buffer_latest_poller *poller, uint32_t *buffer_id,
		uint64_t *submission_sequence)
{
	struct latest_link *cached_link;
	struct latest_link_view link;
	uint64_t active;
	uint32_t slot;
	int res;

	if (poller == NULL || buffer_id == NULL || submission_sequence == NULL)
		return -EINVAL;
	*buffer_id = SPA_ID_INVALID;
	*submission_sequence = 0;
	if (poller->latest == NULL || poller->reserved != 0)
		return -EINVAL;
	if (poller->io != NULL) {
		if (SPA_UNLIKELY(poller->slot >= MAX_LATEST_LINKS)) {
			pw_buffer_latest_poller_clear(poller);
			return -EINVAL;
		}
		cached_link = &poller->latest->links[poller->slot];
		if (SPA_UNLIKELY(SPA_ATOMIC_LOAD(cached_link->state) != LINK_ACTIVE ||
				cached_link->io != poller->io)) {
			clear_poller_pin(poller);
			return 0;
		}
		link.io = poller->io;
		link.slot = poller->slot;
	} else {
		active = SPA_ATOMIC_LOAD(poller->latest->active_mask);
		if (SPA_UNLIKELY(active == 0))
			return 0;
		slot = (uint32_t)__builtin_ctzll(active);
		if (!pin_link(poller->latest, slot, &link))
			return 0;
		poller->io = link.io;
		poller->slot = link.slot;
	}
	res = claim_input(poller->latest, &link, buffer_id, submission_sequence);
	if (res != 0) {
		clear_poller_pin(poller);
		poller->latest = NULL;
	}
	return res;
}

void pw_buffer_latest_poller_clear(struct pw_buffer_latest_poller *poller)
{
	if (poller == NULL)
		return;
	if (poller->latest != NULL)
		clear_poller_pin(poller);
	*poller = PW_BUFFER_LATEST_POLLER_INIT;
}
