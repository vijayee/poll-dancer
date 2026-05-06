/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Internal timer structure definition.
 */

#ifndef PD_TIMER_INTERNAL_H
#define PD_TIMER_INTERNAL_H

#include "poll-dancer/types.h"
#include "internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Internal timer structure.
 */
struct pd_timer {
    struct pd_loop *loop;        /**< Loop this timer belongs to */
    pd_watcher_t *watcher;      /**< Internal watcher for the timer fd/event */
    uint64_t timeout_ms;        /**< Initial delay before first firing */
    uint64_t interval_ms;       /**< Repeat interval (0 = one-shot) */
    pd_callback_t callback;     /**< User callback function */
    void *user_data;            /**< User data for callback */
    int active;                 /**< Non-zero if timer is active */
    int owns_watcher;           /**< Non-zero if timer owns the watcher (epoll); 0 if manually allocated (kqueue/IOCP) */
    void *platform_data;        /**< Platform-specific timer state */
};

#ifdef __cplusplus
}
#endif

#endif /* PD_TIMER_INTERNAL_H */