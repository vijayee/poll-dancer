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

/* STATUS_CANCELLED is defined in <ntstatus.h>, which conflicts with the
 * regular Win32 headers (it #define's its own error codes). Define the
 * values inline so we can check the Internal field of an OVERLAPPED
 * without pulling in the full NT status surface. */
#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED        ((DWORD)0xC0000120L)
#endif
/* NT status codes used to classify read completions into READ / HANGUP /
 * ERROR. STATUS_SUCCESS with >0 bytes is data; STATUS_SUCCESS with 0 bytes
 * is a clean end-of-stream (peer closed); any failure status (connection
 * reset, pipe broken, etc.) is an error. */
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS          ((DWORD)0x00000000L)
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE       ((DWORD)0xC0000011L)
#endif
#ifndef STATUS_CONNECTION_RESET
#define STATUS_CONNECTION_RESET  ((DWORD)0xC000020DL)
#endif
#ifndef STATUS_LOCAL_DISCONNECT
#define STATUS_LOCAL_DISCONNECT  ((DWORD)0xC000020EL)
#endif
#ifndef STATUS_NETNAME_DELETED
#define STATUS_NETNAME_DELETED   ((DWORD)0xC0000034L)
#endif
#ifndef STATUS_PIPE_BROKEN
#define STATUS_PIPE_BROKEN       ((DWORD)0xC000014BL)
#endif
#ifndef STATUS_PORT_DISCONNECTED
#define STATUS_PORT_DISCONNECTED ((DWORD)0xC0000037L)
#endif

/**
 * Magic completion key for timer completions.
 * Distinctive value ("TIMR" in ASCII) so timer completions are not
 * confused with other completion types that happen to have
 * lpOverlapped == NULL.
 */
#define PD_IOCP_TIMER_KEY ((ULONG_PTR)0x54494D52)

/**
 * Magic completion key for drain-sync completions. The dispatch path posts
 * one of these after it has finished mutating watcher state (e.g. just
 * destroyed a timer) and waits for the loop thread to process it. The wait
 * guarantees that the loop has already consumed any in-flight completion
 * packets the dispatcher cares about (e.g. a timer's final completion,
 * whose lpOverlapped is the now-freed pd_timer_t*).
 *
 * lpOverlapped carries a pd_iocp_sync_t* describing what the loop thread
 * should do (typically: signal an event the dispatcher is waiting on).
 */
#define PD_IOCP_SYNC_KEY ((ULONG_PTR)0x53594E43)

/**
 * Platform-specific data for IOCP.
 */
typedef struct {
    HANDLE iocp_handle;             /**< IOCP handle */
    OVERLAPPED_ENTRY *events;        /**< Event array for GetQueuedCompletionStatusEx */
    int max_events;                  /**< Maximum events per wait */
    /* Thread id of the loop thread (the thread running iocp_loop_run). Set
     * lazily on each iocp_loop_run entry; the single loop thread is the only
     * writer and the value is idempotent. iocp_watcher_unregister reads it
     * to detect a call from the loop thread itself, in which case it must
     * NOT block on iocp_drain_sync (the loop thread is here, not running the
     * IOCP wait, so the sync completion would never be processed -> deadlock).
     * A value of 0 means the loop has not run yet, treated as "not the loop
     * thread" by unregister. */
    DWORD loop_thread_id;
} pd_iocp_data_t;

/**
 * Payload for a drain-sync completion. The dispatcher fills in `event`,
 * posts the completion, and blocks on the event. The loop thread, when it
 * processes the completion, sets the event and returns. The blocking
 * dispatcher then knows the loop has caught up.
 */
typedef struct {
    HANDLE event;                    /**< Signaled by loop thread after processing */
} pd_iocp_sync_t;

/**
 * Platform-specific timer data for IOCP timers. Defined here (rather than
 * near the iocp_timer_* implementations) so iocp_loop_run can reference
 * the `destroyed` flag while processing completion packets — see the
 * PD_IOCP_TIMER_KEY branch in iocp_loop_run.
 */
