/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * BSD/macOS kqueue backend implementation.
 */

#include "internal/platform.h"
#include "poll-dancer/poll-dancer.h"
#include "internal/loop.h"
#include "internal/watcher.h"
#include "internal/timer.h"
#include "internal/internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

/**
 * Platform-specific data for kqueue.
 */
typedef struct {
    int kqueue_fd;                   /**< kqueue file descriptor */
    struct kevent *events;           /**< Event array for kevent */
    int max_events;                  /**< Maximum events per wait */
} pd_kqueue_data_t;

/**
 * Platform-specific watcher data for kqueue.
 */
typedef struct {
    struct kevent kev_read;          /**< Read filter kevent */
    struct kevent kev_write;         /**< Write filter kevent */
} pd_kqueue_watcher_data_t;

/* Forward declarations */
static int kqueue_loop_create(pd_loop_t *loop, const pd_loop_config_t *config);
static void kqueue_loop_destroy(pd_loop_t *loop);
static int kqueue_loop_run(pd_loop_t *loop, int timeout_ms);
static int kqueue_loop_stop(pd_loop_t *loop);
static int kqueue_watcher_register(pd_loop_t *loop, pd_watcher_t *watcher);
static int kqueue_watcher_update(pd_watcher_t *watcher, pd_event_t events);
static int kqueue_watcher_unregister(pd_watcher_t *watcher);
static int kqueue_async_send(pd_loop_t *loop, void *data);
static int kqueue_timer_create(pd_loop_t *loop, pd_timer_t *timer);
static int kqueue_timer_start(pd_timer_t *timer);
static int kqueue_timer_stop(pd_timer_t *timer);
static void kqueue_timer_destroy(pd_timer_t *timer);

/* Platform operations */
const pd_platform_ops_t pd_platform_kqueue = {
    .loop_create = kqueue_loop_create,
    .loop_destroy = kqueue_loop_destroy,
    .loop_run = kqueue_loop_run,
    .loop_stop = kqueue_loop_stop,
    .watcher_register = kqueue_watcher_register,
    .watcher_update = kqueue_watcher_update,
    .watcher_unregister = kqueue_watcher_unregister,
    .async_send = kqueue_async_send,
    .timer_create = kqueue_timer_create,
    .timer_start = kqueue_timer_start,
    .timer_stop = kqueue_timer_stop,
    .timer_destroy = kqueue_timer_destroy,
    .name = "kqueue",
    .max_events = 1024,
};

/**
 * Get filter flags from poll-dancer events.
 */
static short events_to_filter(pd_event_t events, int *need_read, int *need_write) {
    *need_read = (events & PD_EVENT_READ) != 0;
    *need_write = (events & PD_EVENT_WRITE) != 0;
    return 0;
}

static int kqueue_loop_create(pd_loop_t *loop, const pd_loop_config_t *config) {
    /* Allocate platform data */
    pd_kqueue_data_t *data = calloc(1, sizeof(pd_kqueue_data_t));
    if (!data) {
        pd_set_system_error(loop, pd_get_current_system_error());
        return PD_ERR_NO_MEMORY;
    }

    /* Create kqueue */
    data->kqueue_fd = kqueue();
    if (data->kqueue_fd < 0) {
        pd_set_system_error(loop, pd_get_current_system_error());
        free(data);
        return PD_ERR_SYSTEM;
    }

    /* Allocate event array */
    data->max_events = config->max_events_per_wait;
    data->events = calloc(data->max_events, sizeof(struct kevent));
    if (!data->events) {
        close(data->kqueue_fd);
        free(data);
        return PD_ERR_NO_MEMORY;
    }

    loop->platform_data = data;
    return PD_OK;
}

static void kqueue_loop_destroy(pd_loop_t *loop) {
    if (!loop || !loop->platform_data) {
        return;
    }

    pd_kqueue_data_t *data = (pd_kqueue_data_t *)loop->platform_data;

    if (data->kqueue_fd >= 0) {
        close(data->kqueue_fd);
    }

    free(data->events);
    free(data);
    loop->platform_data = NULL;
}

