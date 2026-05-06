/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Linux epoll(7) backend implementation.
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
#include <sys/epoll.h>
#include <sys/timerfd.h>

/**
 * Platform-specific data for epoll.
 */
typedef struct {
    int epoll_fd;                    /**< epoll file descriptor */
    struct epoll_event *events;      /**< Event array for epoll_wait */
    int max_events;                  /**< Maximum events per wait */
} pd_epoll_data_t;

/**
 * Platform-specific watcher data for epoll.
 */
typedef struct {
    struct epoll_event epoll_event;  /**< epoll event configuration */
} pd_epoll_watcher_data_t;

/* Forward declarations */
static int epoll_loop_create(pd_loop_t *loop, const pd_loop_config_t *config);
static void epoll_loop_destroy(pd_loop_t *loop);
static int epoll_loop_run(pd_loop_t *loop, int timeout_ms);
static int epoll_loop_stop(pd_loop_t *loop);
static int epoll_watcher_register(pd_loop_t *loop, pd_watcher_t *watcher);
static int epoll_watcher_update(pd_watcher_t *watcher, pd_event_t events);
static int epoll_watcher_unregister(pd_watcher_t *watcher);
static int epoll_async_send(pd_loop_t *loop, void *data);
static int epoll_timer_create(pd_loop_t *loop, pd_timer_t *timer);
static int epoll_timer_start(pd_timer_t *timer);
static int epoll_timer_stop(pd_timer_t *timer);
static void epoll_timer_destroy(pd_timer_t *timer);

/* Platform operations */
const pd_platform_ops_t pd_platform_epoll = {
    .loop_create = epoll_loop_create,
    .loop_destroy = epoll_loop_destroy,
    .loop_run = epoll_loop_run,
    .loop_stop = epoll_loop_stop,
    .watcher_register = epoll_watcher_register,
    .watcher_update = epoll_watcher_update,
    .watcher_unregister = epoll_watcher_unregister,
    .async_send = epoll_async_send,
    .timer_create = epoll_timer_create,
    .timer_start = epoll_timer_start,
    .timer_stop = epoll_timer_stop,
    .timer_destroy = epoll_timer_destroy,
    .name = "epoll",
    .max_events = 1024,
};

/**
 * Convert poll-dancer events to epoll events.
 */
static uint32_t events_to_epoll(pd_event_t events) {
    uint32_t epoll_events = 0;

    if (events & PD_EVENT_READ) {
        epoll_events |= EPOLLIN;
    }
    if (events & PD_EVENT_WRITE) {
        epoll_events |= EPOLLOUT;
    }
    if (events & PD_EVENT_ERROR) {
        epoll_events |= EPOLLERR;
    }
    if (events & PD_EVENT_HANGUP) {
        epoll_events |= EPOLLHUP | EPOLLRDHUP;
    }
    if (events & PD_EVENT_PRI) {
        epoll_events |= EPOLLPRI;
    }
    if (events & PD_EVENT_EDGE) {
        epoll_events |= EPOLLET;
    }

    return epoll_events;
}

/**
 * Convert epoll events to poll-dancer events.
 */
static pd_event_t epoll_to_events(uint32_t epoll_events) {
    pd_event_t events = PD_EVENT_NONE;

    if (epoll_events & EPOLLIN) {
        events |= PD_EVENT_READ;
    }
    if (epoll_events & EPOLLOUT) {
        events |= PD_EVENT_WRITE;
    }
    if (epoll_events & EPOLLERR) {
        events |= PD_EVENT_ERROR;
    }
    if (epoll_events & (EPOLLHUP | EPOLLRDHUP)) {
        events |= PD_EVENT_HANGUP;
    }
    if (epoll_events & EPOLLPRI) {
        events |= PD_EVENT_PRI;
    }

    return events;
}

static int epoll_loop_create(pd_loop_t *loop, const pd_loop_config_t *config) {
    /* Allocate platform data */
    pd_epoll_data_t *data = calloc(1, sizeof(pd_epoll_data_t));
    if (!data) {
        pd_set_system_error(loop, pd_get_current_system_error());
        return PD_ERR_NO_MEMORY;
    }

    /* Create epoll instance */
    data->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (data->epoll_fd < 0) {
        pd_set_system_error(loop, pd_get_current_system_error());
        free(data);
        return PD_ERR_SYSTEM;
    }

    /* Allocate event array */
    data->max_events = config->max_events_per_wait;
    data->events = calloc(data->max_events, sizeof(struct epoll_event));
    if (!data->events) {
        close(data->epoll_fd);
        free(data);
        return PD_ERR_NO_MEMORY;
    }

    loop->platform_data = data;
    return PD_OK;
}

