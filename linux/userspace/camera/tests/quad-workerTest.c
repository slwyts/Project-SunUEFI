/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../quad-worker.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Block only the scheduling boundary; every invocation still calls the real
 * f70 Rust converter with synthetic RAW data, including the held render.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool hold_render, render_entered, release_render, stop_joined, stop_done;
static unsigned int render_calls;
static _Thread_local bool watch_join;

int __real_piano_quad_render(struct piano_quad *, uint8_t *, size_t,
			    struct piano_quad_metrics *);
int __real_pthread_join(pthread_t, void **);

int __wrap_piano_quad_render(struct piano_quad *converter, uint8_t *output,
			    size_t bytes, struct piano_quad_metrics *metrics)
{
	pthread_mutex_lock(&lock);
	render_calls++;
	if (hold_render) {
		render_entered = true;
		pthread_cond_broadcast(&changed);
		while (!release_render)
			pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);
	return __real_piano_quad_render(converter, output, bytes, metrics);
}

int __wrap_pthread_join(pthread_t thread, void **value)
{
	if (watch_join) {
		pthread_mutex_lock(&lock);
		stop_joined = true;
		pthread_cond_broadcast(&changed);
		pthread_mutex_unlock(&lock);
	}
	return __real_pthread_join(thread, value);
}

static void wait_completion(struct quad_worker *worker)
{
	struct pollfd fd = { .fd = quad_worker_fd(worker), .events = POLLIN };

	assert(poll(&fd, 1, -1) == 1);
	assert(fd.revents == POLLIN);
}

static void expect_neutral(const struct quad_worker_result *result, uint8_t y)
{
	assert(result->status == 0 && result->nv12 && result->bytes == 8 * 8 * 3 / 2);
	for (size_t i = 0; i < 64; i++)
		assert(result->nv12[i] == y);
	for (size_t i = 64; i < result->bytes; i++)
		assert(result->nv12[i] == 128);
}

static void *stop_thread(void *argument)
{
	watch_join = true;
	assert(quad_worker_stop(argument) == 0);
	pthread_mutex_lock(&lock);
	stop_done = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	return NULL;
}

int main(void)
{
	struct quad_worker *worker = NULL;
	struct piano_quad_params params;
	struct quad_worker_result result;
	uint8_t raw[8 * 16];
	struct pollfd fd;
	pthread_t stopper;

	assert(quad_worker_create(7, 8, PIANO_QUAD_RGGB, &worker) == -EINVAL);
	assert(!worker);
	assert(quad_worker_stop(NULL) == 0);
	assert(quad_worker_create(8, 8, PIANO_QUAD_GRBG, &worker) == 0);
	fd = (struct pollfd){ .fd = quad_worker_fd(worker), .events = POLLIN };
	assert(fcntl(fd.fd, F_GETFD) & FD_CLOEXEC);
	assert(fcntl(fd.fd, F_GETFL) & O_NONBLOCK);
	assert(quad_worker_start(worker) == -EINVAL);
	assert(quad_worker_take(worker, &result) == -EAGAIN);

	piano_quad_params_default(&params);
	memset(raw, 255, sizeof(raw));
	assert(quad_worker_submit(worker, raw, 3, 16, &params, 1) == -EINVAL);
	assert(quad_worker_submit(worker, raw, sizeof(raw), 16, &params, 42) == 0);
	memset(raw, 0, sizeof(raw));
	params.digital_gain = 0;
	assert(quad_worker_submit(worker, raw, sizeof(raw), 16, &params, 99) == -EBUSY);
	assert(poll(&fd, 1, 0) == 0);
	assert(quad_worker_take(worker, &result) == -EAGAIN);
	assert(render_calls == 0); /* No render before the caller's QBUF/start. */

	assert(quad_worker_start(worker) == 0);
	assert(quad_worker_start(worker) == -EINVAL);
	assert(quad_worker_submit(worker, raw, sizeof(raw), 16, &params, 99) == -EBUSY);
	wait_completion(worker);
	assert(quad_worker_submit(worker, raw, sizeof(raw), 16, &params, 99) == -EBUSY);
	assert(quad_worker_take(worker, &result) == 0);
	assert(result.sequence == 42);
	expect_neutral(&result, 255); /* DMA and control inputs were copied. */
	assert(quad_worker_take(worker, &result) == -EAGAIN);
	assert(quad_worker_submit(worker, raw, sizeof(raw), 16, &params, 99) == -EBUSY);
	assert(quad_worker_release(worker) == 0);
	assert(quad_worker_release(worker) == -EINVAL);
	assert(poll(&fd, 1, 0) == 0);

	piano_quad_params_default(&params);
	assert(quad_worker_submit(worker, raw, sizeof(raw), 16, &params, 43) == 0);
	assert(quad_worker_start(worker) == 0);
	wait_completion(worker);
	assert(quad_worker_take(worker, &result) == 0 && result.sequence == 43);
	expect_neutral(&result, 0);
	assert(quad_worker_release(worker) == 0);
	assert(quad_worker_stop(worker) == 0);
	assert(fcntl(fd.fd, F_GETFD) == -1 && errno == EBADF);

	/* Stop a prepared frame without ever scheduling its render. */
	unsigned int before = render_calls;
	assert(quad_worker_create(8, 8, PIANO_QUAD_RGGB, &worker) == 0);
	assert(quad_worker_submit(worker, raw, sizeof(raw), 16, &params, 44) == 0);
	assert(quad_worker_stop(worker) == 0);
	assert(render_calls == before);

	/* Hold the real render at its entry, then prove stop reaches join and
	 * cannot finish before the renderer is released. No timed sleeps.
	 */
	hold_render = true;
	assert(quad_worker_create(8, 8, PIANO_QUAD_BGGR, &worker) == 0);
	assert(quad_worker_submit(worker, raw, sizeof(raw), 16, &params, 45) == 0);
	assert(quad_worker_start(worker) == 0);
	pthread_mutex_lock(&lock);
	while (!render_entered)
		pthread_cond_wait(&changed, &lock);
	pthread_mutex_unlock(&lock);
	assert(pthread_create(&stopper, NULL, stop_thread, worker) == 0);
	pthread_mutex_lock(&lock);
	while (!stop_joined)
		pthread_cond_wait(&changed, &lock);
	assert(!stop_done);
	release_render = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	assert(pthread_join(stopper, NULL) == 0);
	assert(stop_done);
	puts("quad worker: real converter copy/freeze, queue states, eventfd and join checks passed");
	return 0;
}
