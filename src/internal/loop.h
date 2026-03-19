/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Internal loop structure definition.
 */

#ifndef PD_LOOP_INTERNAL_H
#define PD_LOOP_INTERNAL_H

#include "poll-dancer/types.h"
#include "internal.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Internal loop structure.
 */
struct pd_loop {
    const pd_platform_ops_t *ops;  /**< Platform operations */
    int ref_count;                 /**< Reference count */
    int running;                   /**< Non-zero if loop is running */
    int stop_requested;            /**< Non-zero if stop was requested */
    int system_error;              /**< Last system error code */

    /* Configuration */
    int max_events_per_wait;       /**< Max events to return per wait */
    int enable_thread_safety;      /**< Thread safety enabled flag */

    /* Platform-specific data */
    void *platform_data;           /**< Platform backend private data */

    /* Watcher management */
    struct pd_watcher **watchers;  /**< Array of active watchers */
    size_t watcher_count;          /**< Number of active watchers */
    size_t watcher_capacity;       /**< Capacity of watchers array */

    /* Thread safety */
    PD_MUTEX_T mutex;              /**< Mutex for thread-safe operations */
};

/**
 * Initialize a loop structure.
 *
 * @param loop The loop to initialize
 * @param config Configuration for the loop
 * @return 0 on success, negative error code on failure
 */
int pd_loop_init(struct pd_loop *loop, const pd_loop_config_t *config);

/**
 * Clean up a loop structure.
 *
 * @param loop The loop to clean up
 */
void pd_loop_cleanup(struct pd_loop *loop);

/**
 * Add a watcher to the loop's watcher list.
 *
 * @param loop The loop
 * @param watcher The watcher to add
 * @return 0 on success, negative error code on failure
 */
int pd_loop_add_watcher(struct pd_loop *loop, struct pd_watcher *watcher);

/**
 * Remove a watcher from the loop's watcher list.
 *
 * @param loop The loop
 * @param watcher The watcher to remove
 * @return 0 on success, negative error code on failure
 */
int pd_loop_remove_watcher(struct pd_loop *loop, struct pd_watcher *watcher);

#ifdef __cplusplus
}
#endif

#endif /* PD_LOOP_INTERNAL_H */