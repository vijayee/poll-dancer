/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * This library provides a unified API for event notification mechanisms
 * across different operating systems (epoll, kqueue, IOCP).
 */

#ifndef POLL_DANCER_H
#define POLL_DANCER_H

#include "types.h"
#include "errors.h"
#include "version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Loop Lifecycle Management
 * ============================================================================ */

/**
 * Create a new event loop.
 *
 * @param config Configuration for the loop (can be NULL for defaults)
 * @return A new loop handle, or NULL on error
 */
pd_loop_t *pd_loop_create(const pd_loop_config_t *config);

/**
 * Get the process-wide default event loop.
 * The default loop is created on first call and destroyed on process exit.
 * This is thread-safe if POLL_DANCER_THREAD_SAFE was enabled at compile time.
 *
 * @return The default loop handle, or NULL on error
 */
pd_loop_t *pd_loop_default(void);

/**
 * Destroy an event loop.
 * All watchers must be destroyed before calling this.
 *
 * @param loop The loop to destroy
 */
void pd_loop_destroy(pd_loop_t *loop);

/**
 * Increment the loop reference count.
 *
 * @param loop The loop to reference
 */
void pd_loop_ref(pd_loop_t *loop);

/**
 * Decrement the loop reference count.
 * When the count reaches zero, the loop is destroyed.
 *
 * @param loop The loop to unreference
 */
void pd_loop_unref(pd_loop_t *loop);

/* ============================================================================
 * Event Loop Control
 * ============================================================================ */

/**
 * Run the event loop.
 * This will block until there are no more active watchers or pd_loop_stop is called.
 *
 * @param loop The loop to run
 * @return 0 on success, negative error code on failure
 */
int pd_loop_run(pd_loop_t *loop);

/**
 * Run the event loop once with a timeout.
 * This will wait for events up to timeout_ms milliseconds.
 *
 * @param loop The loop to run
 * @param timeout_ms Maximum time to wait in milliseconds (-1 for infinite)
 * @return Number of events processed, or negative error code on failure
 */
int pd_loop_run_once(pd_loop_t *loop, int timeout_ms);

/**
 * Stop a running event loop.
 * This can be called from another thread or from within a callback.
 *
 * @param loop The loop to stop
 * @return 0 on success, negative error code on failure
 */
int pd_loop_stop(pd_loop_t *loop);

/* ============================================================================
 * Watcher Management
 * ============================================================================ */

/**
 * Create a new watcher.
 * The watcher is initially active.
 *
 * @param loop The loop to attach the watcher to
 * @param fd The file descriptor to monitor
 * @param events The events to monitor (combination of pd_event_t values)
 * @param callback The callback to invoke when events occur
 * @param user_data User data to pass to the callback
 * @return A new watcher handle, or NULL on error
 */
pd_watcher_t *pd_watcher_create(pd_loop_t *loop,
                                 int fd,
                                 pd_event_t events,
                                 pd_callback_t callback,
                                 void *user_data);

/**
 * Update the events monitored by a watcher.
 *
 * @param watcher The watcher to update
 * @param events The new events to monitor
 * @return 0 on success, negative error code on failure
 */
int pd_watcher_update(pd_watcher_t *watcher, pd_event_t events);

/**
 * Stop a watcher.
 * The watcher remains allocated but no longer receives events.
 *
 * @param watcher The watcher to stop
 * @return 0 on success, negative error code on failure
 */
int pd_watcher_stop(pd_watcher_t *watcher);

/**
 * Start a stopped watcher.
 *
 * @param watcher The watcher to start
 * @return 0 on success, negative error code on failure
 */
int pd_watcher_start(pd_watcher_t *watcher);

/**
 * Destroy a watcher.
 * The watcher handle becomes invalid after this call.
 *
 * @param watcher The watcher to destroy
 */
void pd_watcher_destroy(pd_watcher_t *watcher);

/**
 * Get the file descriptor associated with a watcher.
 *
 * @param watcher The watcher
 * @return The file descriptor, or -1 if the watcher is invalid
 */
int pd_watcher_get_fd(pd_watcher_t *watcher);

/**
 * Get the events monitored by a watcher.
 *
 * @param watcher The watcher
 * @return The events, or PD_EVENT_NONE if the watcher is invalid
 */
pd_event_t pd_watcher_get_events(pd_watcher_t *watcher);

/* ============================================================================
 * Timer Management
 * ============================================================================ */

/**
 * Create a new timer.
 *
 * @param loop The loop to attach the timer to
 * @param timeout_ms Initial delay before first firing (0 = fire immediately)
 * @param interval_ms Repeat interval (0 = one-shot timer)
 * @param callback The callback to invoke when the timer fires
 * @param user_data User data to pass to the callback
 * @return A new timer handle, or NULL on error
 */
pd_timer_t *pd_timer_create(pd_loop_t *loop, uint64_t timeout_ms,
                            uint64_t interval_ms, pd_callback_t callback,
                            void *user_data);

/**
 * Start a timer.
 * The timer begins counting from when this is called.
 *
 * @param timer The timer to start
 * @return 0 on success, negative error code on failure
 */
pd_error_t pd_timer_start(pd_timer_t *timer);

/**
 * Stop a timer.
 * The timer stops firing but remains allocated.
 *
 * @param timer The timer to stop
 * @return 0 on success, negative error code on failure
 */
pd_error_t pd_timer_stop(pd_timer_t *timer);

/**
 * Destroy a timer.
 * The timer handle becomes invalid after this call.
 *
 * @param timer The timer to destroy
 * @return 0 on success, negative error code on failure
 */
pd_error_t pd_timer_destroy(pd_timer_t *timer);

/* ============================================================================
 * Async Operations
 * ============================================================================ */

/**
 * Send an async notification to a loop.
 * This can be called from another thread to wake up the loop.
 *
 * @param loop The loop to notify
 * @param data User data to pass to the loop (can be NULL)
 * @return 0 on success, negative error code on failure
 */
int pd_loop_async_send(pd_loop_t *loop, void *data);

/**
 * Retrieve the async data pointer from a loop.
 * The async data is set by the most recent call to pd_loop_async_send.
 * This should be called from within the loop thread after the loop
 * has been woken by an async notification.
 *
 * @param loop The loop to get async data from
 * @return The async data pointer, or NULL if none was set
 */
void *pd_loop_get_async_data(pd_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* POLL_DANCER_H */