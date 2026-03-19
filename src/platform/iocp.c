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
#include "internal/loop.h"
#include "internal/watcher.h"
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
    .name = "iocp",
    .max_events = 0,
};

#endif /* PD_PLATFORM_WINDOWS */