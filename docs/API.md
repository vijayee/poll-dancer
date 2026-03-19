# poll-dancer API Documentation

## Overview

poll-dancer is a cross-platform C library that provides a unified API for event-driven I/O. It abstracts platform-specific event notification mechanisms (epoll on Linux, kqueue on BSD/macOS, IOCP on Windows) behind a single, easy-to-use interface.

## Quick Start

```c
#include <poll-dancer/poll-dancer.h>

void on_read(pd_loop_t *loop, pd_watcher_t *watcher,
             pd_event_t events, void *user_data) {
    char buffer[1024];
    ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
    // Handle data...
}

int main(void) {
    // Create event loop
    pd_loop_t *loop = pd_loop_create(NULL);

    // Create a watcher for a socket
    pd_watcher_t *watcher = pd_watcher_create(
        loop,
        socket_fd,
        PD_EVENT_READ,
        on_read,
        user_data
    );

    // Run the event loop
    pd_loop_run(loop);

    // Cleanup
    pd_watcher_destroy(watcher);
    pd_loop_destroy(loop);

    return 0;
}
```

## Core Concepts

### Event Loop

The event loop (`pd_loop_t`) is the central object that manages all I/O watchers and dispatches events to callbacks.

**Lifecycle:**
1. Create with `pd_loop_create()`
2. Add watchers with `pd_watcher_create()`
3. Run with `pd_loop_run()` or `pd_loop_run_once()`
4. Destroy with `pd_loop_destroy()`

**Thread Safety:**
By default, loops are thread-safe when compiled with `POLL_DANCER_THREAD_SAFE=ON`. You can disable this for performance-critical single-threaded applications.

### Watchers

Watchers (`pd_watcher_t`) monitor file descriptors for specified events and invoke callbacks when events occur.

**Event Types:**
- `PD_EVENT_READ` - Socket/file is readable
- `PD_EVENT_WRITE` - Socket/file is writable
- `PD_EVENT_ERROR` - Error condition occurred
- `PD_EVENT_HANGUP` - Remote end closed connection
- `PD_EVENT_PRI` - Priority/out-of-band data available
- `PD_EVENT_EDGE` - Use edge-triggered mode (vs level-triggered)

**Lifecycle:**
1. Create with `pd_watcher_create()`
2. Optionally modify with `pd_watcher_update()`
3. Stop/start with `pd_watcher_stop()` / `pd_watcher_start()`
4. Destroy with `pd_watcher_destroy()`

### Callbacks

Event callbacks have the signature:
```c
void (*pd_callback_t)(pd_loop_t *loop, pd_watcher_t *watcher,
                     pd_event_t events, void *user_data);
```

The `events` parameter contains the events that occurred (may be multiple events OR'd together).

## API Reference

### Loop Management

```c
pd_loop_t *pd_loop_create(const pd_loop_config_t *config);
```
Create a new event loop with optional configuration. Returns NULL on error.

```c
pd_loop_t *pd_loop_default(void);
```
Get the process-wide default loop. Created on first call, destroyed on process exit.

```c
void pd_loop_destroy(pd_loop_t *loop);
void pd_loop_ref(pd_loop_t *loop);
void pd_loop_unref(pd_loop_t *loop);
```
Destroy a loop or manage reference counting.

### Running the Loop

```c
int pd_loop_run(pd_loop_t *loop);
```
Run the loop until no more watchers remain or `pd_loop_stop()` is called. Returns 0 on success, negative error code on failure.

```c
int pd_loop_run_once(pd_loop_t *loop, int timeout_ms);
```
Run one iteration of the loop. Returns number of events processed, or negative error code. Timeout of -1 means wait indefinitely, 0 means poll (return immediately).

```c
int pd_loop_stop(pd_loop_t *loop);
```
Stop a running loop. Can be called from another thread or from within a callback.

### Watcher Management

```c
pd_watcher_t *pd_watcher_create(pd_loop_t *loop,
                                 int fd,
                                 pd_event_t events,
                                 pd_callback_t callback,
                                 void *user_data);
```
Create a new watcher. The watcher is initially active.

```c
int pd_watcher_update(pd_watcher_t *watcher, pd_event_t events);
```
Change the events being monitored.

```c
int pd_watcher_stop(pd_watcher_t *watcher);
int pd_watcher_start(pd_watcher_t *watcher);
```
Stop/start a watcher without destroying it.

```c
void pd_watcher_destroy(pd_watcher_t *watcher);
```
Destroy a watcher and free its resources.

```c
int pd_watcher_get_fd(pd_watcher_t *watcher);
pd_event_t pd_watcher_get_events(pd_watcher_t *watcher);
```
Get the file descriptor or events for a watcher.

### Error Handling

```c
const char *pd_error_string(pd_error_t error);
```
Get a human-readable error string for an error code.

```c
int pd_get_system_error(pd_loop_t *loop);
const char *pd_get_system_error_string(pd_loop_t *loop);
```
Get the system error (errno/GetLastError) from the last failed operation on a loop.

### Version Information

```c
const char *pd_version_string(void);
void pd_version_components(int *major, int *minor, int *patch);
int pd_is_thread_safe(void);
const char *pd_platform_name(void);
```
Get version and platform information.

## Platform Differences

### Linux (epoll)
- Supports edge-triggered mode (`PD_EVENT_EDGE`)
- Supports priority data (`PD_EVENT_PRI`)
- Handles `EPOLLRDHUP` for hangup detection
- Most efficient for high-performance servers

### BSD/macOS (kqueue)
- Supports edge-triggered mode via `EV_CLEAR`
- Separates read and write filters internally
- Excellent performance and feature parity with epoll

### Windows (IOCP)
- Completion-based, not readiness-based
- When you register for `PD_EVENT_READ`, an async read is started
- Requires buffer management for pending operations
- Different performance characteristics than epoll/kqueue

## Best Practices

### Resource Management
- Always destroy watchers before destroying the loop
- Use reference counting for loops shared across threads
- Close file descriptors after destroying watchers

### Performance
- Use edge-triggered mode for high-load scenarios
- Consider disabling thread safety for single-threaded applications
- Batch operations when possible

### Error Handling
- Always check return values for errors
- Use `pd_get_system_error_string()` for detailed diagnostics
- Handle `PD_EVENT_HANGUP` and `PD_EVENT_ERROR` in callbacks

### Thread Safety
- The loop can be used from multiple threads when compiled with `POLL_DANCER_THREAD_SAFE`
- Callbacks are invoked from the thread running `pd_loop_run()`
- Use `pd_loop_async_send()` to wake the loop from another thread

## Common Patterns

### Echo Server

```c
void echo_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                   pd_event_t events, void *user_data) {
    int fd = pd_watcher_get_fd(watcher);
    char buffer[4096];

    if (events & PD_EVENT_READ) {
        ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes > 0) {
            send(fd, buffer, bytes, 0);  // Echo back
        } else {
            // Connection closed
            close(fd);
            pd_watcher_destroy(watcher);
        }
    }
}
```

### Timer with socketpair

```c
// Create socketpair for timer notifications
int timer_fds[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, timer_fds);

// Watch for timer notifications
pd_watcher_t *watcher = pd_watcher_create(loop, timer_fds[0],
                                          PD_EVENT_READ, timer_callback, NULL);

// In another thread or signal handler:
write(timer_fds[1], "X", 1);  // Wake up loop
```

## Building and Installation

See [BUILDING.md](BUILDING.md) for build instructions.

## Platform-Specific Notes

See [PLATFORM.md](PLATFORM.md) for platform-specific details.