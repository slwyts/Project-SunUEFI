/* SPDX-License-Identifier: MIT */
#ifndef PIANO_PHOTO_JPEG_H
#define PIANO_PHOTO_JPEG_H

#include "protocol.h"
#include <stdio.h>

int piano_photo_jpeg(int image_fd, const struct piano_photo_result *result, FILE *output);

#endif