static int kqueue_loop_run(pd_loop_t *loop, int timeout_ms) {
    if (!loop || !loop->platform_data) {
        return PD_ERR_INVALID_ARG;
    }

    pd_kqueue_data_t *data = (pd_kqueue_data_t *)loop->platform_data;

    /* Convert timeout to timespec */
    struct timespec timeout;
    struct timespec *timeout_ptr = NULL;
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (timeout_ms % 1000) * 1000000;
        timeout_ptr = &timeout;
    }

    /* Wait for events */
    int nfds = kevent(data->kqueue_fd, NULL, 0, data->events, data->max_events, timeout_ptr);
    if (nfds < 0) {
        if (errno == EINTR) {
            /* Interrupted by signal, not an error */
            return 0;
        }
        pd_set_system_error(loop, pd_get_current_system_error());
        return PD_ERR_SYSTEM;
    }

    /* Process events */
    for (int i = 0; i < nfds; i++) {
        struct kevent *event = &data->events[i];

        /* Handle timer events specially */
        if (event->filter == EVFILT_TIMER) {
            pd_timer_t *timer = (pd_timer_t *)event->udata;
            if (timer && timer->callback) {
                /* Check if this is the initial one-shot phase of a two-phase
                 * repeating timer. If so, reconfigure as a repeating timer. */
                pd_kqueue_timer_data_t *timer_data =
                    (pd_kqueue_timer_data_t *)timer->platform_data;
                if (timer_data && timer_data->pending_initial) {
                    timer_data->pending_initial = 0;
                    /* Reconfigure as a repeating timer with interval_ms */
                    struct kevent change;
                    EV_SET(&change, (uintptr_t)timer, EVFILT_TIMER,
                           EV_ADD | EV_ENABLE,
                           NOTE_MSECONDS,
                           timer->interval_ms,
                           timer);
                    kevent(data->kqueue_fd, &change, 1, NULL, 0, NULL);
                }
                timer->callback(loop, timer->watcher, PD_EVENT_READ, timer->user_data);
            }
            continue;
        }

        pd_watcher_t *watcher = (pd_watcher_t *)event->udata;

        if (!watcher || !watcher->callback) {
            continue;
        }

        /* Convert kqueue event to poll-dancer event */
        pd_event_t events = PD_EVENT_NONE;
        if (event->filter == EVFILT_READ) {
            events |= PD_EVENT_READ;
        }
        if (event->filter == EVFILT_WRITE) {
            events |= PD_EVENT_WRITE;
        }
        if (event->flags & EV_ERROR) {
            events |= PD_EVENT_ERROR;
        }
        if (event->flags & EV_EOF) {
            events |= PD_EVENT_HANGUP;
        }

        /* Invoke callback */
        watcher->callback(loop, watcher, events, watcher->user_data);
    }

    return nfds;
}

static int kqueue_loop_stop(pd_loop_t *loop) {
    /* The generic implementation handles the stop flag.
     * For kqueue, we could use an EVFILT_USER event to wake up kevent.
     */
    return PD_OK;
}

static int kqueue_watcher_register(pd_loop_t *loop, pd_watcher_t *watcher) {
    if (!loop || !watcher) {
        return PD_ERR_INVALID_ARG;
    }

    pd_kqueue_data_t *data = (pd_kqueue_data_t *)loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    /* Allocate watcher platform data */
    pd_kqueue_watcher_data_t *watcher_data = calloc(1, sizeof(pd_kqueue_watcher_data_t));
    if (!watcher_data) {
        pd_set_system_error(loop, pd_get_current_system_error());
        return PD_ERR_NO_MEMORY;
    }

    /* Set up read filter if requested */
    if (watcher->events & PD_EVENT_READ) {
        EV_SET(&watcher_data->kev_read, watcher->fd, EVFILT_READ,
               EV_ADD | EV_ENABLE | (watcher->events & PD_EVENT_EDGE ? EV_CLEAR : 0),
               0, 0, watcher);
    }

    /* Set up write filter if requested */
    if (watcher->events & PD_EVENT_WRITE) {
        EV_SET(&watcher_data->kev_write, watcher->fd, EVFILT_WRITE,
               EV_ADD | EV_ENABLE | (watcher->events & PD_EVENT_EDGE ? EV_CLEAR : 0),
               0, 0, watcher);
    }

    /* Register filters with kqueue */
    struct kevent changes[2];
    int nchanges = 0;

    if (watcher->events & PD_EVENT_READ) {
        changes[nchanges++] = watcher_data->kev_read;
    }
    if (watcher->events & PD_EVENT_WRITE) {
        changes[nchanges++] = watcher_data->kev_write;
    }

    if (nchanges > 0) {
        int result = kevent(data->kqueue_fd, changes, nchanges, NULL, 0, NULL);
        if (result < 0) {
            pd_set_system_error(loop, pd_get_current_system_error());
            free(watcher_data);
            return PD_ERR_SYSTEM;
        }
    }

    watcher->platform_data = watcher_data;
    return PD_OK;
}

