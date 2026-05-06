/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Windows I/O Completion Ports (IOCP) backend implementation.
 *
 * Note: IOCP is completion-based, not readiness-based like epoll/kqueue.
 * This requires a paradigm shift: when we register for READ, we must issue
 * an async read operation, and the callback is invoked when the read completes.
 */

#include "internal/platform.h"
#include "poll-dancer/poll-dancer.h"
#include "internal/loop.h"
#include "internal/watcher.h"
#include "internal/timer.h"
#include "internal/internal.h"

#ifdef PD_PLATFORM_WINDOWS

#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* Link with ws2_32 and mswsock libraries */
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

/**
 * Platform-specific data for IOCP.
 */
typedef struct {
    HANDLE iocp_handle;             /**< IOCP handle */
    OVERLAPPED_ENTRY *events;        /**< Event array for GetQueuedCompletionStatusEx */
    int max_events;                  /**< Maximum events per wait */
} pd_iocp_data_t;

/**
 * Platform-specific watcher data for IOCP.
 */
typedef struct {
    OVERLAPPED overlapped;          /**< Overlapped structure for async I/O */
    WSABUF wsa_buffer;              /**< Buffer for async operations */
    char buffer[4096];              /**< Default buffer for operations */
    int pending_operation;          /**< 0=none, 1=read, 2=write */
    pd_event_t events;              /**< Events being monitored */
} pd_iocp_watcher_data_t;

/* Forward declarations */
static int iocp_loop_create(pd_loop_t *loop, const pd_loop_config_t *config);
static void iocp_loop_destroy(pd_loop_t *loop);
static int iocp_loop_run(pd_loop_t *loop, int timeout_ms);
static int iocp_loop_stop(pd_loop_t *loop);
static int iocp_watcher_register(pd_loop_t *loop, pd_watcher_t *watcher);
static int iocp_watcher_update(pd_watcher_t *watcher, pd_event_t events);
static int iocp_watcher_unregister(pd_watcher_t *watcher);
static int iocp_async_send(pd_loop_t *loop, void *data);
static int iocp_timer_create(pd_loop_t *loop, pd_timer_t *timer);
static int iocp_timer_start(pd_timer_t *timer);
static int iocp_timer_stop(pd_timer_t *timer);
static void iocp_timer_destroy(pd_timer_t *timer);

/* Platform operations */
const pd_platform_ops_t pd_platform_iocp = {
    .loop_create = iocp_loop_create,
    .loop_destroy = iocp_loop_destroy,
    .loop_run = iocp_loop_run,
    .loop_stop = iocp_loop_stop,
    .watcher_register = iocp_watcher_register,
    .watcher_update = iocp_watcher_update,
    .watcher_unregister = iocp_watcher_unregister,
    .async_send = iocp_async_send,
    .timer_create = iocp_timer_create,
    .timer_start = iocp_timer_start,
    .timer_stop = iocp_timer_stop,
    .timer_destroy = iocp_timer_destroy,
    .name = "iocp",
    .max_events = 1024,
};

/**
 * Convert file descriptor to Windows handle.
 */
static HANDLE fd_to_handle(int fd) {
    return (HANDLE)_get_osfhandle(fd);
}

static int iocp_loop_create(pd_loop_t *loop, const pd_loop_config_t *config) {
    /* Allocate platform data */
    pd_iocp_data_t *data = calloc(1, sizeof(pd_iocp_data_t));
    if (!data) {
        pd_set_system_error(loop, GetLastError());
        return PD_ERR_NO_MEMORY;
    }

    /* Create IOCP handle */
    data->iocp_handle = CreateIoCompletionPort(
        INVALID_HANDLE_VALUE,  /* No file handle yet */
        NULL,                  /* Create new IOCP */
        0,                     /* No completion key */
        0                      /* Use default concurrency */
    );

    if (data->iocp_handle == NULL) {
        pd_set_system_error(loop, GetLastError());
        free(data);
        return PD_ERR_SYSTEM;
    }

    /* Allocate event array */
    data->max_events = config->max_events_per_wait;
    data->events = calloc(data->max_events, sizeof(OVERLAPPED_ENTRY));
    if (!data->events) {
        CloseHandle(data->iocp_handle);
        free(data);
        return PD_ERR_NO_MEMORY;
    }

    loop->platform_data = data;
    return PD_OK;
}