static void epoll_loop_destroy(pd_loop_t *loop) {
    if (!loop || !loop->platform_data) {
        return;
    }

    pd_epoll_data_t *data = (pd_epoll_data_t *)loop->platform_data;

    if (data->epoll_fd >= 0) {
        close(data->epoll_fd);
    }

    free(data->events);
    free(data);
    loop->platform_data = NULL;
}

static int epoll_loop_run(pd_loop_t *loop, int timeout_ms) {
    if (!loop || !loop->platform_data) {
        return PD_ERR_INVALID_ARG;
    }

    pd_epoll_data_t *data = (pd_epoll_data_t *)loop->platform_data;

    /* Convert timeout to epoll format */
    int epoll_timeout = timeout_ms < 0 ? -1 : timeout_ms;

    /* Wait for events */
    int nfds = epoll_wait(data->epoll_fd, data->events, data->max_events, epoll_timeout);
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
        struct epoll_event *event = &data->events[i];
        pd_watcher_t *watcher = (pd_watcher_t *)event->data.ptr;

        if (!watcher || !watcher->callback) {
            continue;
        }

        /* Convert events and invoke callback */
        pd_event_t events = epoll_to_events(event->events);
        watcher->callback(loop, watcher, events, watcher->user_data);
    }

    return nfds;
}

static int epoll_loop_stop(pd_loop_t *loop) {
    /* The generic implementation handles the stop flag.
     * For epoll, we could use eventfd or a pipe to wake up epoll_wait,
     * but the simpler approach is to just let epoll_wait timeout.
     */
    return PD_OK;
}

static int epoll_watcher_register(pd_loop_t *loop, pd_watcher_t *watcher) {
    if (!loop || !watcher) {
        return PD_ERR_INVALID_ARG;
    }

    pd_epoll_data_t *data = (pd_epoll_data_t *)loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    /* Allocate watcher platform data */
    pd_epoll_watcher_data_t *watcher_data = calloc(1, sizeof(pd_epoll_watcher_data_t));
    if (!watcher_data) {
        pd_set_system_error(loop, pd_get_current_system_error());
        return PD_ERR_NO_MEMORY;
    }

    /* Configure epoll event */
    watcher_data->epoll_event.events = events_to_epoll(watcher->events);
    watcher_data->epoll_event.data.ptr = watcher;

    /* Add to epoll */
    int result = epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, watcher->fd, &watcher_data->epoll_event);
    if (result < 0) {
        pd_set_system_error(loop, pd_get_current_system_error());
        free(watcher_data);
        return PD_ERR_SYSTEM;
    }

    watcher->platform_data = watcher_data;
    return PD_OK;
}

static int epoll_watcher_update(pd_watcher_t *watcher, pd_event_t events) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_epoll_data_t *data = (pd_epoll_data_t *)watcher->loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    pd_epoll_watcher_data_t *watcher_data = (pd_epoll_watcher_data_t *)watcher->platform_data;
    if (!watcher_data) {
        return PD_ERR_INVALID_ARG;
    }

    /* Update event configuration */
    watcher_data->epoll_event.events = events_to_epoll(events);

    /* Modify in epoll */
    int result = epoll_ctl(data->epoll_fd, EPOLL_CTL_MOD, watcher->fd, &watcher_data->epoll_event);
    if (result < 0) {
        pd_set_system_error(watcher->loop, pd_get_current_system_error());
        return PD_ERR_SYSTEM;
    }

    return PD_OK;
}

static int epoll_watcher_unregister(pd_watcher_t *watcher) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_epoll_data_t *data = (pd_epoll_data_t *)watcher->loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    /* Remove from epoll */
    int result = epoll_ctl(data->epoll_fd, EPOLL_CTL_DEL, watcher->fd, NULL);
    if (result < 0 && errno != EBADF) {
        /* EBADF is OK, fd was closed */
        pd_set_system_error(watcher->loop, pd_get_current_system_error());
        return PD_ERR_SYSTEM;
    }

    /* Free watcher platform data */
    free(watcher->platform_data);
    watcher->platform_data = NULL;

    return PD_OK;
}

static int epoll_async_send(pd_loop_t *loop, void *data) {
    /* For epoll, we could use eventfd or a pipe to wake up epoll_wait.
     * For simplicity, this implementation relies on the stop mechanism
     * and doesn't provide async wake-up from another thread.
     * A full implementation would create an eventfd on loop creation and
     * add it to the epoll set, then write to it here.
     */
    (void)data;
    return PD_ERR_NOT_IMPLEMENTED;
}

/* ============================================================================
 * Timer implementation using timerfd
 * ============================================================================ */

/**
 * Internal callback for timerfd watcher.
 * Reads the timerfd to acknowledge the firing, then invokes user callback.
 */