static int kqueue_watcher_update(pd_watcher_t *watcher, pd_event_t events) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_kqueue_data_t *data = (pd_kqueue_data_t *)watcher->loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    pd_kqueue_watcher_data_t *watcher_data = (pd_kqueue_watcher_data_t *)watcher->platform_data;
    if (!watcher_data) {
        return PD_ERR_INVALID_ARG;
    }

    /* Disable old filters and enable new ones */
    struct kevent changes[4];
    int nchanges = 0;

    /* Disable old filters */
    if (watcher->events & PD_EVENT_READ) {
        if (!(events & PD_EVENT_READ)) {
            EV_SET(&changes[nchanges], watcher->fd, EVFILT_READ, EV_DISABLE, 0, 0, watcher);
            nchanges++;
        }
    }

    if (watcher->events & PD_EVENT_WRITE) {
        if (!(events & PD_EVENT_WRITE)) {
            EV_SET(&changes[nchanges], watcher->fd, EVFILT_WRITE, EV_DISABLE, 0, 0, watcher);
            nchanges++;
        }
    }

    /* Enable new filters */
    if (events & PD_EVENT_READ) {
        if (!(watcher->events & PD_EVENT_READ)) {
            EV_SET(&changes[nchanges], watcher->fd, EVFILT_READ,
                   EV_ADD | EV_ENABLE | (events & PD_EVENT_EDGE ? EV_CLEAR : 0),
                   0, 0, watcher);
            nchanges++;
        }
    }

    if (events & PD_EVENT_WRITE) {
        if (!(watcher->events & PD_EVENT_WRITE)) {
            EV_SET(&changes[nchanges], watcher->fd, EVFILT_WRITE,
                   EV_ADD | EV_ENABLE | (events & PD_EVENT_EDGE ? EV_CLEAR : 0),
                   0, 0, watcher);
            nchanges++;
        }
    }

    if (nchanges > 0) {
        int result = kevent(data->kqueue_fd, changes, nchanges, NULL, 0, NULL);
        if (result < 0) {
            pd_set_system_error(watcher->loop, pd_get_current_system_error());
            return PD_ERR_SYSTEM;
        }
    }

    /* Update stored events */
    if (events & PD_EVENT_READ) {
        EV_SET(&watcher_data->kev_read, watcher->fd, EVFILT_READ,
               EV_ADD | EV_ENABLE | (events & PD_EVENT_EDGE ? EV_CLEAR : 0),
               0, 0, watcher);
    }
    if (events & PD_EVENT_WRITE) {
        EV_SET(&watcher_data->kev_write, watcher->fd, EVFILT_WRITE,
               EV_ADD | EV_ENABLE | (events & PD_EVENT_EDGE ? EV_CLEAR : 0),
               0, 0, watcher);
    }

    return PD_OK;
}