static void iocp_loop_destroy(pd_loop_t *loop) {
    if (!loop || !loop->platform_data) {
        return;
    }

    pd_iocp_data_t *data = (pd_iocp_data_t *)loop->platform_data;

    if (data->iocp_handle != NULL) {
        CloseHandle(data->iocp_handle);
    }

    free(data->events);
    free(data);
    loop->platform_data = NULL;
}

static int iocp_loop_run(pd_loop_t *loop, int timeout_ms) {
    if (!loop || !loop->platform_data) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_data_t *data = (pd_iocp_data_t *)loop->platform_data;

    /* Convert timeout to milliseconds for Windows */
    DWORD win_timeout = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;

    /* Wait for completion packets */
    ULONG num_entries = 0;
    BOOL success = GetQueuedCompletionStatusEx(
        data->iocp_handle,
        data->events,
        data->max_events,
        &num_entries,
        win_timeout,
        FALSE
    );

    if (!success) {
        DWORD error = GetLastError();
        if (error == WAIT_TIMEOUT) {
            return 0;  /* Timeout, no events */
        }
        pd_set_system_error(loop, error);
        return PD_ERR_SYSTEM;
    }

    /* Process completion packets */
    for (ULONG i = 0; i < num_entries; i++) {
        OVERLAPPED_ENTRY *entry = &data->events[i];

        /* Check if this is a timer completion (no overlapped structure) */
        if (entry->lpOverlapped == NULL && entry->lpCompletionKey != 0) {
            /* This could be a timer completion or a stop signal */
            pd_timer_t *timer = (pd_timer_t *)entry->lpCompletionKey;
            /* Verify this is actually a timer by checking if it has a callback */
            if (timer && timer->callback) {
                timer->callback(loop, timer->watcher, PD_EVENT_READ, timer->user_data);
            }
            continue;
        }

        pd_watcher_t *watcher = (pd_watcher_t *)entry->lpCompletionKey;

        if (!watcher || !watcher->callback) {
            continue;
        }

        /* Convert completion to event */
        pd_event_t events = PD_EVENT_NONE;

        /* Determine what event completed */
        pd_iocp_watcher_data_t *watcher_data = (pd_iocp_watcher_data_t *)watcher->platform_data;
        if (watcher_data) {
            if (watcher_data->pending_operation == 1) {
                events |= PD_EVENT_READ;
            } else if (watcher_data->pending_operation == 2) {
                events |= PD_EVENT_WRITE;
            }
            watcher_data->pending_operation = 0;
        }

        /* Invoke callback */
        watcher->callback(loop, watcher, events, watcher->user_data);

        /* Re-issue async operation for edge-triggered mode */
        /* TODO: Handle edge-triggered mode properly */
    }

    return (int)num_entries;
}

static int iocp_loop_stop(pd_loop_t *loop) {
    /* Post a special completion packet to wake up the loop */
    if (!loop || !loop->platform_data) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_data_t *data = (pd_iocp_data_t *)loop->platform_data;

    /* Post a completion packet with completion key 0 */
    BOOL success = PostQueuedCompletionStatus(
        data->iocp_handle,
        0,        /* Bytes transferred */
        0,        /* Completion key (NULL watcher) */
        NULL      /* Overlapped */
    );

    if (!success) {
        pd_set_system_error(loop, GetLastError());
        return PD_ERR_SYSTEM;
    }

    return PD_OK;
}

