/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Platform abstraction interface.
 * Each platform backend implements this interface.
 */

#ifndef PD_PLATFORM_H
#define PD_PLATFORM_H

#include "poll-dancer/types.h"
#include "internal.h"

/* Forward declarations */
struct pd_loop;
struct pd_watcher;
struct pd_timer;

/**
 * Platform-specific operations.
 * Each backend (epoll, kqueue, IOCP) implements these functions.
 */
typedef struct pd_platform_ops {
    /* Loop operations */
    int (*loop_create)(struct pd_loop *loop, const pd_loop_config_t *config);
    void (*loop_destroy)(struct pd_loop *loop);
    int (*loop_run)(struct pd_loop *loop, int timeout_ms);
    int (*loop_stop)(struct pd_loop *loop);

    /* Watcher operations */
    int (*watcher_register)(struct pd_loop *loop, struct pd_watcher *watcher);
    int (*watcher_update)(struct pd_watcher *watcher, pd_event_t events);
    int (*watcher_unregister)(struct pd_watcher *watcher);

    /* Async operations */
    int (*async_send)(struct pd_loop *loop, void *data);

    /* Timer operations */
    int (*timer_create)(struct pd_loop *loop, struct pd_timer *timer);
    int (*timer_start)(struct pd_timer *timer);
    int (*timer_stop)(struct pd_timer *timer);
    void (*timer_destroy)(struct pd_timer *timer);

    /* Platform info */
    const char *name;       /**< Platform name (e.g., "epoll", "kqueue", "iocp") */
    int max_events;         /**< Maximum events that can be returned in one wait */
} pd_platform_ops_t;

/**
 * Detect and return the platform operations for the current system.
 *
 * @return Pointer to the platform operations, or NULL if not supported
 */
const pd_platform_ops_t *pd_platform_detect(void);

/* Platform implementations */
extern const pd_platform_ops_t pd_platform_epoll;
extern const pd_platform_ops_t pd_platform_kqueue;
extern const pd_platform_ops_t pd_platform_iocp;

#endif /* PD_PLATFORM_H */