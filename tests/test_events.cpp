/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Event handling tests using Google Test.
 */

#include <gtest/gtest.h>
#include <poll-dancer/poll-dancer.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

/* Test callback data */
typedef struct {
    int callback_count;
    pd_event_t last_events;
    void *last_user_data;
} test_callback_data_t;

/* Test callback function */
static void test_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                          pd_event_t events, void *user_data) {
    test_callback_data_t *data = (test_callback_data_t *)user_data;
    if (data) {
        data->callback_count++;
        data->last_events = events;
        data->last_user_data = user_data;
    }
}

/* Test: Create and destroy watcher */
TEST(EventsTest, WatcherCreateDestroy) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    /* Create a socket pair for testing */
    int fds[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(result, 0);

    test_callback_data_t callback_data = {0};

    pd_watcher_t *watcher = pd_watcher_create(loop, fds[0], PD_EVENT_READ,
                                               test_callback, &callback_data);
    ASSERT_NE(watcher, nullptr);

    pd_watcher_destroy(watcher);
    close(fds[0]);
    close(fds[1]);
    pd_loop_destroy(loop);
}

/* Test: Watcher start and stop */
TEST(EventsTest, WatcherStartStop) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    int fds[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(result, 0);

    test_callback_data_t callback_data = {0};

    pd_watcher_t *watcher = pd_watcher_create(loop, fds[0], PD_EVENT_READ,
                                               test_callback, &callback_data);
    ASSERT_NE(watcher, nullptr);

    /* Stop watcher */
    result = pd_watcher_stop(watcher);
    EXPECT_EQ(result, 0);

    /* Start watcher */
    result = pd_watcher_start(watcher);
    EXPECT_EQ(result, 0);

    pd_watcher_destroy(watcher);
    close(fds[0]);
    close(fds[1]);
    pd_loop_destroy(loop);
}

/* Test: Watcher update events */
TEST(EventsTest, WatcherUpdate) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    int fds[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(result, 0);

    test_callback_data_t callback_data = {0};

    pd_watcher_t *watcher = pd_watcher_create(loop, fds[0], PD_EVENT_READ,
                                               test_callback, &callback_data);
    ASSERT_NE(watcher, nullptr);

    /* Update to monitor write events */
    result = pd_watcher_update(watcher, (pd_event_t)(PD_EVENT_READ | PD_EVENT_WRITE));
    EXPECT_EQ(result, 0);

    pd_event_t events = pd_watcher_get_events(watcher);
    EXPECT_TRUE(events & PD_EVENT_READ);
    EXPECT_TRUE(events & PD_EVENT_WRITE);

    int fd = pd_watcher_get_fd(watcher);
    EXPECT_EQ(fds[0], fd);

    pd_watcher_destroy(watcher);
    close(fds[0]);
    close(fds[1]);
    pd_loop_destroy(loop);
}

/* Test: Run loop once with timeout */
TEST(EventsTest, LoopRunOnceTimeout) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    /* Run with 0 timeout - should return immediately */
    int result = pd_loop_run_once(loop, 0);
    EXPECT_GE(result, 0);

    /* Run with 10ms timeout - should return quickly */
    result = pd_loop_run_once(loop, 10);
    EXPECT_GE(result, 0);

    pd_loop_destroy(loop);
}

/* Test: Stop loop */
TEST(EventsTest, LoopStop) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    int result = pd_loop_stop(loop);
    EXPECT_EQ(result, 0);

    pd_loop_destroy(loop);
}