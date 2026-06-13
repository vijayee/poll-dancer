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
 * Magic completion key for timer completions.
 * Distinctive value ("TIMR" in ASCII) so timer completions are not
 * confused with other completion types that happen to have
 * lpOverlapped == NULL.
 */
#define PD_IOCP_TIMER_KEY ((ULONG_PTR)0x54494D52)

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
static int iocp_watcher_register_handle(pd_loop_t *loop, pd_watcher_t *watcher);
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
    .watcher_register_handle = iocp_watcher_register_handle,
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

        /* Check if this is a timer completion (identified by magic key) */
        if (entry->lpCompletionKey == PD_IOCP_TIMER_KEY) {
            /* Timer pointer was passed via lpOverlapped */
            pd_timer_t *timer = (pd_timer_t *)entry->lpOverlapped;
            if (timer && timer->callback) {
                timer->callback(loop, timer->watcher, PD_EVENT_READ, timer->user_data);
            }
            continue;
        }

        /* Check for stop signal (completion key 0 with no overlapped) */
        if (entry->lpOverlapped == NULL && entry->lpCompletionKey == 0) {
            continue;
        }

        pd_watcher_t *watcher = (pd_watcher_t *)entry->lpCompletionKey;

        if (!watcher || !watcher->callback) {
            continue;
        }

        /* Check whether the I/O was cancelled (CancelIoEx, pipe close,
         * etc.) before reaching the user. Without this, a cancelled
         * read or write would invoke the callback with a HANGUP/ERROR
         * event that's really just our own teardown surfacing. */
        DWORD io_error = (DWORD)(ULONG_PTR)entry->lpOverlapped->Internal;
        if (entry->lpOverlapped->Internal == STATUS_CANCELLED ||
            io_error == ERROR_OPERATION_ABORTED) {
            /* Reset the pending-operation state for this watcher so a
             * subsequent watcher_update can re-issue. */
            pd_iocp_watcher_data_t *wd = (pd_iocp_watcher_data_t *)watcher->platform_data;
            if (wd) {
                wd->pending_operation = 0;
            }
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

    /* Cancel any pending I/O operations. Use CancelIoEx when we have a
     * real HANDLE (handle-watchers and any future fd-watchers that have
     * resolved to one); fall back to CancelIo for the legacy socket path
     * which still passes a CRT fd. */
    HANDLE handle = NULL;
#ifdef PD_PLATFORM_WINDOWS
    if (watcher->is_handle && watcher->handle) {
        handle = (HANDLE)watcher->handle;
        if (handle != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle, &watcher_data->overlapped);
        }
    } else if (watcher->fd >= 0) {
        HANDLE fd_handle = fd_to_handle(watcher->fd);
        if (fd_handle && fd_handle != INVALID_HANDLE_VALUE) {
            CancelIo(fd_handle);
        }
    }
#else
    (void)handle;
#endif

    /* Free watcher platform data */
    free(watcher->platform_data);
    watcher->platform_data = NULL;

    return PD_OK;
}

