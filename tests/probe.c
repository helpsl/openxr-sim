// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// WHAT AN APPLICATION SEES, through the real loader: the instance is created with the extensions an
// engine typically requires, the runtime names itself, and asking for a headset answers "not
// connected". Exit 0 when all of that holds. Run with XR_RUNTIME_JSON pointing at a manifest, or
// against whatever runtime is active.
#include <openxr/openxr.h>

#include <stdio.h>
#include <string.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

int main(void)
{
    const char *exts[] = { "XR_KHR_vulkan_enable2", "XR_KHR_opengl_enable", XR_EXT_DEBUG_UTILS_EXTENSION_NAME };
    XrInstanceCreateInfo ci = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "openxr-sim probe");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 3;
    ci.enabledExtensionNames = exts;
    XrInstance in = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance(&ci, &in);
    check(r == XR_SUCCESS, "xrCreateInstance with vulkan_enable2, opengl_enable, debug_utils");
    if (r != XR_SUCCESS) { printf("  (xrCreateInstance: %d)\n", r); return 1; }

    XrInstanceProperties p = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    check(xrGetInstanceProperties(in, &p) == XR_SUCCESS && strcmp(p.runtimeName, "openxr-sim") == 0,
          "the runtime is openxr-sim");

    XrSystemGetInfo gi = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(in, &gi, &sys);
    check(r == XR_ERROR_FORM_FACTOR_UNAVAILABLE, "a headset: XR_ERROR_FORM_FACTOR_UNAVAILABLE (not connected)");
    char text[XR_MAX_RESULT_STRING_SIZE];
    check(xrResultToString(in, r, text) == XR_SUCCESS && strcmp(text, "XR_ERROR_FORM_FACTOR_UNAVAILABLE") == 0,
          "xrResultToString names it");

    XrEventDataBuffer ev = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    check(xrPollEvent(in, &ev) == XR_EVENT_UNAVAILABLE, "no events");

    XrPath path = XR_NULL_PATH;
    check(xrStringToPath(in, "/interaction_profiles/khr/simple_controller", &path) == XR_SUCCESS && path != XR_NULL_PATH,
          "paths intern");
    XrActionSetCreateInfo asi = { .type = XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(asi.actionSetName, "gameplay");
    strcpy(asi.localizedActionSetName, "Gameplay");
    XrActionSet set = XR_NULL_HANDLE;
    check(xrCreateActionSet(in, &asi, &set) == XR_SUCCESS, "an action set before any system");

    XrSystemProperties sp = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    check(xrGetSystemProperties(in, sys, &sp) == XR_ERROR_SYSTEM_INVALID, "a system call without a system: SYSTEM_INVALID, no crash");

    check(xrDestroyActionSet(set) == XR_SUCCESS, "the action set is destroyed");
    check(xrDestroyInstance(in) == XR_SUCCESS, "the instance is destroyed");
    printf(failures ? "probe: %d FAILED\n" : "probe: all checks passed\n", failures);
    return failures ? 1 : 0;
}