static int iocp_watcher_register(pd_loop_t *loop, pd_watcher_t *watcher) {
    if (!loop || !watcher) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_data_t *data = (pd_iocp_data_t *)loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    /* Allocate watcher platform data */
    pd_iocp_watcher_data_t *watcher_data = calloc(1, sizeof(pd_iocp_watcher_data_t));
    if (!watcher_data) {
        pd_set_system_error(loop, GetLastError());
        return PD_ERR_NO_MEMORY;
    }

    watcher_data->events = watcher->events;
    watcher_data->wsa_buffer.buf = watcher_data->buffer;
    watcher_data->wsa_buffer.len = sizeof(watcher_data->buffer);

    /* Associate file descriptor with IOCP */
    HANDLE handle = fd_to_handle(watcher->fd);
    HANDLE result = CreateIoCompletionPort(
        handle,
        data->iocp_handle,
        (ULONG_PTR)watcher,  /* Completion key is watcher pointer */
        0
    );

    if (result == NULL) {
        pd_set_system_error(loop, GetLastError());
        free(watcher_data);
        return PD_ERR_SYSTEM;
    }

    /* For READ monitoring, issue an async read */
    if (watcher->events & PD_EVENT_READ) {
        DWORD bytes_received = 0;
        DWORD flags = 0;
        memset(&watcher_data->overlapped, 0, sizeof(OVERLAPPED));

        int rc = WSARecv(
            watcher->fd,
            &watcher_data->wsa_buffer,
            1,
            &bytes_received,
            &flags,
            &watcher_data->overlapped,
            NULL
        );

        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            /* Error that isn't "operation pending" */
            pd_set_system_error(loop, WSAGetLastError());
            free(watcher_data);
            return PD_ERR_SYSTEM;
        }

        watcher_data->pending_operation = 1;  /* Read pending */
    }

    watcher->platform_data = watcher_data;
    return PD_OK;
}

static int iocp_watcher_update(pd_watcher_t *watcher, pd_event_t events) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_watcher_data_t *watcher_data = (pd_iocp_watcher_data_t *)watcher->platform_data;
    if (!watcher_data) {
        return PD_ERR_INVALID_ARG;
    }

    /* Cancel any pending operations */
    /* TODO: Implement proper cancellation */

    watcher_data->events = events;
    watcher->events = events;

    /* Re-issue operations for new event mask */
    if (events & PD_EVENT_READ && watcher_data->pending_operation == 0) {
        DWORD bytes_received = 0;
        DWORD flags = 0;
        memset(&watcher_data->overlapped, 0, sizeof(OVERLAPPED));

        int rc = WSARecv(
            watcher->fd,
            &watcher_data->wsa_buffer,
            1,
            &bytes_received,
            &flags,
            &watcher_data->overlapped,
            NULL
        );

        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            pd_set_system_error(watcher->loop, WSAGetLastError());
            return PD_ERR_SYSTEM;
        }

        watcher_data->pending_operation = 1;
    }

    return PD_OK;
}