static void timerfd_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                             pd_event_t events, void *user_data) {
    (void)events;
    pd_timer_t *timer = (pd_timer_t *)user_data;
    if (!timer) {
        return;
    }

    /* Read the timerfd to acknowledge the event */
    int timerfd = watcher->fd;
    if (timerfd >= 0) {
        uint64_t expirations;
        ssize_t n = read(timerfd, &expirations, sizeof(expirations));
        (void)n;  /* Ignore read errors, timer still fires */
    }

    /* Invoke the user callback */
    if (timer->callback) {
        timer->callback(loop, watcher, PD_EVENT_READ, timer->user_data);
    }
}

static int epoll_timer_create(pd_loop_t *loop, pd_timer_t *timer) {
    if (!loop || !timer) {
        return PD_ERR_INVALID_ARG;
    }

    /* Create a timerfd */
    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timerfd < 0) {
        pd_set_system_error(loop, pd_get_current_system_error());
        return PD_ERR_SYSTEM;
    }

    /* Store the timerfd as platform data */
    int *timerfd_ptr = calloc(1, sizeof(int));
    if (!timerfd_ptr) {
        close(timerfd);
        return PD_ERR_NO_MEMORY;
    }
    *timerfd_ptr = timerfd;
    timer->platform_data = timerfd_ptr;

    /* Create an internal watcher for the timerfd.
     * The watcher's user_data points back to the timer so the callback
     * can bridge to the user's callback. */
    timer->watcher = pd_watcher_create(loop, timerfd, PD_EVENT_READ,
                                        timerfd_callback, timer);
    if (!timer->watcher) {
        close(timerfd);
        free(timerfd_ptr);
        timer->platform_data = NULL;
        return PD_ERR_SYSTEM;
    }

    /* The watcher was created via pd_watcher_create, so the timer owns it
     * and must call pd_watcher_destroy on cleanup. */
    timer->owns_watcher = 1;

    /* Stop the watcher immediately - it will be started when the timer starts */
    pd_watcher_stop(timer->watcher);

    return PD_OK;
}

static int epoll_timer_start(pd_timer_t *timer) {
    if (!timer || !timer->loop) {
        return PD_ERR_INVALID_ARG;
    }

    int timerfd = *(int *)timer->platform_data;
    if (timerfd < 0) {
        return PD_ERR_SYSTEM;
    }

    /* Convert timeout and interval to itimerspec */
    struct itimerspec its;
    memset(&its, 0, sizeof(its));

    if (timer->timeout_ms > 0) {
        its.it_value.tv_sec = timer->timeout_ms / 1000;
        its.it_value.tv_nsec = (timer->timeout_ms % 1000) * 1000000;
    } else if (timer->timeout_ms == 0) {
        /* Fire immediately: set to smallest non-zero value */
        its.it_value.tv_sec = 0;
        its.it_value.tv_nsec = 1;
    }

    if (timer->interval_ms > 0) {
        its.it_interval.tv_sec = timer->interval_ms / 1000;
        its.it_interval.tv_nsec = (timer->interval_ms % 1000) * 1000000;
    }
    /* If interval_ms == 0, it_interval is all zeros = one-shot */

    /* Arm the timer */
    int result = timerfd_settime(timerfd, 0, &its, NULL);
    if (result < 0) {
        pd_set_system_error(timer->loop, pd_get_current_system_error());
        return PD_ERR_SYSTEM;
    }

    /* Start the watcher */
    if (timer->watcher) {
        result = pd_watcher_start(timer->watcher);
        if (result != 0) {
            return result;
        }
    }

    return PD_OK;
}

static int epoll_timer_stop(pd_timer_t *timer) {
    if (!timer || !timer->loop) {
        return PD_ERR_INVALID_ARG;
    }

    int timerfd = *(int *)timer->platform_data;
    if (timerfd < 0) {
        return PD_ERR_SYSTEM;
    }

    /* Disarm the timer */
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    int result = timerfd_settime(timerfd, 0, &its, NULL);
    if (result < 0) {
        pd_set_system_error(timer->loop, pd_get_current_system_error());
        return PD_ERR_SYSTEM;
    }

    /* Stop the watcher */
    if (timer->watcher) {
        result = pd_watcher_stop(timer->watcher);
        if (result != 0) {
            return result;
        }
    }

    return PD_OK;
}

static void epoll_timer_destroy(pd_timer_t *timer) {
    if (!timer) {
        return;
    }

    /* Close the timerfd */
    if (timer->platform_data) {
        int *timerfd_ptr = (int *)timer->platform_data;
        if (*timerfd_ptr >= 0) {
            close(*timerfd_ptr);
        }
        free(timerfd_ptr);
        timer->platform_data = NULL;
    }
}