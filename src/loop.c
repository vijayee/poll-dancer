/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Event loop implementation.
 */

#include "poll-dancer/poll-dancer.h"
#include "internal/loop.h"
#include "internal/watcher.h"
#include "internal/platform.h"

#include <stdlib.h>
#include <string.h>

/* Default configuration */
#define DEFAULT_MAX_EVENTS_PER_WAIT 64
#define DEFAULT_THREAD_SAFETY 1

/* Global default loop */
static pd_loop_t *default_loop = NULL;
#ifdef PD_THREAD_SAFE
static pthread_mutex_t default_loop_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/**
 * Initialize loop configuration with defaults.
 */
static void init_config_defaults(pd_loop_config_t *config) {
    if (!config) {
        return;
    }

    if (config->max_events_per_wait <= 0) {
        config->max_events_per_wait = DEFAULT_MAX_EVENTS_PER_WAIT;
    }
    if (config->enable_thread_safety < 0) {
        config->enable_thread_safety = DEFAULT_THREAD_SAFETY;
    }
}

pd_loop_t *pd_loop_create(const pd_loop_config_t *config) {
    /* Allocate loop structure */
    pd_loop_t *loop = calloc(1, sizeof(pd_loop_t));
    if (!loop) {
        return NULL;
    }

    /* Copy and initialize configuration */
    pd_loop_config_t config_copy;
    if (config) {
        config_copy = *config;
    } else {
        memset(&config_copy, 0, sizeof(config_copy));
    }
    init_config_defaults(&config_copy);

    /* Initialize loop */
    loop->max_events_per_wait = config_copy.max_events_per_wait;
    loop->enable_thread_safety = config_copy.enable_thread_safety;
    loop->ref_count = 1;
    loop->running = 0;
    loop->stop_requested = 0;
    loop->system_error = 0;

    /* Initialize mutex if thread-safe */
    if (loop->enable_thread_safety) {
        if (PD_MUTEX_INIT(loop->mutex) != 0) {
            free(loop);
            return NULL;
        }
    }

    /* Detect platform */
    loop->ops = pd_platform_detect();
    if (!loop->ops) {
        if (loop->enable_thread_safety) {
            PD_MUTEX_DESTROY(loop->mutex);
        }
        free(loop);
        return NULL;
    }

    /* Initialize watchers array */
    loop->watcher_capacity = 16;
    loop->watchers = malloc(loop->watcher_capacity * sizeof(pd_watcher_t *));
    if (!loop->watchers) {
        if (loop->enable_thread_safety) {
            PD_MUTEX_DESTROY(loop->mutex);
        }
        free(loop);
        return NULL;
    }

    /* Call platform-specific initialization */
    if (loop->ops->loop_create(loop, &config_copy) != 0) {
        free(loop->watchers);
        if (loop->enable_thread_safety) {
            PD_MUTEX_DESTROY(loop->mutex);
        }
        free(loop);
        return NULL;
    }

    return loop;
}

pd_loop_t *pd_loop_default(void) {
#ifdef PD_THREAD_SAFE
    pthread_mutex_lock(&default_loop_mutex);
#endif

    if (!default_loop) {
        default_loop = pd_loop_create(NULL);
        if (default_loop) {
            /* Increment ref count so it's not destroyed when user calls unref */
            default_loop->ref_count++;
        }
    }

#ifdef PD_THREAD_SAFE
    pthread_mutex_unlock(&default_loop_mutex);
#endif

    return default_loop;
}

void pd_loop_destroy(pd_loop_t *loop) {
    if (!loop) {
        return;
    }

    pd_loop_unref(loop);
}

void pd_loop_ref(pd_loop_t *loop) {
    if (!loop) {
        return;
    }

    if (loop->enable_thread_safety) {
        PD_MUTEX_LOCK(loop->mutex);
    }

    loop->ref_count++;

    if (loop->enable_thread_safety) {
        PD_MUTEX_UNLOCK(loop->mutex);
    }
}

void pd_loop_unref(pd_loop_t *loop) {
    if (!loop) {
        return;
    }

    int should_destroy = 0;

    if (loop->enable_thread_safety) {
        PD_MUTEX_LOCK(loop->mutex);
    }

    loop->ref_count--;
    should_destroy = (loop->ref_count == 0);

    if (loop->enable_thread_safety) {
        PD_MUTEX_UNLOCK(loop->mutex);
    }

    if (should_destroy) {
        /* Destroy all watchers */
        for (size_t i = 0; i < loop->watcher_count; i++) {
            if (loop->watchers[i]) {
                pd_watcher_destroy(loop->watchers[i]);
            }
        }
        free(loop->watchers);

        /* Call platform-specific cleanup */
        if (loop->ops && loop->ops->loop_destroy) {
            loop->ops->loop_destroy(loop);
        }

        /* Destroy mutex */
        if (loop->enable_thread_safety) {
            PD_MUTEX_DESTROY(loop->mutex);
        }

        free(loop);
    }
}

