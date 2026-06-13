/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Internal watcher structure definition.
 */

#ifndef PD_WATCHER_INTERNAL_H
#define PD_WATCHER_INTERNAL_H

#include "poll-dancer/types.h"
#include "internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Internal watcher structure.
 *
 * The watcher can monitor either an OS file descriptor (`fd`) or a native
 * Windows HANDLE (`handle`). On POSIX builds only `fd` is meaningful. On
 * Windows, callers that obtained the resource as a HANDLE (e.g. a named
 * pipe) use `pd_watcher_create_for_handle`; callers with a CRT file
 * descriptor continue to use `pd_watcher_create` which stores the fd and
 * resolves it to a HANDLE on demand.
 */
struct pd_watcher {
    int fd;                        /**< File descriptor being watched */
    void *handle;                  /**< Native handle (Windows HANDLE), or NULL */
    int is_handle;                 /**< Non-zero if `handle` is the watched resource */
    pd_event_t events;            /**< Events being monitored */
    pd_callback_t callback;       /**< Callback function */
    void *user_data;              /**< User data for callback */
    struct pd_loop *loop;         /**< Loop this watcher belongs to */
    int active;                   /**< Non-zero if watcher is active */
    int ref_count;                 /**< Reference count */

    /* Platform-specific data */
    void *platform_data;          /**< Platform backend private data */
};

/**
 * Initialize a watcher structure.
 *
 * @param watcher The watcher to initialize
 * @param loop The loop to attach to
 * @param fd The file descriptor to monitor
 * @param events The events to monitor
 * @param callback The callback function
 * @param user_data User data for callback
 * @return 0 on success, negative error code on failure
 */
int pd_watcher_init(struct pd_watcher *watcher,
                    struct pd_loop *loop,
                    int fd,
                    pd_event_t events,
                    pd_callback_t callback,
                    void *user_data);

/**
 * Clean up a watcher structure.
 *
 * @param watcher The watcher to clean up
 */
void pd_watcher_cleanup(struct pd_watcher *watcher);

#ifdef __cplusplus
}
#endif

#endif /* PD_WATCHER_INTERNAL_H */