// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// A HEADSET THAT IS NOT PLUGGED IN. The answer OpenXR has for exactly this is
// XR_ERROR_FORM_FACTOR_UNAVAILABLE: the form factor is supported, nothing is connected right now.
// Applications are written for it -- they show "connect your headset" and try xrGetSystem again --
// whereas a missing or broken runtime (an empty library_path) is XR_ERROR_RUNTIME_UNAVAILABLE or
// XR_ERROR_RUNTIME_FAILURE at xrCreateInstance, which most report as a crash or a fatal error.
//
// Handheld (a phone as AR) is UNSUPPORTED rather than unavailable: no device here will ever be one.
#include "device.h"

#include <openxr/openxr.h>

static int null_get_system(int form_factor, uint64_t *id)
{
    (void)id;
    return form_factor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY ? XR_ERROR_FORM_FACTOR_UNAVAILABLE
                                                              : XR_ERROR_FORM_FACTOR_UNSUPPORTED;
}

const struct oxs_device oxs_device_null = {
    .name = "null",
    .system_name = "openxr-sim (no headset)",
    .get_system = null_get_system,
};
