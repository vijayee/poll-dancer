/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Watcher implementation.
 */

#include "poll-dancer/poll-dancer.h"
#include "internal/watcher.h"
#include "internal/loop.h"
#include "internal/platform.h"

#include <stdlib.h>
#include <string.h>

pd_watcher_t *pd_watcher_create(pd_loop_t *loop,
                                 int fd,
                                 pd_event_t events,
                                 pd_callback_t callback,
                                 void *user_data) {
    if (!loop) {
        return NULL;
    }

    /* Allocate watcher structure */
    pd_watcher_t *watcher = calloc(1, sizeof(pd_watcher_t));
    if (!watcher) {
        return NULL;
    }

    /* Initialize watcher */
    if (pd_watcher_init(watcher, loop, fd, events, callback, user_data) != 0) {
        free(watcher);
        return NULL;
    }

    return watcher;
}

int pd_watcher_init(pd_watcher_t *watcher,
                    pd_loop_t *loop,
                    int fd,
                    pd_event_t events,
                    pd_callback_t callback,
                    void *user_data) {
    if (!watcher || !loop) {
        return PD_ERR_INVALID_ARG;
    }

    watcher->fd = fd;
    watcher->events = events;
    watcher->callback = callback;
    watcher->user_data = user_data;
    watcher->loop = loop;
    watcher->active = 1;
    watcher->ref_count = 1;

    /* Register with platform backend */
    if (loop->ops && loop->ops->watcher_register) {
        int result = loop->ops->watcher_register(loop, watcher);
        if (result != 0) {
            return result;
        }
    }

    /* Add to loop's watcher list */
    int result = pd_loop_add_watcher(loop, watcher);
    if (result != 0) {
        /* Unregister from platform backend */
        if (loop->ops && loop->ops->watcher_unregister) {
            loop->ops->watcher_unregister(watcher);
        }
        return result;
    }

    return PD_OK;
}

void pd_watcher_cleanup(pd_watcher_t *watcher) {
    if (!watcher) {
        return;
    }

    /* Unregister from platform backend */
    if (watcher->loop && watcher->loop->ops && watcher->loop->ops->watcher_unregister) {
        watcher->loop->ops->watcher_unregister(watcher);
    }

    /* Remove from loop's watcher list */
    if (watcher->loop) {
        pd_loop_remove_watcher(watcher->loop, watcher);
    }
}

int pd_watcher_update(pd_watcher_t *watcher, pd_event_t events) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    if (!watcher->active) {
        return PD_ERR_WATCHER_STOPPED;
    }

    watcher->events = events;

    /* Update platform backend */
    if (watcher->loop->ops && watcher->loop->ops->watcher_update) {
        return watcher->loop->ops->watcher_update(watcher, events);
    }

    return PD_OK;
}

int pd_watcher_stop(pd_watcher_t *watcher) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    if (!watcher->active) {
        return PD_OK;  /* Already stopped */
    }

    watcher->active = 0;

    /* Unregister from platform backend */
    if (watcher->loop->ops && watcher->loop->ops->watcher_unregister) {
        int result = watcher->loop->ops->watcher_unregister(watcher);
        if (result != 0) {
            watcher->active = 1;  /* Restore state on failure */
            return result;
        }
    }

    return PD_OK;
}

int pd_watcher_start(pd_watcher_t *watcher) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    if (watcher->active) {
        return PD_OK;  /* Already started */
    }

    watcher->active = 1;

    /* Register with platform backend */
    if (watcher->loop->ops && watcher->loop->ops->watcher_register) {
        int result = watcher->loop->ops->watcher_register(watcher->loop, watcher);
        if (result != 0) {
            watcher->active = 0;  /* Restore state on failure */
            return result;
        }
    }

    return PD_OK;
}

void pd_watcher_destroy(pd_watcher_t *watcher) {
    if (!watcher) {
        return;
    }

    /* Stop and cleanup */
    pd_watcher_stop(watcher);
    pd_watcher_cleanup(watcher);

    free(watcher);
}

int pd_watcher_get_fd(pd_watcher_t *watcher) {
    if (!watcher) {
        return -1;
    }
    return watcher->fd;
}

pd_event_t pd_watcher_get_events(pd_watcher_t *watcher) {
    if (!watcher) {
        return PD_EVENT_NONE;
    }
    return watcher->events;
}