/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Linux-specific tests using Google Test.
 */

#include <gtest/gtest.h>
#include <poll-dancer/poll-dancer.h>

#include <sys/socket.h>
#include <unistd.h>

/* Test: epoll edge-triggered mode */
TEST(EpollTest, EdgeTriggered) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    int fds[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(result, 0);

    pd_watcher_t *watcher = pd_watcher_create(loop, fds[0],
                                               (pd_event_t)(PD_EVENT_READ | PD_EVENT_EDGE),
                                               nullptr, nullptr);
    ASSERT_NE(watcher, nullptr);

    pd_watcher_destroy(watcher);
    close(fds[0]);
    close(fds[1]);
    pd_loop_destroy(loop);
}

/* Test: epoll hangup detection */
TEST(EpollTest, HangupDetection) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    int fds[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(result, 0);

    pd_watcher_t *watcher = pd_watcher_create(loop, fds[0],
                                               (pd_event_t)(PD_EVENT_READ | PD_EVENT_HANGUP),
                                               nullptr, nullptr);
    ASSERT_NE(watcher, nullptr);

    pd_watcher_destroy(watcher);
    close(fds[0]);
    close(fds[1]);
    pd_loop_destroy(loop);
}