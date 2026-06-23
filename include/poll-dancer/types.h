/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * This library provides a unified API for event notification mechanisms
 * across different operating systems (epoll, kqueue, IOCP).
 */

#ifndef POLL_DANCER_TYPES_H
#define POLL_DANCER_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque handle to an event loop.
 * The loop manages all watchers and dispatches events.
 */
typedef struct pd_loop pd_loop_t;

/**
 * Opaque handle to a watcher.
 * A watcher monitors a file descriptor for specified events.
 */
typedef struct pd_watcher pd_watcher_t;

/**
 * Opaque handle to a timer.
 * A timer fires a callback after a specified timeout, optionally repeating.
 */
typedef struct pd_timer pd_timer_t;

/**
 * Error codes returned by library functions.
 * Negative values indicate errors, 0 indicates success.
 */
typedef enum {
    PD_OK = 0,                    /**< Operation successful */
    PD_ERR_UNKNOWN = -1,          /**< Unknown error */
    PD_ERR_INVALID_ARG = -2,     /**< Invalid argument */
    PD_ERR_NO_MEMORY = -3,       /**< Memory allocation failed */
    PD_ERR_SYSTEM = -4,          /**< System call failed */
    PD_ERR_NOT_IMPLEMENTED = -5, /**< Feature not implemented on this platform */
    PD_ERR_LOOP_CLOSED = -6,     /**< Loop has been closed */
    PD_ERR_WATCHER_STOPPED = -7, /**< Watcher is stopped */
    PD_ERR_ALREADY_EXISTS = -8,  /**< Resource already exists */
    PD_ERR_NOT_FOUND = -9,       /**< Resource not found */
} pd_error_t;

/**
 * Event types that can be monitored.
 * These can be combined using bitwise OR.
 */
typedef enum {
    PD_EVENT_NONE = 0,       /**< No events */
    PD_EVENT_READ = 1 << 0,  /**< Readable data available */
    PD_EVENT_WRITE = 1 << 1, /**< Writable (not full buffer) */
    PD_EVENT_ERROR = 1 << 2, /**< Error condition */
    PD_EVENT_HANGUP = 1 << 3, /**< Hangup (remote closed) */
    PD_EVENT_PRI = 1 << 4,   /**< Priority/out-of-band data */
    PD_EVENT_EDGE = 1 << 5,  /**< Edge-triggered mode (vs level-triggered) */
} pd_event_t;

/**
 * Callback function signature for event notifications.
 *
 * @param loop The event loop that triggered the callback
 * @param watcher The watcher that received the event
 * @param events The events that occurred (combination of pd_event_t values)
 * @param user_data User-provided data passed to pd_watcher_create
 */
typedef void (*pd_callback_t)(pd_loop_t *loop, pd_watcher_t *watcher,
                              pd_event_t events, void *user_data);

/**
 * Configuration for loop creation.
 */
typedef struct {
    int max_events_per_wait;  /**< Max events to return per wait call (default: 64) */
    int enable_thread_safety;  /**< Make loop thread-safe (default: 1) */
} pd_loop_config_t;

#ifdef __cplusplus
}
#endif

#endif /* POLL_DANCER_TYPES_H */