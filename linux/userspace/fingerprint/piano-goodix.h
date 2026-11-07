// SPDX-License-Identifier: BSD-3-Clause
#ifndef PIANO_GOODIX_H
#define PIANO_GOODIX_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

enum piano_goodix_progress {
	PIANO_GOODIX_FINGER_DOWN,
	PIANO_GOODIX_FINGER_UP,
	PIANO_GOODIX_ENROLL_PROGRESS,
	PIANO_GOODIX_RETRY,     /* Bad acquisition; enrollment can continue. */
	PIANO_GOODIX_NOT_MATCH, /* A negative verification, followed by cleanup. */
};

struct piano_goodix_options {
	/* Zero selects 120 seconds for enrollment and 30 for verification. */
	unsigned int timeout_seconds;
	volatile sig_atomic_t *cancelled;
	/* Threaded callers supply a synchronized cancellation query here. */
	int (*should_cancel)(void *context);
	void (*progress)(enum piano_goodix_progress event, unsigned int remaining,
			 void *context);
	void *context;
};

/* Fingers use libfprint's numbers: left thumb=1 through right little=10.
 * Verification also accepts finger=0 to match any enrolled Linux finger.
 * These blocking APIs return zero on completion or a negative errno. They
 * never print a template, image, credential, or authentication token.
 */
int piano_goodix_enroll(uint32_t linux_uid, unsigned int finger,
			 const struct piano_goodix_options *options);
int piano_goodix_verify(uint32_t linux_uid, unsigned int finger,
			 unsigned int *matched_finger,
			 const struct piano_goodix_options *options);
int piano_goodix_list(uint32_t linux_uid, unsigned int fingers[10], size_t *count);
int piano_goodix_delete(uint32_t linux_uid, unsigned int finger);

/* Existing native diagnostic commands; the CLI also uses these unchanged. */
int piano_goodix_diagnostics(int argc, char **argv);

#endif
