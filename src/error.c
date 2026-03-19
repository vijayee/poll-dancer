/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Error handling implementation.
 */

#include "poll-dancer/errors.h"
#include "internal/internal.h"
#include "internal/loop.h"
#include "internal/loop.h"

#include <string.h>
#include <errno.h>

#ifdef PD_PLATFORM_WINDOWS
    #include <windows.h>
#endif

/**
 * Error code descriptions.
 */
static const char *error_strings[] = {
    "Success",
    "Unknown error",
    "Invalid argument",
    "Memory allocation failed",
    "System error",
    "Feature not implemented on this platform",
    "Loop has been closed",
    "Watcher is stopped",
    "Resource already exists",
    "Resource not found",
};

const char *pd_error_string(pd_error_t error) {
    /* Handle success case */
    if (error == PD_OK) {
        return error_strings[0];
    }

    /* Handle negative error codes */
    if (error < 0) {
        int index = -error;
        if (index >= 0 && (size_t)index < PD_ARRAY_SIZE(error_strings)) {
            return error_strings[index];
        }
    }
    return "Invalid error code";
}

int pd_get_system_error(pd_loop_t *loop) {
    if (!loop) {
        return 0;
    }
    return loop->system_error;
}

const char *pd_get_system_error_string(pd_loop_t *loop) {
    if (!loop) {
        return "No error";
    }

#ifdef PD_PLATFORM_WINDOWS
    static __thread char buffer[256];
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        loop->system_error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer,
        sizeof(buffer),
        NULL
    );
    return buffer;
#else
    return strerror(loop->system_error);
#endif
}

/**
 * Set the system error for a loop.
 *
 * @param loop The loop
 * @param error The system error code
 */
void pd_set_system_error(pd_loop_t *loop, int error) {
    if (loop) {
        loop->system_error = error;
    }
}

/**
 * Get the current errno (Unix) or GetLastError() (Windows).
 *
 * @return The current system error
 */
int pd_get_current_system_error(void) {
#ifdef PD_PLATFORM_WINDOWS
    return GetLastError();
#else
    return errno;
#endif
}