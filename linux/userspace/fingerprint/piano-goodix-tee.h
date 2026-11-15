// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2026 slwyts <slwyts@foxmail.com>
 * Object argument layout follows Qualcomm's public Mink Object C ABI.
 * The transport itself uses only the Linux TEE object uAPI.
 */
#ifndef PIANO_GOODIX_TEE_H
#define PIANO_GOODIX_TEE_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t ObjectOp;
typedef uint32_t ObjectCounts;
typedef void *ObjectCxt;
typedef struct Object Object;
typedef union ObjectArg ObjectArg;
typedef int32_t (*ObjectInvoke)(ObjectCxt, ObjectOp, ObjectArg *, ObjectCounts);

struct Object {
	ObjectInvoke invoke;
	ObjectCxt context;
};
union ObjectArg {
	struct { void *ptr; size_t size; } b;
	struct { const void *ptr; size_t size; } bi;
	Object o;
};

#define Object_NULL ((Object){NULL, NULL})
#define Object_OP_release 65535U
#define Object_OP_retain 65534U
#define ObjectOp_methodID(op) ((op) & 65535U)
#define ObjectCounts_pack(bi, bo, oi, oo) ((bi) | (bo) << 4 | (oi) << 8 | (oo) << 12)
#define ObjectCounts_numBI(n) ((n) & 15U)
#define ObjectCounts_numBO(n) (((n) >> 4) & 15U)
#define ObjectCounts_numOI(n) (((n) >> 8) & 15U)
#define ObjectCounts_numOO(n) (((n) >> 12) & 15U)
#define ObjectCounts_total(n) (ObjectCounts_numBI(n) + ObjectCounts_numBO(n) + \
	ObjectCounts_numOI(n) + ObjectCounts_numOO(n))
#define Object_OK 0
#define Object_ERROR 1
#define Object_ERROR_INVALID 2
#define Object_ERROR_MEM 5
#define Object_ERROR_BADOBJ (-92)
#define Object_ERROR_UNAVAIL (-96)
#define Object_ERROR_BUSY (-99)

static inline int32_t Object_invoke(Object object, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	return object.invoke ? object.invoke(object.context, op, args, counts) : Object_ERROR_BADOBJ;
}

static inline int Object_isNull(Object object)
{
	return !object.invoke;
}

static inline int32_t Object_retain(Object object)
{
	return Object_invoke(object, Object_OP_retain, NULL, 0);
}

static inline int32_t Object_release(Object object)
{
	return Object_invoke(object, Object_OP_release, NULL, 0);
}
#define Object_RELEASE_IF(object) do { \
	Object released_object = (object); \
	if (!Object_isNull(released_object)) Object_release(released_object); \
} while (0)

/* Returned objects own a reference. Callback input objects are borrowed for
 * the duration of dispatch; output buffers remain valid through SUPPL_SEND.
 * ROOT must outlive all invocations. Its final release stops callbacks and
 * closes the context; remaining memory handles may then only be released.
 */
int goodix_tee_open(Object *root);
int goodix_tee_client(Object root, Object *environment);
int goodix_tee_memory(Object root, size_t bytes, Object *memory);
int goodix_tee_memory_info(Object memory, void **address, size_t *bytes);

#endif
