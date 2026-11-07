/* SPDX-License-Identifier: MIT */
#ifndef PIANO_LOOPBACK_OUTPUT_H
#define PIANO_LOOPBACK_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

struct loopback_mapping {
	void *data;
	size_t bytes;
};

/* The camera main thread owns this queue and the borrowed fd. Native capture
 * DMA is separate; these mappings are the loopback driver's RAM buffers.
 */
struct loopback_output {
	int fd;
	struct loopback_mapping *maps;
	unsigned int count, initial;
	size_t bytes;
	bool allocated, streaming;
};

int loopback_output_setup(struct loopback_output *output, int fd, size_t bytes);
int loopback_output_publish(struct loopback_output *output, const uint8_t *frame,
			    size_t bytes, const struct timeval *timestamp, uint32_t flags);
int loopback_output_clear(struct loopback_output *output);

#endif
