/* SPDX-License-Identifier: MIT */
#include "quad-worker.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

enum worker_state {
	WORKER_IDLE,
	WORKER_PREPARED,
	WORKER_QUEUED,
	WORKER_RENDERING,
	WORKER_READY,
	WORKER_TAKEN,
};

struct quad_worker {
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t wake;
	bool stopping;
	enum worker_state state;
	int completion_fd;

	struct piano_quad *converter;
	uint8_t *nv12;
	size_t bytes;
	uint32_t sequence;
	int status;
	struct piano_quad_metrics metrics;
};

static void *render_thread(void *argument)
{
	struct quad_worker *worker = argument;

	pthread_mutex_lock(&worker->mutex);
	for (;;) {
		while (!worker->stopping && worker->state != WORKER_QUEUED)
			pthread_cond_wait(&worker->wake, &worker->mutex);
		if (worker->stopping)
			break;

		worker->state = WORKER_RENDERING;
		pthread_mutex_unlock(&worker->mutex);

		struct piano_quad_metrics metrics = { 0 };
		int status = piano_quad_render(worker->converter, worker->nv12,
					      worker->bytes, &metrics);

		pthread_mutex_lock(&worker->mutex);
		if (worker->stopping)
			break;
		worker->status = status;
		worker->metrics = metrics;
		worker->state = WORKER_READY;

		/* Only one completion can exist: take/release must happen before
		 * another submission. Keep the fd open until after this join.
		 */
		uint64_t one = 1;
		ssize_t written;

		do {
			written = write(worker->completion_fd, &one, sizeof(one));
		} while (written < 0 && errno == EINTR);
		if (written != sizeof(one))
			worker->status = written < 0 ? -errno : -EIO;
	}
	pthread_mutex_unlock(&worker->mutex);
	return NULL;
}

int quad_worker_create(uint32_t width, uint32_t height, uint32_t cfa,
		       struct quad_worker **output)
{
	struct quad_worker *worker;
	int result;

	if (!output)
		return -EINVAL;
	*output = NULL;
	worker = calloc(1, sizeof(*worker));
	if (!worker)
		return -ENOMEM;

	result = pthread_mutex_init(&worker->mutex, NULL);
	if (result) {
		result = -result;
		goto free_worker;
	}
	result = pthread_cond_init(&worker->wake, NULL);
	if (result) {
		result = -result;
		goto destroy_mutex;
	}
	worker->completion_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (worker->completion_fd < 0) {
		result = -errno;
		goto destroy_condition;
	}

	/* The converter validates the device dimensions before this size is
	 * used. These allocations are CPU memory, not capture/CMA buffers.
	 */
	result = piano_quad_create(width, height, cfa, &worker->converter);
	if (result < 0)
		goto close_event;
	worker->bytes = (size_t)width * height * 3 / 2;
	worker->nv12 = malloc(worker->bytes);
	if (!worker->nv12) {
		result = -ENOMEM;
		goto destroy_converter;
	}

	result = pthread_create(&worker->thread, NULL, render_thread, worker);
	if (result) {
		result = -result;
		goto free_image;
	}
	*output = worker;
	return 0;

free_image:
	free(worker->nv12);
destroy_converter:
	piano_quad_destroy(worker->converter);
close_event:
	close(worker->completion_fd);
destroy_condition:
	pthread_cond_destroy(&worker->wake);
destroy_mutex:
	pthread_mutex_destroy(&worker->mutex);
free_worker:
	free(worker);
	return result;
}

int quad_worker_fd(const struct quad_worker *worker)
{
	return worker->completion_fd;
}

int quad_worker_submit(struct quad_worker *worker, const uint8_t *raw,
		       size_t bytes, size_t stride,
		       const struct piano_quad_params *params, uint32_t sequence)
{
	int result;

	pthread_mutex_lock(&worker->mutex);
	if (worker->state != WORKER_IDLE) {
		pthread_mutex_unlock(&worker->mutex);
		return -EBUSY;
	}

	/* The render thread is asleep until start(), and unpack retains no
	 * raw pointer. Returning from here is the caller's QBUF boundary.
	 */
	result = piano_quad_unpack(worker->converter, raw, bytes, stride, params);
	if (!result) {
		worker->sequence = sequence;
		worker->state = WORKER_PREPARED;
	}
	pthread_mutex_unlock(&worker->mutex);
	return result;
}

int quad_worker_start(struct quad_worker *worker)
{
	pthread_mutex_lock(&worker->mutex);
	if (worker->state != WORKER_PREPARED) {
		pthread_mutex_unlock(&worker->mutex);
		return -EINVAL;
	}
	worker->state = WORKER_QUEUED;
	pthread_cond_signal(&worker->wake);
	pthread_mutex_unlock(&worker->mutex);
	return 0;
}

int quad_worker_take(struct quad_worker *worker, struct quad_worker_result *result)
{
	uint64_t notifications;
	ssize_t received;

	pthread_mutex_lock(&worker->mutex);
	if (worker->state != WORKER_READY) {
		pthread_mutex_unlock(&worker->mutex);
		return -EAGAIN;
	}

	do {
		received = read(worker->completion_fd, &notifications, sizeof(notifications));
	} while (received < 0 && errno == EINTR);
	if (received < 0 && errno != EAGAIN) {
		int error = -errno;

		pthread_mutex_unlock(&worker->mutex);
		return error;
	}
	*result = (struct quad_worker_result) {
		.nv12 = worker->status ? NULL : worker->nv12,
		.bytes = worker->bytes,
		.sequence = worker->sequence,
		.status = worker->status,
		.metrics = worker->metrics,
	};
	worker->state = WORKER_TAKEN;
	pthread_mutex_unlock(&worker->mutex);
	return 0;
}

int quad_worker_release(struct quad_worker *worker)
{
	pthread_mutex_lock(&worker->mutex);
	if (worker->state != WORKER_TAKEN) {
		pthread_mutex_unlock(&worker->mutex);
		return -EINVAL;
	}
	worker->state = WORKER_IDLE;
	pthread_mutex_unlock(&worker->mutex);
	return 0;
}

int quad_worker_stop(struct quad_worker *worker)
{
	int result;

	if (!worker)
		return 0;
	pthread_mutex_lock(&worker->mutex);
	worker->stopping = true;
	pthread_cond_signal(&worker->wake);
	pthread_mutex_unlock(&worker->mutex);

	/* render() is not cancellable. Joining before destroy also covers a
	 * queued operation that has not started, and a prepared unused frame.
	 */
	result = pthread_join(worker->thread, NULL);
	if (result)
		return -result;
	piano_quad_destroy(worker->converter);
	free(worker->nv12);
	close(worker->completion_fd);
	pthread_cond_destroy(&worker->wake);
	pthread_mutex_destroy(&worker->mutex);
	free(worker);
	return 0;
}