int pd_loop_run(pd_loop_t *loop) {
    if (!loop) {
        return PD_ERR_INVALID_ARG;
    }

    loop->running = 1;
    loop->stop_requested = 0;

    while (!loop->stop_requested && loop->watcher_count > 0) {
        int result = pd_loop_run_once(loop, -1);
        if (result < 0) {
            loop->running = 0;
            return result;
        }
    }

    loop->running = 0;
    return PD_OK;
}

int pd_loop_run_once(pd_loop_t *loop, int timeout_ms) {
    if (!loop || !loop->ops) {
        return PD_ERR_INVALID_ARG;
    }

    if (loop->enable_thread_safety) {
        PD_MUTEX_LOCK(loop->mutex);
    }

    int result = loop->ops->loop_run(loop, timeout_ms);

    if (loop->enable_thread_safety) {
        PD_MUTEX_UNLOCK(loop->mutex);
    }

    return result;
}

int pd_loop_stop(pd_loop_t *loop) {
    if (!loop) {
        return PD_ERR_INVALID_ARG;
    }

    if (loop->enable_thread_safety) {
        PD_MUTEX_LOCK(loop->mutex);
    }

    loop->stop_requested = 1;

    if (loop->enable_thread_safety) {
        PD_MUTEX_UNLOCK(loop->mutex);
    }

    /* Wake up the loop if it's waiting */
    if (loop->ops && loop->ops->async_send) {
        loop->ops->async_send(loop, NULL);
    }

    return PD_OK;
}

int pd_loop_async_send(pd_loop_t *loop, void *data) {
    if (!loop || !loop->ops) {
        return PD_ERR_INVALID_ARG;
    }

    if (!loop->ops->async_send) {
        return PD_ERR_NOT_IMPLEMENTED;
    }

    return loop->ops->async_send(loop, data);
}

/**
 * Add a watcher to the loop's watcher list.
 *
 * @param loop The loop
 * @param watcher The watcher to add
 * @return 0 on success, negative error code on failure
 */
int pd_loop_add_watcher(pd_loop_t *loop, pd_watcher_t *watcher) {
    if (!loop || !watcher) {
        return PD_ERR_INVALID_ARG;
    }

    if (loop->enable_thread_safety) {
        PD_MUTEX_LOCK(loop->mutex);
    }

    /* Grow array if needed */
    if (loop->watcher_count >= loop->watcher_capacity) {
        size_t new_capacity = loop->watcher_capacity * 2;
        pd_watcher_t **new_watchers = realloc(loop->watchers,
                                               new_capacity * sizeof(pd_watcher_t *));
        if (!new_watchers) {
            if (loop->enable_thread_safety) {
                PD_MUTEX_UNLOCK(loop->mutex);
            }
            return PD_ERR_NO_MEMORY;
        }
        loop->watchers = new_watchers;
        loop->watcher_capacity = new_capacity;
    }

    loop->watchers[loop->watcher_count++] = watcher;

    if (loop->enable_thread_safety) {
        PD_MUTEX_UNLOCK(loop->mutex);
    }

    return PD_OK;
}

/**
 * Remove a watcher from the loop's watcher list.
 *
 * @param loop The loop
 * @param watcher The watcher to remove
 * @return 0 on success, negative error code on failure
 */
int pd_loop_remove_watcher(pd_loop_t *loop, pd_watcher_t *watcher) {
    if (!loop || !watcher) {
        return PD_ERR_INVALID_ARG;
    }

    if (loop->enable_thread_safety) {
        PD_MUTEX_LOCK(loop->mutex);
    }

    /* Find and remove watcher */
    for (size_t i = 0; i < loop->watcher_count; i++) {
        if (loop->watchers[i] == watcher) {
            /* Shift remaining watchers */
            for (size_t j = i; j < loop->watcher_count - 1; j++) {
                loop->watchers[j] = loop->watchers[j + 1];
            }
            loop->watcher_count--;
            if (loop->enable_thread_safety) {
                PD_MUTEX_UNLOCK(loop->mutex);
            }
            return PD_OK;
        }
    }

    if (loop->enable_thread_safety) {
        PD_MUTEX_UNLOCK(loop->mutex);
    }

    return PD_ERR_NOT_FOUND;
}