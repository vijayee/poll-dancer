/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * This library provides a unified API for event notification mechanisms
 * across different operating systems (epoll, kqueue, IOCP).
 */

#ifndef POLL_DANCER_ERRORS_H
#define POLL_DANCER_ERRORS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get a human-readable string for an error code.
 *
 * @param error The error code
 * @return A static string describing the error (do not free)
 */
const char *pd_error_string(pd_error_t error);

/**
 * Get the system error code associated with a loop.
 * This is typically errno on Unix or GetLastError() on Windows.
 *
 * @param loop The event loop
 * @return The system error code, or 0 if no system error occurred
 */
int pd_get_system_error(pd_loop_t *loop);

/**
 * Get a human-readable string for the system error.
 *
 * @param loop The event loop
 * @return A static string describing the system error (do not free)
 */
const char *pd_get_system_error_string(pd_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* POLL_DANCER_ERRORS_H */