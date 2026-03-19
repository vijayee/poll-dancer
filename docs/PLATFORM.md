# Platform-Specific Notes

This document describes platform-specific behavior and limitations.

## Linux (epoll)

### Features
- Full support for all event types
- Edge-triggered mode (`PD_EVENT_EDGE`)
- Priority data (`PD_EVENT_PRI`)
- Hangup detection (`PD_EVENT_HANGUP` includes `EPOLLRDHUP`)

### Implementation Details
- Uses `epoll_create1(EPOLL_CLOEXEC)` for thread safety
- Supports `EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`, `EPOLL_CTL_DEL`
- Handles `EPOLLIN`, `EPOLLOUT`, `EPOLLERR`, `EPOLLHUP`, `EPOLLRDHUP`, `EPOLLPRI`

### Performance
- Excellent performance for high-concurrency scenarios
- Can handle 100k+ concurrent connections
- O(1) for event delivery

### Limitations
- Linux kernel 2.6.17+ for full functionality
- `EPOLLRDHUP` requires kernel 2.6.17+

### Debugging
- Use `strace -e epoll_wait,ioctl` to trace system calls
- Check `/proc/sys/fs/epoll/max_user_watches` for limits

## BSD/macOS (kqueue)

### Features
- Full support for all event types
- Edge-triggered mode via `EV_CLEAR`
- Hangup detection via `EV_EOF`

### Implementation Details
- Uses `kqueue()` to create event queue
- Separate `EVFILT_READ` and `EVFILT_WRITE` filters
- `EV_ENABLE`/`EV_DISABLE` for watcher control
- Uses `EV_SET` macro for configuration

### Platform Differences
- **macOS**: Works identically to FreeBSD
- **FreeBSD**: Works as documented
- **OpenBSD**: Works as documented
- **NetBSD**: Works as documented

### Performance
- Excellent performance
- Scales well with number of file descriptors
- O(1) for event delivery

### Debugging
- Use `ktrace`/`kdump` to trace kevent calls
- No special system configuration needed

## Windows (IOCP)

### Features
- Completion-based I/O (different from epoll/kqueue)
- Automatic buffer management for async operations
- Integrated with Windows thread pool (optional)

### Implementation Details
- Uses `CreateIoCompletionPort()` for loop creation
- `GetQueuedCompletionStatusEx()` for waiting
- `PostQueuedCompletionStatus()` for async notifications
- File descriptors are converted to HANDLEs
- Automatic async read issuance for `PD_EVENT_READ`

### Paradigm Shift
**Important**: IOCP is completion-based, not readiness-based:
- When you register for `PD_EVENT_READ`, an async read is **started**
- The callback is invoked when the read **completes**, not when data is available
- This requires buffer management for pending operations

### Buffer Management
The implementation maintains internal buffers for pending I/O:
```c
struct pd_iocp_watcher_data_t {
    OVERLAPPED overlapped;
    WSABUF wsa_buffer;
    char buffer[4096];  // Default buffer size
    int pending_operation;
};
```

For large reads, you may need to implement custom buffering.

### Performance
- Excellent for high-concurrency scenarios
- Native Windows thread pool integration
- Best for Windows-specific deployments

### Limitations
- Different semantics than epoll/kqueue
- Requires Winsock 2.2 (`WSAStartup`)
- Must call `WSAStartup()` before using the library

### Debugging
- Use DebugView to monitor debug output
- Check Windows Event Log for errors
- Use `GetLastError()` for detailed error information

## Cross-Platform Considerations

### File Descriptors
- **Unix**: Regular file descriptors (integers)
- **Windows**: SOCKET handles (must call `_get_osfhandle()`)

### Socket Handling
- Always use `recv()`/`send()` instead of `read()`/`write()` for sockets
- Use `closesocket()` instead of `close()` on Windows

### Initialization
- **Windows**: Call `WSAStartup()` before using the library
- **Unix**: No special initialization needed

### Cleanup
- **Windows**: Call `WSACleanup()` before process exit
- **Unix**: No special cleanup needed

## Platform Detection

The library automatically detects the platform at compile time:

```c
#if defined(__linux__)
    #define PD_PLATFORM_LINUX 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || ...
    #define PD_PLATFORM_BSD 1
#elif defined(_WIN32) || defined(_WIN64)
    #define PD_PLATFORM_WINDOWS 1
#endif
```

You can query the platform at runtime:

```c
const char *platform = pd_platform_name();
// Returns "epoll", "kqueue", or "iocp"
```

## Testing on Different Platforms

### Linux
```bash
mkdir build && cd build
cmake ..
make
ctest
```

### macOS
```bash
mkdir build && cd build
cmake ..
make
ctest
```

### Windows (Visual Studio)
```cmd
mkdir build && cd build
cmake ..
cmake --build . --config Release
ctest -C Release
```

### Windows (MinGW)
```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
mingw32-make
ctest
```

## Known Issues

### Linux
- None at this time

### BSD/macOS
- None at this time

### Windows
- `pd_loop_async_send()` implementation is simplified (not thread-safe)
- Need to call `WSAStartup()` before using the library
- Socket pairs require TCP loopback (no Unix domain sockets)

## Future Improvements

### All Platforms
- Add timer support (timerfd on Linux, kevent timers on BSD, CreateTimerQueueTimer on Windows)
- Add signal handling support
- Add async DNS resolution

### Linux
- Use `eventfd` for async notifications
- Add support for `signalfd`
- Add support for `timerfd`

### BSD/macOS
- Add support for `EVFILT_USER` for async notifications
- Add support for `EVFILT_SIGNAL` for signal handling
- Add support for `EVFILT_TIMER` for timers

### Windows
- Improve async send implementation
- Add proper cancellation support
- Implement using Windows thread pool