static int kqueue_watcher_unregister(pd_watcher_t *watcher) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_kqueue_data_t *data = (pd_kqueue_data_t *)watcher->loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    pd_kqueue_watcher_data_t *watcher_data = (pd_kqueue_watcher_data_t *)watcher->platform_data;
    if (!watcher_data) {
        return PD_ERR_INVALID_ARG;
    }

    /* Delete filters from kqueue */
    struct kevent changes[2];
    int nchanges = 0;

    if (watcher->events & PD_EVENT_READ) {
        EV_SET(&changes[nchanges], watcher->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        nchanges++;
    }
    if (watcher->events & PD_EVENT_WRITE) {
        EV_SET(&changes[nchanges], watcher->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        nchanges++;
    }

    if (nchanges > 0) {
        /* Ignore errors, fd might have been closed */
        kevent(data->kqueue_fd, changes, nchanges, NULL, 0, NULL);
    }

    /* Free watcher platform data */
    free(watcher->platform_data);
    watcher->platform_data = NULL;

    return PD_OK;
}

static int kqueue_async_send(pd_loop_t *loop, void *data) {
    /* For kqueue, we could use EVFILT_USER to wake up kevent.
     * For simplicity, this implementation doesn't provide async wake-up.
     * A full implementation would create an EVFILT_USER event on loop creation
     * and trigger it here.
     */
    (void)data;
    return PD_ERR_NOT_IMPLEMENTED;
}

/* ============================================================================
 * Timer implementation using EVFILT_TIMER
 * ============================================================================ */

/**
 * Platform-specific timer data for kqueue timers.
 */
typedef struct {
    int registered;       /**< Non-zero if timer kevent is registered with kqueue */
    int pending_initial;  /**< Non-zero if initial one-shot phase is active */
} pd_kqueue_timer_data_t;

static int kqueue_timer_create(pd_loop_t *loop, pd_timer_t *timer) {
    if (!loop || !timer) {
        return PD_ERR_INVALID_ARG;
    }

    /* Allocate platform data */
    pd_kqueue_timer_data_t *timer_data = calloc(1, sizeof(pd_kqueue_timer_data_t));
    if (!timer_data) {
        pd_set_system_error(loop, pd_get_current_system_error());
        return PD_ERR_NO_MEMORY;
    }

    timer->platform_data = timer_data;

    /* Create a placeholder watcher that is NOT registered with kqueue.
     * kqueue timers use EVFILT_TIMER with the timer pointer as the identifier,
     * so no real file descriptor is needed. The watcher exists only as a
     * container for the timer's callback and user_data that the event loop
     * dispatches to when it detects a timer event. */
    pd_watcher_t *watcher = calloc(1, sizeof(pd_watcher_t));
    if (!watcher) {
        free(timer_data);
        timer->platform_data = NULL;
        return PD_ERR_NO_MEMORY;
    }
    watcher->fd = -1;
    watcher->events = PD_EVENT_NONE;
    watcher->callback = NULL;
    watcher->user_data = timer;
    watcher->loop = loop;
    watcher->active = 0;
    watcher->ref_count = 1;
    watcher->platform_data = NULL;
    timer->watcher = watcher;

    return PD_OK;
}

static int kqueue_timer_start(pd_timer_t *timer) {
    if (!timer || !timer->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_kqueue_data_t *data = (pd_kqueue_data_t *)timer->loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    pd_kqueue_timer_data_t *timer_data = (pd_kqueue_timer_data_t *)timer->platform_data;
    if (!timer_data) {
        return PD_ERR_INVALID_ARG;
    }

    /* Use the timer pointer as the kevent identifier.
     * udata points to the timer so kqueue_loop_run can dispatch correctly. */
    struct kevent change;

    if (timer->interval_ms > 0 && timer->timeout_ms != timer->interval_ms &&
        timer->timeout_ms != 0) {
        /* Repeating timer with a different initial delay: start as one-shot
         * with timeout_ms, then reconfigure to repeat at interval_ms when
         * the initial timer fires. */
        EV_SET(&change, (uintptr_t)timer, EVFILT_TIMER,
               EV_ADD | EV_ENABLE | EV_ONESHOT,
               NOTE_MSECONDS,
               timer->timeout_ms,
               timer);
        timer_data->pending_initial = 1;
    } else {
        /* One-shot timer or repeating timer with same initial delay/interval */
        EV_SET(&change, (uintptr_t)timer, EVFILT_TIMER,
               EV_ADD | EV_ENABLE,
               NOTE_MSECONDS,
               timer->interval_ms > 0 ? timer->interval_ms : timer->timeout_ms,
               timer);
        timer_data->pending_initial = 0;
    }

    int result = kevent(data->kqueue_fd, &change, 1, NULL, 0, NULL);
    if (result < 0) {
        pd_set_system_error(timer->loop, pd_get_current_system_error());
        return PD_ERR_SYSTEM;
    }

    timer_data->registered = 1;
    return PD_OK;
}

static int kqueue_timer_stop(pd_timer_t *timer) {
    if (!timer || !timer->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_kqueue_data_t *data = (pd_kqueue_data_t *)timer->loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    pd_kqueue_timer_data_t *timer_data = (pd_kqueue_timer_data_t *)timer->platform_data;
    if (!timer_data || !timer_data->registered) {
        return PD_OK;  /* Not registered, nothing to do */
    }

    /* Delete the EVFILT_TIMER kevent */
    struct kevent change;
    EV_SET(&change, (uintptr_t)timer, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);

    /* Ignore errors - timer may have already been deleted */
    kevent(data->kqueue_fd, &change, 1, NULL, 0, NULL);

    timer_data->registered = 0;
    timer_data->pending_initial = 0;
    return PD_OK;
}

static void kqueue_timer_destroy(pd_timer_t *timer) {
    if (!timer) {
        return;
    }

    /* Free platform data */
    if (timer->platform_data) {
        free(timer->platform_data);
        timer->platform_data = NULL;
    }

    /* Free the placeholder watcher (manually allocated, not registered) */
    if (timer->watcher) {
        free(timer->watcher);
        timer->watcher = NULL;
    }
}