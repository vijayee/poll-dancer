/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Windows IOCP-specific tests using Google Test.
 */

#include <gtest/gtest.h>
#include <poll-dancer/poll-dancer.h>

#include <winsock2.h>

/* Test: IOCP socket monitoring */
TEST(IocpTest, SocketMonitoring) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    /* Create a socket */
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(sock, INVALID_SOCKET);

    pd_watcher_t *watcher = pd_watcher_create(loop, (int)sock,
                                               PD_EVENT_READ, nullptr, nullptr);
    ASSERT_NE(watcher, nullptr);

    pd_watcher_destroy(watcher);
    closesocket(sock);
    pd_loop_destroy(loop);
}