// SPDX-License-Identifier: BSD-3-Clause
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "piano-goodix.h"

static volatile sig_atomic_t cancelled;
static const char *const fingers[] = {
	"any", "left-thumb", "left-index-finger", "left-middle-finger",
	"left-ring-finger", "left-little-finger", "right-thumb", "right-index-finger",
	"right-middle-finger", "right-ring-finger", "right-little-finger",
};

static void cancel_operation(int signal_number)
{
	(void)signal_number;
	cancelled = 1;
}

static int unsigned_number(const char *text, uint32_t maximum, uint32_t *result)
{
	char *end;

	errno = 0;
	unsigned long value = strtoul(text, &end, 10);
	if (errno || !*text || *text == '-' || *end || value > maximum)
		return -EINVAL;
	*result = value;
	return 0;
}

static int finger_number(const char *text, unsigned int *number)
{
	for (unsigned int i = 0; i < sizeof(fingers) / sizeof(*fingers); ++i)
		if (!strcmp(text, fingers[i])) {
			*number = i;
			return 0;
		}
	uint32_t value;
	if (unsigned_number(text, 10, &value))
		return -EINVAL;
	*number = value;
	return 0;
}

static void progress(enum piano_goodix_progress event, unsigned int remaining, void *context)
{
	(void)context;
	const char *name = event == PIANO_GOODIX_FINGER_DOWN ? "finger-down" :
		event == PIANO_GOODIX_FINGER_UP ? "finger-up" :
		event == PIANO_GOODIX_ENROLL_PROGRESS ? "enroll-progress" :
		event == PIANO_GOODIX_NOT_MATCH ? "not-match" : "retry";

	if (printf("{\"event\":\"%s\",\"remaining\":%u}\n", name, remaining) < 0 ||
	    fflush(stdout) == EOF)
		cancelled = 1;
}

static void usage(void)
{
	puts("Usage: piano-fingerprint-ctl enroll <uid> <finger> [seconds]\n"
	     "       piano-fingerprint-ctl verify <uid> [finger|any] [seconds]\n"
	     "       piano-fingerprint-ctl list <uid>\n"
	     "       piano-fingerprint-ctl delete <uid> <finger>\n"
	     "Fingers use libfprint names, for example right-index-finger, or numbers 1-10.\n"
	     "Linux profiles are separate from Android fingerprint files.\n"
	     "Diagnostics: info, initialize, prepare-auth, watch [seconds],\n"
	     "             storage-probe --state-dir <empty-directory> <group>.");
}

int main(int argc, char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "--help")) {
		usage();
		return 0;
	}
	int enroll = !strcmp(argv[1], "enroll");
	int verify = !strcmp(argv[1], "verify");
	int list = !strcmp(argv[1], "list");
	int remove = !strcmp(argv[1], "delete");
	if (!enroll && !verify && !list && !remove)
		return piano_goodix_diagnostics(argc, argv);
	uint32_t uid, seconds = 0;
	unsigned int finger = 0;
	if (argc < 3 || unsigned_number(argv[2], 0x0fffffffU, &uid) ||
	    (list && argc != 3) || ((enroll || remove) && argc < 4) ||
	    (remove && argc != 4) || ((enroll || verify) && argc > 5) ||
	    (argc >= 4 && (finger_number(argv[3], &finger) || (!verify && !finger))) ||
	    (argc == 5 && (unsigned_number(argv[4], 3600, &seconds) || !seconds))) {
		usage();
		return 2;
	}
	struct sigaction action = { .sa_handler = cancel_operation };
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
		perror("Cannot install cancellation handler");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	struct piano_goodix_options options = {
		.timeout_seconds = seconds, .cancelled = &cancelled, .progress = progress,
	};
	int result;
	if (enroll)
		result = piano_goodix_enroll(uid, finger, &options);
	else if (verify) {
		unsigned int matched = 0;
		result = piano_goodix_verify(uid, finger, &matched, &options);
		if (!result)
			printf("{\"matched\":true,\"finger\":\"%s\"}\n", fingers[matched]);
	} else if (list) {
		unsigned int registered[10];
		size_t count = 0;
		result = piano_goodix_list(uid, registered, &count);
		if (!result) {
			fputs("{\"fingers\":[", stdout);
			for (size_t i = 0; i < count; ++i)
				printf("%s\"%s\"", i ? "," : "", fingers[registered[i]]);
			puts("]}");
		}
	} else
		result = piano_goodix_delete(uid, finger);
	if (result) {
		fprintf(stderr, "Fingerprint %s failed: %s (%d)\n", argv[1], strerror(-result), result);
		return 1;
	}
	if (enroll || remove)
		printf("{\"%s\":true,\"finger\":\"%s\"}\n", enroll ? "enrolled" : "deleted",
		       fingers[finger]);
	return ferror(stdout) ? 1 : 0;
}