typedef struct {
    HANDLE queue_timer;    /**< Windows timer queue timer handle */
    HANDLE iocp_handle;    /**< IOCP handle for posting completions */
    HANDLE stop_event;     /**< Event for safe timer stop/destroy synchronization */
    int started;           /**< Non-zero if timer has been started */
    /* Set non-zero by iocp_timer_destroy under the timer_actor's loop_lock
     * once destroy begins. The Windows thread-pool callback and the loop's
     * completion handler both check this flag and skip processing if set,
     * providing a second line of defense against use-after-free in the
     * brief window where the iocp_drain_sync has not yet caught the
     * completion. The destroy path then waits for the loop to drain via
     * iocp_drain_sync before freeing anything. */
    volatile int destroyed;
} pd_iocp_timer_data_t;

/**
 * Platform-specific watcher data for IOCP.
 */
typedef struct {
    OVERLAPPED overlapped;          /**< Overlapped structure for async I/O */
    WSABUF wsa_buffer;              /**< Buffer for async operations */
    char buffer[4096];              /**< Default buffer for operations */
    int pending_operation;          /**< 0=none, 1=read in flight */
    pd_event_t events;              /**< Events being monitored */
    /* Set non-zero by iocp_watcher_unregister when tearing down a watcher
     * that has a read in flight (pending_operation == 1). The loop's
     * dispatch observes it on the final completion for this watcher's
     * overlapped and frees this struct (and watcher->platform_data) at that
     * point, instead of unregister freeing it synchronously while the kernel
     * still holds a pointer to the embedded OVERLAPPED. */
    volatile int stopping;
    /* Completion state for the most recent async read. Populated by
     * iocp_loop_run from entry->dwNumberOfBytesTransferred. The user
     * callback drains this via pd_watcher_drain_read, after which the
     * backend re-issues the async read so the next completion can fire. */
    DWORD bytes_available;          /**< Bytes pending in `buffer` (0 if no read completion) */
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
static size_t iocp_watcher_drain_read(pd_watcher_t *watcher, void *buf, size_t len);
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
    .watcher_drain_read = iocp_watcher_drain_read,
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
 *
 * poll-dancer callers pass a Winsock SOCKET (a raw kernel handle, NOT a CRT
 * file descriptor) in watcher->fd. _get_osfhandle() expects a CRT fd from
 * _open/open; calling it on a SOCKET value triggers a debug heap check that
 * aborts the process with STATUS_STACK_BUFFER_OVERRUN (0xC0000409). A SOCKET
 * is already usable as a HANDLE for CreateIoCompletionPort/ReadFile/CancelIoEx
 * (Winsock sockets and file handles share the same HANDLE namespace), and
 * SOCKET values fit in 32 bits in practice, so cast directly.
 */
static HANDLE fd_to_handle(int fd) {
    return (HANDLE)(SOCKET)(uintptr_t)(unsigned int)fd;
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

    /* Record the loop thread so iocp_watcher_unregister can detect calls
     * coming from this thread and avoid a deadlocking iocp_drain_sync. */
    data->loop_thread_id = GetCurrentThreadId();

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

        /* Drain-sync completion: signal the event the dispatcher is
         * blocked on. By FIFO ordering on the IOCP, every earlier
         * completion packet the dispatcher cared about has already been
         * handled by the time we get here, so the dispatcher can now
         * safely free any resources those packets referenced. */
        if (entry->lpCompletionKey == PD_IOCP_SYNC_KEY) {
            pd_iocp_sync_t *sync = (pd_iocp_sync_t *)entry->lpOverlapped;
            if (sync != NULL && sync->event != NULL) {
                SetEvent(sync->event);
            }
            continue;
        }

        /* Check if this is a timer completion (identified by magic key) */
        if (entry->lpCompletionKey == PD_IOCP_TIMER_KEY) {
            /* Timer pointer was passed via lpOverlapped */
            pd_timer_t *timer = (pd_timer_t *)entry->lpOverlapped;
            if (timer && timer->callback) {
                /* The timer's destroy path sets timer->platform_data->destroyed
                 * before calling iocp_drain_sync. If we observe the flag set,
                 * skip invoking the user callback — the timer is on its way out
                 * and may already be partially torn down. This pairs with the
                 * check in iocp_timer_callback and the iocp_drain_sync barrier
                 * to keep the user callback from running on a freed timer. */
                pd_iocp_timer_data_t *td =
                    (pd_iocp_timer_data_t *)timer->platform_data;
                if (td == NULL || td->destroyed) {
                    continue;
                }
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

        /* Teardown path: iocp_watcher_unregister set `stopping` on a watcher
         * that had a read in flight and could not free this struct
         * synchronously (the kernel still holds a pointer to the embedded
         * OVERLAPPED). This is the final completion for that overlapped —
         * either the CancelIoEx abort, or a normal read that won the cancel
         * race. Either way the kernel is now done with the OVERLAPPED, so it
         * is safe to free. Skip the user callback and any re-arm; the watcher
         * is already stopped. This check runs before the cancel filter below
         * so the filter never dereferences a freed lpOverlapped->Internal. */
        pd_iocp_watcher_data_t *stop_wd = (pd_iocp_watcher_data_t *)watcher->platform_data;
        if (stop_wd && stop_wd->stopping) {
            stop_wd->pending_operation = 0;
            free(stop_wd);
            watcher->platform_data = NULL;
            continue;
        }

        /* Write-completion demux. A completion whose lpOverlapped is not the
         * backend's read overlapped (&watcher_data->overlapped) is a write the
         * CALLER issued directly on this IOCP-bound handle (WriteFile/WSASend
         * with the caller's own OVERLAPPED). The backend never issues
         * overlapped writes itself, so any non-read overlapped is a client
         * write. Deliver it as PD_EVENT_WRITE; the caller reads bytes/status
         * via GetOverlappedResult on its own OVERLAPPED. Do NOT touch
         * pending_operation (a read may still be in flight on the read
         * overlapped) and do NOT re-arm a read below — a write completion must
         * never disturb read state.
         *
         * This runs before the cancel filter below so a cancelled write is
         * reported as WRITE (the caller's GetOverlappedResult surfaces the
         * error) rather than being swallowed and clobbering the read's
         * pending_operation. Callers that issue overlapped writes must drain
         * them before stopping the watcher: a write completion arriving once
         * `stopping` is set is dropped by the stopping-check above (the
         * caller's wait then times out). */
        pd_iocp_watcher_data_t *write_wd =
            (pd_iocp_watcher_data_t *)watcher->platform_data;
        if (write_wd && entry->lpOverlapped != &write_wd->overlapped) {
            watcher->callback(loop, watcher, PD_EVENT_WRITE, watcher->user_data);
            continue;
        }

        /* Check whether the I/O was cancelled (CancelIoEx, pipe close,
         * etc.) before reaching the user. Only read completions reach here:
         * client-issued write completions are handled by the demux above. A
         * cancelled read would otherwise invoke the callback with a
         * HANGUP/ERROR event that's really just our own teardown surfacing. */
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
        DWORD bytes_completed = entry->dwNumberOfBytesTransferred;

        /* Determine what event completed. `io_error` is the NT status from
         * the overlapped's Internal field (read above the cancel filter).
         * Classify read completions by status + byte count so EOF and
         * errored reads reach the user as HANGUP/ERROR rather than a bare
         * READ with 0 bytes. */
        pd_iocp_watcher_data_t *watcher_data = (pd_iocp_watcher_data_t *)watcher->platform_data;
        if (watcher_data) {
            if (watcher_data->pending_operation == 1) {
                if (io_error == STATUS_SUCCESS) {
                    if (bytes_completed > 0) {
                        events |= PD_EVENT_READ;
                        /* Stash the read bytes so the user callback can
                         * drain them via pd_watcher_drain_read. The kernel
                         * has already delivered them into buffer. */
                        watcher_data->bytes_available = bytes_completed;
                    } else {
                        /* 0 bytes with STATUS_SUCCESS: clean end-of-stream
                         * (the peer closed). Report HANGUP so the user's
                         * disconnect path runs; do not set bytes_available
                         * (nothing to drain). */
                        events |= PD_EVENT_HANGUP;
                    }
                } else {
                    /* Any failure NT status (connection reset, pipe broken,
                     * netname deleted, etc.): report ERROR. */
                    events |= PD_EVENT_ERROR;
                }
            }
            /* pending_operation tracks reads only; write completions are
             * demuxed by overlapped pointer above and never reach here. */
            watcher_data->pending_operation = 0;
        }

        /* Invoke callback. The callback may drain the read buffer and
         * re-register a fresh async read via pd_watcher_update, or it
         * may leave the buffer in place; the unconditional re-issue
         * below covers the common case. */
        watcher->callback(loop, watcher, events, watcher->user_data);

        /* For READ: re-issue the async read on the watcher_data's buffer
         * so the next completion can fire. We do this after the callback
         * to preserve edge-triggered semantics: the callback should
         * drain every byte before we accept more.
         *
         * Re-read platform_data here: the callback may have stopped the
         * watcher (e.g. its HANGUP path), which frees platform_data and
         * NULLs watcher->platform_data. The `watcher_data` local captured
         * above would then be a dangling pointer, so re-bind to the live
         * value before any dereference.
         *
         * Do NOT re-issue when bytes_completed == 0: a zero-byte completion
         * on a stream socket/handle is end-of-stream (the peer closed).
         * Re-issuing WSARecv/ReadFile on an EOF'd socket completes
         * synchronously with 0 bytes and immediately posts another
         * completion, which we would re-issue again — a tight busy-loop
         * that floods the connection actor with HANGUP messages until the
         * pool worker's CancelIoEx catches up. Letting the EOF completion
         * through without re-arm lets the user callback's HANGUP path
         * stop the watcher; a later pd_watcher_update can re-arm if the
         * connection is somehow reused. */
        watcher_data = (pd_iocp_watcher_data_t *)watcher->platform_data;
        /* Re-arm a READ only. Write completions `continue` in the demux above
         * and never reach here, so this block never re-issues a read on top of
         * a still-pending read or disturbs a concurrent write's overlapped. */
        if (watcher_data && (events & PD_EVENT_READ) && bytes_completed > 0 &&
            !watcher_data->stopping) {
            DWORD bytes_read = 0;
            memset(&watcher_data->overlapped, 0, sizeof(OVERLAPPED));
            int issued_ok = 0;
            if (watcher->is_handle) {
                /* ReadFile on an overlapped handle returns FALSE with
                 * ERROR_IO_PENDING on success (async path); that is the
                 * normal case, not a failure. Treat any return value as
                 * a successful re-issue unless GetLastError says
                 * something other than IO_PENDING / MORE_DATA. */
                BOOL ok = ReadFile(
                    (HANDLE)watcher->handle,
                    watcher_data->buffer,
                    sizeof(watcher_data->buffer),
                    &bytes_read,
                    &watcher_data->overlapped
                );
                if (ok) {
                    issued_ok = 1;
                } else {
                    DWORD err = GetLastError();
                    if (err == ERROR_IO_PENDING || err == ERROR_MORE_DATA) {
                        issued_ok = 1;
                    }
                }
            } else {
                DWORD flags = 0;
                int rc = WSARecv(
                    (SOCKET)(uintptr_t)(unsigned int)watcher->fd,
                    &watcher_data->wsa_buffer,
                    1,
                    &bytes_read,
                    &flags,
                    &watcher_data->overlapped,
                    NULL
                );
                issued_ok = (rc == 0) || (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING);
            }
            if (issued_ok) {
                watcher_data->pending_operation = 1;
            }
            /* On error, leave pending_operation = 0; pd_watcher_update
             * can re-issue. */
        }
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

/**
 * Block until the loop thread has processed all completion packets that
 * were already in flight at the time of the call. Used by the timer
 * destroy path to ensure any pending timer completion (whose lpOverlapped
 * is the soon-to-be-freed pd_timer_t*) has been consumed by the loop
 * before the timer is freed.
 *
 * The mechanism: allocate a wait event, post a sync completion carrying
 * a pointer to the event as lpOverlapped, then WaitForSingleObject on
 * it. The loop, on consuming the sync completion, sets the event. Since
 * the IOCP preserves FIFO ordering for PostQueuedCompletionStatus packets
 * posted to the same handle, every completion the dispatcher cared about
 * has been processed by the time the event fires.
 */
static void iocp_drain_sync(pd_loop_t *loop) {
    if (!loop || !loop->platform_data) {
        return;
    }
    pd_iocp_data_t *data = (pd_iocp_data_t *)loop->platform_data;
    pd_iocp_sync_t sync = {0};
    sync.event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (sync.event == NULL) {
        return;
    }
    /* Post AFTER any earlier completion packets the loop might still be
     * holding in its `events` buffer or processing. FIFO ordering
     * guarantees the loop won't see this sync packet until those earlier
     * ones are handled. */
    PostQueuedCompletionStatus(
        data->iocp_handle,
        0,
        PD_IOCP_SYNC_KEY,
        (OVERLAPPED *)&sync
    );
    WaitForSingleObject(sync.event, 5000);
    CloseHandle(sync.event);
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
            (SOCKET)(uintptr_t)(unsigned int)watcher->fd,
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

    /* Cancel any in-flight read whose new event mask no longer requests
     * PD_EVENT_READ. CancelIoEx posts a completion with
     * ERROR_OPERATION_ABORTED, which iocp_loop_run already filters out
     * (and resets pending_operation to 0). For the socket path, the
     * legacy CancelIo behaves the same way. We don't synchronously
     * wait for the cancellation to land; the next read re-issue will
     * observe pending_operation == 0 (or the cancelled completion will
     * have cleared it by then). */
    if (watcher_data->pending_operation == 1 && !(events & PD_EVENT_READ)) {
        HANDLE cancel_handle = NULL;
        if (watcher->is_handle && watcher->handle &&
                watcher->handle != INVALID_HANDLE_VALUE) {
            cancel_handle = (HANDLE)watcher->handle;
        } else if (watcher->fd >= 0) {
            cancel_handle = fd_to_handle(watcher->fd);
        }
        if (cancel_handle && cancel_handle != INVALID_HANDLE_VALUE) {
            if (watcher->is_handle) {
                CancelIoEx(cancel_handle, &watcher_data->overlapped);
            } else {
                CancelIo(cancel_handle);
            }
        }
        /* Don't clear pending_operation here: iocp_loop_run clears it
         * when the cancelled completion is dequeued. Until then, the
         * re-issue branch below treats pending_operation as authoritative. */
    }

    watcher_data->events = events;
    watcher->events = events;

    /* Re-issue operations for the new event mask. IOCP drives READ through
     * the overlapped machinery; WRITE completions are delivered for overlapped
     * writes the CALLER issues directly on this IOCP-bound handle (WriteFile/
     * WSASend with the caller's own OVERLAPPED), demuxed in iocp_loop_run by
     * overlapped pointer. The backend never issues overlapped writes itself,
     * so arming PD_EVENT_WRITE here issues no I/O — it only records intent in
     * the event mask. Callers that want overlapped-write completions must issue
     * their own overlapped WriteFile/WSASend on the registered handle. The
     * re-issue below uses ReadFile for HANDLE watchers and WSARecv for socket
     * watchers; calling WSARecv on a HANDLE watcher would pass fd=-1, which is
     * a hard error. */
    if (events & PD_EVENT_READ && watcher_data->pending_operation == 0) {
        DWORD bytes_received = 0;
        memset(&watcher_data->overlapped, 0, sizeof(OVERLAPPED));

        int issued_ok = 0;
        if (watcher->is_handle && watcher->handle &&
                watcher->handle != INVALID_HANDLE_VALUE) {
            HANDLE handle = (HANDLE)watcher->handle;
            BOOL ok = ReadFile(
                handle,
                watcher_data->buffer,
                sizeof(watcher_data->buffer),
                &bytes_received,
                &watcher_data->overlapped
            );
            if (ok) {
                issued_ok = 1;
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING || err == ERROR_MORE_DATA) {
                    issued_ok = 1;
                } else {
                    pd_set_system_error(watcher->loop, err);
                    return PD_ERR_SYSTEM;
                }
            }
        } else {
            DWORD flags = 0;
            int rc = WSARecv(
                (SOCKET)(uintptr_t)(unsigned int)watcher->fd,
                &watcher_data->wsa_buffer,
                1,
                &bytes_received,
                &flags,
                &watcher_data->overlapped,
                NULL
            );
            if (rc == 0) {
                issued_ok = 1;
            } else if (WSAGetLastError() == WSA_IO_PENDING) {
                issued_ok = 1;
            } else {
                pd_set_system_error(watcher->loop, WSAGetLastError());
                return PD_ERR_SYSTEM;
            }
        }

        if (issued_ok) {
            watcher_data->pending_operation = 1;
        }
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

    /* pd_watcher_destroy calls stop then cleanup, both routing here. A
     * second entry while a deferred free is already in flight must do
     * nothing — the loop will free platform_data when it processes the
     * final completion. */
    if (watcher_data->stopping) {
        return PD_OK;
    }

    /* Resolve the HANDLE used to cancel pending I/O: handle-watchers use
     * watcher->handle directly; socket watchers cast the Winsock SOCKET. */
    HANDLE handle = NULL;
    if (watcher->is_handle && watcher->handle &&
            watcher->handle != INVALID_HANDLE_VALUE) {
        handle = (HANDLE)watcher->handle;
    } else if (watcher->fd >= 0) {
        handle = fd_to_handle(watcher->fd);
        if (handle == INVALID_HANDLE_VALUE) {
            handle = NULL;
        }
    }

    /* No read in flight: the kernel holds no pointer to the embedded
     * OVERLAPPED, so platform_data can be freed synchronously. This covers
     * the common case where the callback stopped its own watcher
     * (pending_operation was cleared to 0 in dispatch before the callback
     * ran) and the teardown case with nothing pending. */
    if (watcher_data->pending_operation == 0) {
        free(watcher->platform_data);
        watcher->platform_data = NULL;
        return PD_OK;
    }

    /* A read is in flight: the kernel still references watcher_data->overlapped
     * and will post one more completion for it (the CancelIoEx abort, or a
     * normal read that wins the cancel race). Freeing now would leave a
     * dangling OVERLAPPED the loop later dereferences -> use-after-free.
     * Mark the watcher stopping and cancel the specific overlapped.
     * CancelIoEx is cross-thread and per-overlapped; the legacy CancelIo only
     * cancels I/O issued by the calling thread, which would miss reads issued
     * by the loop thread, so it is not used here. The loop's stopping-check
     * frees platform_data when it processes that final completion. */
    watcher_data->stopping = 1;
    if (handle != NULL) {
        CancelIoEx(handle, &watcher_data->overlapped);
    }

    pd_iocp_data_t *data = (pd_iocp_data_t *)watcher->loop->platform_data;
    /* If we are NOT on the loop thread, block until the loop has processed
     * that final completion: iocp_drain_sync posts a sync packet after the
     * cancel, and IOCP FIFO ordering means the completion posted before the
     * sync is dequeued first, so the stopping-check has freed platform_data
     * by the time the sync fires. When drain_sync returns the kernel is done
     * with the overlapped and the caller may safely free the watcher struct.
     * If we ARE on the loop thread we must not block (the loop is not running
     * the IOCP wait, so the sync would never be processed -> deadlock); the
     * loop frees platform_data on its next completion, and the caller must
     * keep the watcher struct alive until then. */
    if (data && data->loop_thread_id != 0 &&
        GetCurrentThreadId() != data->loop_thread_id) {
        iocp_drain_sync(watcher->loop);
    }

    return PD_OK;
}

static size_t iocp_watcher_drain_read(pd_watcher_t *watcher, void *buf, size_t len) {
    if (!watcher || !watcher->platform_data || !buf || len == 0) {
        return 0;
    }
    pd_iocp_watcher_data_t *wd = (pd_iocp_watcher_data_t *)watcher->platform_data;
    if (wd->bytes_available == 0) {
        return 0;
    }
    size_t to_copy = (size_t)wd->bytes_available;
    if (to_copy > len) to_copy = len;
    memcpy(buf, wd->buffer, to_copy);
    if ((size_t)wd->bytes_available > to_copy) {
        memmove(wd->buffer, wd->buffer + to_copy,
                (size_t)wd->bytes_available - to_copy);
    }
    wd->bytes_available -= (DWORD)to_copy;
    return to_copy;
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

    /* Refuse to post a completion for a timer whose destroy has begun.
     * Even with the iocp_drain_sync barrier, a completion posted here
     * after the destroy read the destroyed flag would race the free. */
    if (timer_data->destroyed) {
        return;
    }

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
        /* Mark the timer as destroyed BEFORE DeleteTimerQueueTimer so that
         * any in-flight iocp_timer_callback observes the flag and refuses
         * to post a completion that would later race the free. */
        timer_data->destroyed = 1;
        if (timer_data->started && timer_data->queue_timer) {
            DeleteTimerQueueTimer(NULL, timer_data->queue_timer, INVALID_HANDLE_VALUE);
            timer_data->queue_timer = NULL;
            timer_data->started = 0;
        }
        /* Close the stop event handle. The completion dispatcher reads only
         * timer_data->destroyed (never stop_event), so closing it here — before
         * the drain — is safe. */
        if (timer_data->stop_event != NULL) {
            CloseHandle(timer_data->stop_event);
            timer_data->stop_event = NULL;
        }
        /* Do NOT free timer_data / clear platform_data yet. The loop thread may
         * be mid-dispatch of a queued timer completion: it has already read
         * td = timer->platform_data (in the PD_IOCP_TIMER_KEY branch of
         * iocp_loop_run) and is about to dereference td->destroyed. Freeing
         * here would turn that into a use-after-free.
         * Defer the free to after iocp_drain_sync, which is a barrier: once it
         * returns the loop has drained every queued completion for this timer
         * (no new ones can arrive — DeleteTimerQueueTimer above cancelled the
         * queue timer), so no loop thread holds a stale td pointer. The
         * dispatcher observes destroyed==1 on LIVE memory and skips the user
         * callback. */
    }

    /* Drain any in-flight completion packets BEFORE freeing the
     * placeholder watcher. After DeleteTimerQueueTimer(INVALID_HANDLE_VALUE)
     * returns, no new completions will be posted for this timer, but
     * completions already in the IOCP queue still reference this timer
     * via lpOverlapped. Wait for the loop to consume them; otherwise the
     * loop thread would dereference a freed pointer when it next runs
     * pd_loop_run_once. */
    if (timer->loop && timer->watcher) {
        iocp_drain_sync(timer->loop);
    }

    /* The drain barrier has now guaranteed no loop thread is mid-dispatch of a
     * timer completion for this timer, so freeing timer_data and clearing
     * platform_data is safe. */
    if (timer->platform_data) {
        pd_iocp_timer_data_t *timer_data = (pd_iocp_timer_data_t *)timer->platform_data;
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
    .watcher_drain_read = NULL,
    .async_send = NULL,
    .timer_create = NULL,
    .timer_start = NULL,
    .timer_stop = NULL,
    .timer_destroy = NULL,
    .name = "iocp",
    .max_events = 0,
};

#endif /* PD_PLATFORM_WINDOWS */