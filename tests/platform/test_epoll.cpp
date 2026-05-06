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
#include <thread>
#include <atomic>
#include <chrono>

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

/* Test: async_send wakes up a loop blocked in epoll_wait */
TEST(EpollTest, AsyncSendWakesLoop) {
    pd_loop_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_events_per_wait = 64;
    config.enable_thread_safety = 1;  /* Enable thread safety for async tests */
    pd_loop_t *loop = pd_loop_create(&config);
    ASSERT_NE(loop, nullptr);

    /* Create a socket pair so the loop has a watcher and doesn't exit immediately */
    int fds[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(result, 0);

    std::atomic<int> callback_count{0};
    auto callback = [](pd_loop_t *loop, pd_watcher_t *watcher,
                       pd_event_t events, void *user_data) {
        (void)loop; (void)watcher; (void)events;
        auto *count = static_cast<std::atomic<int>*>(user_data);
        count->fetch_add(1);
    };

    pd_watcher_t *watcher = pd_watcher_create(loop, fds[0], PD_EVENT_READ,
                                               callback, &callback_count);
    ASSERT_NE(watcher, nullptr);

    /* Send async notification from another thread.
     * The loop should wake up and the stop flag should be checked.
     * We stop the loop from the async sender. */
    std::atomic<bool> async_received{false};
    std::thread sender([&]() {
        /* Give the loop a moment to enter epoll_wait */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        async_received.store(true);
        int value = 42;
        int rc = pd_loop_async_send(loop, (void *)(intptr_t)value);
        EXPECT_EQ(rc, 0);
        /* Also stop the loop so it exits */
        pd_loop_stop(loop);
    });

    /* Run the loop with a generous timeout.
     * Without async_send, this would block for a long time.
     * With it, the loop should wake up quickly. */
    auto start = std::chrono::steady_clock::now();
    pd_loop_run(loop);  /* Should exit quickly due to async_send + stop */
    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    sender.join();

    /* The loop should have woken up within a reasonable time (well under 1 second) */
    EXPECT_LT(elapsed_ms, 1000);
    EXPECT_TRUE(async_received.load());

    /* Verify async_data can be retrieved */
    void *async_data = pd_loop_get_async_data(loop);
    EXPECT_EQ((intptr_t)async_data, 42);

    pd_watcher_destroy(watcher);
    close(fds[0]);
    close(fds[1]);
    pd_loop_destroy(loop);
}

/* Test: async_send returns success on a valid loop */
TEST(EpollTest, AsyncSendReturnsOK) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    int result = pd_loop_async_send(loop, nullptr);
    EXPECT_EQ(result, 0);

    pd_loop_destroy(loop);
}

/* Test: async_send with NULL loop returns error */
TEST(EpollTest, AsyncSendNullLoop) {
    int result = pd_loop_async_send(nullptr, nullptr);
    EXPECT_EQ(result, PD_ERR_INVALID_ARG);
}

/* Test: thread-safe loop does not deadlock when async_send is called
 * from another thread while the loop is blocked in epoll_wait.
 * Before the fix, the loop thread held the mutex during epoll_wait
 * and the sender thread would block trying to acquire it. */
TEST(EpollTest, ThreadSafeNoDeadlockOnAsyncSend) {
    pd_loop_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_events_per_wait = 64;
    config.enable_thread_safety = 1;
    pd_loop_t *loop = pd_loop_create(&config);
    ASSERT_NE(loop, nullptr);

    /* Create a socket pair so the loop has a watcher and doesn't exit immediately */
    int fds[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(result, 0);

    pd_watcher_t *watcher = pd_watcher_create(loop, fds[0], PD_EVENT_READ,
                                               nullptr, nullptr);
    ASSERT_NE(watcher, nullptr);

    /* Send async from another thread while the loop is blocked.
     * If the deadlock bug is present, pd_loop_async_send will never return
     * because it cannot acquire the mutex held by the blocked loop thread.
     * We use a short timeout on the sender to detect the hang. */
    std::atomic<bool> async_sent{false};
    std::thread sender([&]() {
        /* Give the loop a moment to enter epoll_wait */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int value = 99;
        int rc = pd_loop_async_send(loop, (void *)(intptr_t)value);
        EXPECT_EQ(rc, 0);
        async_sent.store(true);
        /* Stop the loop so it exits */
        pd_loop_stop(loop);
    });

    /* Run the loop - with the fix it should not deadlock */
    auto start = std::chrono::steady_clock::now();
    pd_loop_run(loop);
    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    sender.join();

    /* If this takes longer than 2 seconds, a deadlock likely occurred */
    EXPECT_LT(elapsed_ms, 2000);
    EXPECT_TRUE(async_sent.load());

    /* Verify async_data can be retrieved */
    void *async_data = pd_loop_get_async_data(loop);
    EXPECT_EQ((intptr_t)async_data, 99);

    pd_watcher_destroy(watcher);
    close(fds[0]);
    close(fds[1]);
    pd_loop_destroy(loop);
}