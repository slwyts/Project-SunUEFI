/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#define __dmb(barrier) __atomic_thread_fence(__ATOMIC_SEQ_CST)
