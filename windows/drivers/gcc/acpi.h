/* SPDX-License-Identifier: MIT */
#pragma once
#include <wdf.h>

NTSTATUS Sm8750CheckClockModel(WDFIOTARGET target, PCSTR model, SIZE_T size);
