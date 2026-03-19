/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Internal definitions shared across implementation files.
 */

#ifndef PD_INTERNAL_H
#define PD_INTERNAL_H

#include "poll-dancer/types.h"
#include <stddef.h>

/* Platform detection */
#if defined(__linux__)
    #define PD_PLATFORM_LINUX 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #define PD_PLATFORM_BSD 1
#elif defined(_WIN32) || defined(_WIN64)
    #define PD_PLATFORM_WINDOWS 1
#else
    #error "Unsupported platform"
#endif

/* Thread safety support */
#ifdef PD_THREAD_SAFE
    #include <pthread.h>
    #define PD_MUTEX_T pthread_mutex_t
    #define PD_MUTEX_INIT(mutex) pthread_mutex_init(&(mutex), NULL)
    #define PD_MUTEX_LOCK(mutex) pthread_mutex_lock(&(mutex))
    #define PD_MUTEX_UNLOCK(mutex) pthread_mutex_unlock(&(mutex))
    #define PD_MUTEX_DESTROY(mutex) pthread_mutex_destroy(&(mutex))
#else
    #define PD_MUTEX_T int
    #define PD_MUTEX_INIT(mutex) ((mutex) = 0)
    #define PD_MUTEX_LOCK(mutex) ((void)0)
    #define PD_MUTEX_UNLOCK(mutex) ((void)0)
    #define PD_MUTEX_DESTROY(mutex) ((void)0)
#endif

/* Min/max macros */
#define PD_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PD_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Array size macro */
#define PD_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/**
 * Set the system error for a loop.
 *
 * @param loop The loop
 * @param error The system error code
 */
void pd_set_system_error(struct pd_loop *loop, int error);

/**
 * Get the current system error (errno on Unix, GetLastError() on Windows).
 *
 * @return The current system error code
 */
int pd_get_current_system_error(void);

#endif /* PD_INTERNAL_H */