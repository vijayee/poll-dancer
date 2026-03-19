/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * This library provides a unified API for event notification mechanisms
 * across different operating systems (epoll, kqueue, IOCP).
 */

#ifndef POLL_DANCER_VERSION_H
#define POLL_DANCER_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Library version information.
 */
#define POLL_DANCER_VERSION_MAJOR 0
#define POLL_DANCER_VERSION_MINOR 1
#define POLL_DANCER_VERSION_PATCH 0

/**
 * Get the library version as a string.
 *
 * @return Version string in format "MAJOR.MINOR.PATCH"
 */
const char *pd_version_string(void);

/**
 * Get the library version as components.
 *
 * @param major Pointer to store major version (can be NULL)
 * @param minor Pointer to store minor version (can be NULL)
 * @param patch Pointer to patch version (can be NULL)
 */
void pd_version_components(int *major, int *minor, int *patch);

/**
 * Check if the library was compiled with thread safety support.
 *
 * @return 1 if thread-safe, 0 otherwise
 */
int pd_is_thread_safe(void);

/**
 * Get the platform backend name.
 *
 * @return String identifying the backend (e.g., "epoll", "kqueue", "iocp")
 */
const char *pd_platform_name(void);

#ifdef __cplusplus
}
#endif

#endif /* POLL_DANCER_VERSION_H */