/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Generic timer implementation.
 * Delegates to platform-specific timer operations.
 */

#include "poll-dancer/poll-dancer.h"
#include "internal/timer.h"
#include "internal/loop.h"
#include "internal/watcher.h"
#include "internal/platform.h"
#include "internal/internal.h"

#include <stdlib.h>
#include <string.h>

pd_timer_t *pd_timer_create(pd_loop_t *loop, uint64_t timeout_ms,
                            uint64_t interval_ms, pd_callback_t callback,
                            void *user_data) {
    if (!loop || !callback) {
        return NULL;
    }

    /* Allocate timer structure */
    pd_timer_t *timer = calloc(1, sizeof(pd_timer_t));
    if (!timer) {
        return NULL;
    }

    timer->loop = loop;
    timer->timeout_ms = timeout_ms;
    timer->interval_ms = interval_ms;
    timer->callback = callback;
    timer->user_data = user_data;
    timer->active = 0;
    timer->watcher = NULL;
    timer->platform_data = NULL;

    /* Call platform-specific timer creation */
    if (loop->ops && loop->ops->timer_create) {
        int result = loop->ops->timer_create(loop, timer);
        if (result != 0) {
            free(timer);
            return NULL;
        }
    }

    return timer;
}

pd_error_t pd_timer_start(pd_timer_t *timer) {
    if (!timer || !timer->loop) {
        return PD_ERR_INVALID_ARG;
    }

    if (timer->active) {
        return PD_OK;  /* Already started */
    }

    timer->active = 1;

    /* Call platform-specific timer start */
    if (timer->loop->ops && timer->loop->ops->timer_start) {
        int result = timer->loop->ops->timer_start(timer);
        if (result != 0) {
            timer->active = 0;
            return (pd_error_t)result;
        }
    }

    return PD_OK;
}

pd_error_t pd_timer_stop(pd_timer_t *timer) {
    if (!timer || !timer->loop) {
        return PD_ERR_INVALID_ARG;
    }

    if (!timer->active) {
        return PD_OK;  /* Already stopped */
    }

    timer->active = 0;

    /* Call platform-specific timer stop */
    if (timer->loop->ops && timer->loop->ops->timer_stop) {
        int result = timer->loop->ops->timer_stop(timer);
        if (result != 0) {
            timer->active = 1;
            return (pd_error_t)result;
        }
    }

    return PD_OK;
}

pd_error_t pd_timer_destroy(pd_timer_t *timer) {
    if (!timer) {
        return PD_ERR_INVALID_ARG;
    }

    /* Stop the timer first */
    if (timer->active) {
        pd_timer_stop(timer);
    }

    /* Call platform-specific timer destroy */
    if (timer->loop && timer->loop->ops && timer->loop->ops->timer_destroy) {
        timer->loop->ops->timer_destroy(timer);
    }

    /* Destroy internal watcher if it exists and we own it.
     * On epoll, the watcher was created via pd_watcher_create and should
     * be destroyed via pd_watcher_destroy. On kqueue/IOCP, the watcher
     * was manually allocated and already freed by the platform's
     * timer_destroy function. */
    if (timer->watcher && timer->owns_watcher) {
        pd_watcher_destroy(timer->watcher);
        timer->watcher = NULL;
    }

    free(timer);
    return PD_OK;
}