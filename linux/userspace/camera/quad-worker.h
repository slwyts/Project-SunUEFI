/* SPDX-License-Identifier: MIT */
#ifndef PIANO_QUAD_WORKER_H
#define PIANO_QUAD_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "piano-quad.h"

struct quad_worker;

struct quad_worker_result {
	const uint8_t *nv12;
	size_t bytes;
	uint32_t sequence;
	int status; /* Render result: zero or negative errno. */
	struct piano_quad_metrics metrics;
};

/* The camera main thread serializes these calls. The helper never accesses
 * device descriptors. Each stream owns one converter, output and thread.
 */
int quad_worker_create(uint32_t width, uint32_t height, uint32_t cfa,
		       struct quad_worker **worker);
int quad_worker_fd(const struct quad_worker *worker);

/* Submit synchronously copies/unpacks the DMA frame and freezes its controls.
 * It does not start rendering. The caller must successfully QBUF the original
 * DMA buffer before start(), or stop the stream and destroy this helper.
 * A busy helper rejects the frame with -EBUSY; no pending queue is maintained.
 */
int quad_worker_submit(struct quad_worker *worker, const uint8_t *raw,
		       size_t bytes, size_t stride,
		       const struct piano_quad_params *params, uint32_t sequence);
int quad_worker_start(struct quad_worker *worker);

/* Poll fd() for completion, then take(). A successful take returns the render
 * status inside result. NV12 storage remains owned by the helper until release
 * or stop; main may run statistics and publish it before releasing it.
 */
int quad_worker_take(struct quad_worker *worker, struct quad_worker_result *result);
int quad_worker_release(struct quad_worker *worker);

/* First STREAMOFF and close/unmap capture DMA in the camera main thread.
 * Stop joins the helper before freeing any CPU-owned data. An in-progress,
 * non-cancellable render may take several seconds to finish. No detach, sleep
 * or polling is used. On a join error the object is retained for safe cleanup.
 */
int quad_worker_stop(struct quad_worker *worker);

#endif
