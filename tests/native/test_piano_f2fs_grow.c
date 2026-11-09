// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../tools/android/piano_f2fs_grow.c"
#include <assert.h>

int piano_f2fs_geometry(const char *mapper, const char *userdata, uint64_t expected,
                      unsigned char uuid[16], uint64_t *blocks, uint64_t *section) {
    (void)mapper; (void)userdata; (void)expected; (void)uuid; (void)blocks; (void)section;
    abort();
}

static void check(const char *json, int accepted) {
    struct grow_request request = {0};
    int result = receipt_request(json, strlen(json), UINT64_C(495286484992), &request);
    assert((result == 0) == accepted);
    if (accepted) assert(!strcmp(request.job, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
}

#define RECORD "\"schema_version\":1,\"job_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"gpt_written\":true,\"pending_android_expansion\":true,\"target_userdata_bytes\":495286484992"

int main(void) {
    check("{" RECORD "}", 1);
    check("{" RECORD ",\"created_boot_id\":\"example\",\"android_grow_detected\":false}\n", 1);
    check("{" RECORD ",\"gpt_written\":false}", 0);
    check("{" RECORD ",\"target_userdata_bytes\":1}", 0);
    check("{" RECORD ",\"job_id\":\"another-job\"}", 0);
    check("{\"schema_version\":1,\"job_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"gpt_written\":\"true\",\"pending_android_expansion\":true,\"target_userdata_bytes\":495286484992}", 0);
    check("{\"schema_version\":1,\"job_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"gpt_written\":true,\"pending_android_expansion\":true,\"target_userdata_bytes\":\"495286484992\"}", 0);
    check("{" RECORD "} {\"gpt_written\":false}", 0);
    check("{" RECORD ",\"extra\":{\"gpt_written\":true}}", 0);
    check("{" RECORD, 0);
    check("{}", 0);
    return 0;
}
