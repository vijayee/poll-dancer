/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Platform detection implementation.
 */

#include "internal/platform.h"

const pd_platform_ops_t *pd_platform_detect(void) {
#if defined(PD_PLATFORM_LINUX)
    return &pd_platform_epoll;
#elif defined(PD_PLATFORM_BSD)
    return &pd_platform_kqueue;
#elif defined(PD_PLATFORM_WINDOWS)
    return &pd_platform_iocp;
#else
    return NULL;
#endif
}