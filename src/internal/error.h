/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Internal error handling functions.
 */

#ifndef PD_ERROR_INTERNAL_H
#define PD_ERROR_INTERNAL_H

#include "poll-dancer/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set the system error for a loop.
 *
 * @param loop The loop (can be NULL)
 * @param error The system error code
 */
void pd_set_system_error(pd_loop_t *loop, int error);

/**
 * Get the current system error code.
 * On Unix, this returns errno.
 * On Windows, this returns GetLastError().
 *
 * @return The current system error code
 */
int pd_get_current_system_error(void);

#ifdef __cplusplus
}
#endif

#endif /* PD_ERROR_INTERNAL_H */