static int iocp_watcher_unregister(pd_watcher_t *watcher) {
    if (!watcher || !watcher->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_watcher_data_t *watcher_data = (pd_iocp_watcher_data_t *)watcher->platform_data;
    if (!watcher_data) {
        return PD_ERR_INVALID_ARG;
    }

    /* Cancel any pending I/O operations */
    CancelIo(fd_to_handle(watcher->fd));

    /* Free watcher platform data */
    free(watcher->platform_data);
    watcher->platform_data = NULL;

    return PD_OK;
}

static int iocp_async_send(pd_loop_t *loop, void *data) {
    /* IOCP can wake up via PostQueuedCompletionStatus */
    return iocp_loop_stop(loop);
}

/* ============================================================================
 * Timer implementation using CreateTimerQueueTimer
 * ============================================================================ */

/**
 * Platform-specific timer data for IOCP timers.
 */
typedef struct {
    HANDLE queue_timer;    /**< Windows timer queue timer handle */
    HANDLE iocp_handle;    /**< IOCP handle for posting completions */
    int started;           /**< Non-zero if timer has been started */
} pd_iocp_timer_data_t;

/**
 * Windows timer callback. Runs on a thread pool thread.
 * Posts a completion to IOCP so the user callback runs on the loop thread.
 */
static VOID CALLBACK iocp_timer_callback(PVOID lpParam, BOOLEAN timer_or_wait_fired) {
    (void)timer_or_wait_fired;
    pd_timer_t *timer = (pd_timer_t *)lpParam;
    if (!timer || !timer->platform_data) {
        return;
    }

    pd_iocp_timer_data_t *timer_data = (pd_iocp_timer_data_t *)timer->platform_data;

    /* Post a completion packet to IOCP so the user callback runs on the loop thread */
    PostQueuedCompletionStatus(
        timer_data->iocp_handle,
        0,                        /* Bytes transferred */
        (ULONG_PTR)timer,         /* Completion key = timer pointer */
        NULL                      /* No overlapped */
    );
}

static int iocp_timer_create(pd_loop_t *loop, pd_timer_t *timer) {
    if (!loop || !timer) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_data_t *data = (pd_iocp_data_t *)loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    /* Allocate platform data */
    pd_iocp_timer_data_t *timer_data = calloc(1, sizeof(pd_iocp_timer_data_t));
    if (!timer_data) {
        pd_set_system_error(loop, GetLastError());
        return PD_ERR_NO_MEMORY;
    }

    timer_data->iocp_handle = data->iocp_handle;
    timer_data->queue_timer = NULL;
    timer_data->started = 0;
    timer->platform_data = timer_data;

    /* Create a placeholder watcher with fd = -1 */
    timer->watcher = pd_watcher_create(loop, -1, PD_EVENT_READ, NULL, timer);
    if (!timer->watcher) {
        free(timer_data);
        timer->platform_data = NULL;
        return PD_ERR_SYSTEM;
    }

    /* Stop the watcher immediately */
    pd_watcher_stop(timer->watcher);

    return PD_OK;
}

static int iocp_timer_start(pd_timer_t *timer) {
    if (!timer || !timer->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_timer_data_t *timer_data = (pd_iocp_timer_data_t *)timer->platform_data;
    if (!timer_data) {
        return PD_ERR_INVALID_ARG;
    }

    /* Create the timer queue timer */
    DWORD due_time = (DWORD)timer->timeout_ms;
    DWORD period = (DWORD)timer->interval_ms;

    BOOL success = CreateTimerQueueTimer(
        &timer_data->queue_timer,
        NULL,                     /* Default timer queue */
        iocp_timer_callback,      /* Callback */
        timer,                    /* Parameter */
        due_time,                 /* Initial delay */
        period,                   /* Period (0 = one-shot) */
        WT_EXECUTEINTIMERTHREAD   /* Execute in timer thread for low latency */
    );

    if (!success) {
        pd_set_system_error(timer->loop, GetLastError());
        return PD_ERR_SYSTEM;
    }

    timer_data->started = 1;
    return PD_OK;
}

static int iocp_timer_stop(pd_timer_t *timer) {
    if (!timer || !timer->loop) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_timer_data_t *timer_data = (pd_iocp_timer_data_t *)timer->platform_data;
    if (!timer_data || !timer_data->started) {
        return PD_OK;  /* Not started, nothing to do */
    }

    /* Delete the timer queue timer.
     * Use DeleteTimerQueueTimer with NULL completion event to avoid blocking. */
    BOOL success = DeleteTimerQueueTimer(
        NULL,                     /* Default timer queue */
        timer_data->queue_timer,
        NULL                      /* No completion event */
    );

    /* ERROR_IO_PENDING is expected when the timer is still pending */
    if (!success && GetLastError() != ERROR_IO_PENDING) {
        pd_set_system_error(timer->loop, GetLastError());
        return PD_ERR_SYSTEM;
    }

    timer_data->queue_timer = NULL;
    timer_data->started = 0;
    return PD_OK;
}

static void iocp_timer_destroy(pd_timer_t *timer) {
    if (!timer) {
        return;
    }

    /* Stop the timer if running */
    if (timer->platform_data) {
        pd_iocp_timer_data_t *timer_data = (pd_iocp_timer_data_t *)timer->platform_data;
        if (timer_data->started && timer_data->queue_timer) {
            DeleteTimerQueueTimer(NULL, timer_data->queue_timer, NULL);
        }
        free(timer_data);
        timer->platform_data = NULL;
    }
}

#else /* !PD_PLATFORM_WINDOWS */

/* Stub implementation for non-Windows platforms */
const pd_platform_ops_t pd_platform_iocp = {
    .loop_create = NULL,
    .loop_destroy = NULL,
    .loop_run = NULL,
    .loop_stop = NULL,
    .watcher_register = NULL,
    .watcher_update = NULL,
    .watcher_unregister = NULL,
    .async_send = NULL,
    .timer_create = NULL,
    .timer_start = NULL,
    .timer_stop = NULL,
    .timer_destroy = NULL,
    .name = "iocp",
    .max_events = 0,
};

#endif /* PD_PLATFORM_WINDOWS */