static int iocp_watcher_register_handle(pd_loop_t *loop, pd_watcher_t *watcher) {
    if (!loop || !watcher || !watcher->handle) {
        return PD_ERR_INVALID_ARG;
    }

    pd_iocp_data_t *data = (pd_iocp_data_t *)loop->platform_data;
    if (!data) {
        return PD_ERR_LOOP_CLOSED;
    }

    HANDLE handle = (HANDLE)watcher->handle;
    if (handle == INVALID_HANDLE_VALUE) {
        return PD_ERR_INVALID_ARG;
    }

    /* Allocate watcher platform data. We reuse the same struct as the
     * socket path so dispatch in iocp_loop_run doesn't need a separate
     * branch. The buffer is a fixed 4 KiB read buffer; callers that need
     * more control over the buffer can use the existing wsa_buffer
     * field, but for named pipes 4 KiB matches typical request sizes. */
    pd_iocp_watcher_data_t *watcher_data = calloc(1, sizeof(pd_iocp_watcher_data_t));
    if (!watcher_data) {
        pd_set_system_error(loop, GetLastError());
        return PD_ERR_NO_MEMORY;
    }

    watcher_data->events = watcher->events;
    watcher_data->wsa_buffer.buf = watcher_data->buffer;
    watcher_data->wsa_buffer.len = sizeof(watcher_data->buffer);

    /* Associate the HANDLE with the IOCP. The completion key is the
     * watcher pointer so the completion dispatch can recover it
     * without a separate lookup table. */
    HANDLE result = CreateIoCompletionPort(
        handle,
        data->iocp_handle,
        (ULONG_PTR)watcher,
        0
    );

    if (result == NULL) {
        DWORD err = GetLastError();
        pd_set_system_error(loop, err);
        free(watcher_data);
        return PD_ERR_SYSTEM;
    }

    /* For READ monitoring, issue an async read. ReadFile on an
     * overlapped handle returns FALSE with ERROR_IO_PENDING on success
     * (the "I/O is pending" return is the normal async path). */
    if (watcher->events & PD_EVENT_READ) {
        DWORD bytes_read = 0;
        memset(&watcher_data->overlapped, 0, sizeof(OVERLAPPED));

        BOOL ok = ReadFile(
            handle,
            watcher_data->buffer,
            sizeof(watcher_data->buffer),
            &bytes_read,
            &watcher_data->overlapped
        );

        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING && err != ERROR_MORE_DATA) {
                pd_set_system_error(loop, err);
                free(watcher_data);
                return PD_ERR_SYSTEM;
            }
        }

        watcher_data->pending_operation = 1;  /* Read pending */
    }

    watcher->platform_data = watcher_data;
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
    HANDLE stop_event;     /**< Event for safe timer stop/destroy synchronization */
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

    /* Post a completion packet to IOCP so the user callback runs on the loop thread.
     * Use PD_IOCP_TIMER_KEY as the completion key to identify this as a timer
     * completion, and pass the timer pointer via lpOverlapped (overloaded). */
    PostQueuedCompletionStatus(
        timer_data->iocp_handle,
        0,                        /* Bytes transferred */
        PD_IOCP_TIMER_KEY,        /* Completion key = timer magic tag */
        (OVERLAPPED *)timer       /* Timer pointer as overlapped (overloaded) */
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

    /* Create an event object for safe timer stop/destroy synchronization.
     * This allows DeleteTimerQueueTimer to signal when pending callbacks
     * have completed, preventing use-after-free on timer->platform_data. */
    timer_data->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (timer_data->stop_event == NULL) {
        pd_set_system_error(loop, GetLastError());
        free(timer_data);
        return PD_ERR_SYSTEM;
    }

    timer->platform_data = timer_data;

    /* Create a placeholder watcher that is NOT registered with IOCP.
     * IOCP timers post completion packets directly; no real file descriptor
     * is needed. The watcher exists only as a container for the timer's
     * callback and user_data that the event loop dispatches to when it
     * detects a timer completion. */
    pd_watcher_t *watcher = calloc(1, sizeof(pd_watcher_t));
    if (!watcher) {
        CloseHandle(timer_data->stop_event);
        free(timer_data);
        timer->platform_data = NULL;
        pd_set_system_error(loop, GetLastError());
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
    timer->owns_watcher = 0;  /* Manually allocated, not via pd_watcher_create */

    /* Add watcher to the loop's watcher list so the loop stays alive
     * when only timers are active (no regular I/O watchers). */
    int result = pd_loop_add_watcher(loop, watcher);
    if (result != 0) {
        free(watcher);
        CloseHandle(timer_data->stop_event);
        free(timer_data);
        timer->platform_data = NULL;
        timer->watcher = NULL;
        return result;
    }

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

    /* Reset the stop event before deleting the timer */
    ResetEvent(timer_data->stop_event);

    /* Delete the timer queue timer with a completion event.
     * Using the stop_event allows safe cancellation: the event is signaled
     * when all pending callbacks have completed, preventing use-after-free
     * on timer->platform_data. This also allows iocp_timer_stop to be
     * called from within the timer callback itself safely. */
    BOOL success = DeleteTimerQueueTimer(
        NULL,                     /* Default timer queue */
        timer_data->queue_timer,
        timer_data->stop_event    /* Signaled when pending callbacks complete */
    );

    /* ERROR_IO_PENDING is expected when the timer callback is still running */
    if (!success && GetLastError() != ERROR_IO_PENDING) {
        pd_set_system_error(timer->loop, GetLastError());
        return PD_ERR_SYSTEM;
    }

    /* Wait for the stop event to be signaled (callback has finished).
     * This ensures no callback is accessing timer->platform_data when we
     * return. The wait is bounded because the callback will complete. */
    WaitForSingleObject(timer_data->stop_event, 5000);

    timer_data->queue_timer = NULL;
    timer_data->started = 0;
    return PD_OK;
}

static void iocp_timer_destroy(pd_timer_t *timer) {
    if (!timer) {
        return;
    }

    /* Stop the timer if running, using INVALID_HANDLE_VALUE to wait for
     * any pending callback to complete before freeing resources. This
     * prevents use-after-free on timer->platform_data. */
    if (timer->platform_data) {
        pd_iocp_timer_data_t *timer_data = (pd_iocp_timer_data_t *)timer->platform_data;
        if (timer_data->started && timer_data->queue_timer) {
            DeleteTimerQueueTimer(NULL, timer_data->queue_timer, INVALID_HANDLE_VALUE);
            timer_data->queue_timer = NULL;
            timer_data->started = 0;
        }
        /* Close the stop event handle */
        if (timer_data->stop_event != NULL) {
            CloseHandle(timer_data->stop_event);
            timer_data->stop_event = NULL;
        }
        free(timer_data);
        timer->platform_data = NULL;
    }

    /* Remove the placeholder watcher from the loop's watcher list and free it.
     * This watcher was manually allocated (not via pd_watcher_create), so we
     * only need to remove it from the list and free it directly. */
    if (timer->watcher) {
        if (timer->loop) {
            pd_loop_remove_watcher(timer->loop, timer->watcher);
        }
        free(timer->watcher);
        timer->watcher = NULL;
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
    .watcher_register_handle = NULL,
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