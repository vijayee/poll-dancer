/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Library initialization and version information.
 */

#include "poll-dancer/version.h"
#include "internal/platform.h"

#include <stddef.h>

/**
 * Library version components.
 */
#define VERSION_STRING "0.1.0"

const char *pd_version_string(void) {
    return VERSION_STRING;
}

void pd_version_components(int *major, int *minor, int *patch) {
    if (major) {
        *major = POLL_DANCER_VERSION_MAJOR;
    }
    if (minor) {
        *minor = POLL_DANCER_VERSION_MINOR;
    }
    if (patch) {
        *patch = POLL_DANCER_VERSION_PATCH;
    }
}

int pd_is_thread_safe(void) {
#ifdef PD_THREAD_SAFE
    return 1;
#else
    return 0;
#endif
}

const char *pd_platform_name(void) {
    const pd_platform_ops_t *ops = pd_platform_detect();
    return ops ? ops->name : "unknown";
}