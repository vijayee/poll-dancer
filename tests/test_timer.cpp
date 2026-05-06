/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Timer tests using Google Test.
 */

#include <gtest/gtest.h>
#include <poll-dancer/poll-dancer.h>

#include <unistd.h>
#include <atomic>

/* Test callback data */
typedef struct {
    std::atomic<int> callback_count;
    pd_loop_t *loop;
} timer_test_data_t;

/* Timer callback that counts invocations and stops the loop after a limit */
static void timer_test_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                                pd_event_t events, void *user_data) {
    (void)watcher;
    (void)events;
    timer_test_data_t *data = (timer_test_data_t *)user_data;
    if (data) {
        data->callback_count++;
    }
}

/* Test: Create and destroy timer */
TEST(TimerTest, CreateDestroy) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer_test_data_t test_data = {0, loop};

    pd_timer_t *timer = pd_timer_create(loop, 1000, 0,
                                         timer_test_callback, &test_data);
    ASSERT_NE(timer, nullptr);

    pd_error_t err = pd_timer_destroy(timer);
    EXPECT_EQ(err, PD_OK);

    pd_loop_destroy(loop);
}

/* Test: Start and stop timer without running loop */
TEST(TimerTest, StartStop) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer_test_data_t test_data = {0, loop};

    pd_timer_t *timer = pd_timer_create(loop, 1000, 0,
                                         timer_test_callback, &test_data);
    ASSERT_NE(timer, nullptr);

    /* Start the timer */
    pd_error_t err = pd_timer_start(timer);
    EXPECT_EQ(err, PD_OK);

    /* Stop the timer */
    err = pd_timer_stop(timer);
    EXPECT_EQ(err, PD_OK);

    pd_timer_destroy(timer);
    pd_loop_destroy(loop);
}

/* Test: One-shot timer fires once */
TEST(TimerTest, OneShotFires) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer_test_data_t test_data = {0, loop};

    /* Create a one-shot timer with 50ms delay */
    pd_timer_t *timer = pd_timer_create(loop, 50, 0,
                                         timer_test_callback, &test_data);
    ASSERT_NE(timer, nullptr);

    pd_error_t err = pd_timer_start(timer);
    EXPECT_EQ(err, PD_OK);

    /* Run the loop for a short while. Use run_once with timeout to allow
     * the timer to fire. */
    for (int i = 0; i < 10 && test_data.callback_count == 0; i++) {
        pd_loop_run_once(loop, 100);
    }

    /* The timer should have fired at least once */
    EXPECT_GE(test_data.callback_count.load(), 1);

    pd_timer_stop(timer);
    pd_timer_destroy(timer);
    pd_loop_destroy(loop);
}

/* Test: Repeating timer fires multiple times */
TEST(TimerTest, RepeatingFiresMultiple) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer_test_data_t test_data = {0, loop};

    /* Create a repeating timer: first fire at 20ms, then every 20ms */
    pd_timer_t *timer = pd_timer_create(loop, 20, 20,
                                         timer_test_callback, &test_data);
    ASSERT_NE(timer, nullptr);

    pd_error_t err = pd_timer_start(timer);
    EXPECT_EQ(err, PD_OK);

    /* Run the loop long enough for multiple firings */
    for (int i = 0; i < 20; i++) {
        pd_loop_run_once(loop, 50);
        if (test_data.callback_count.load() >= 3) {
            break;
        }
    }

    /* The timer should have fired at least 3 times */
    EXPECT_GE(test_data.callback_count.load(), 3);

    pd_timer_stop(timer);
    pd_timer_destroy(timer);
    pd_loop_destroy(loop);
}

/* Test: Stop timer prevents further firings */
TEST(TimerTest, StopPreventsFiring) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer_test_data_t test_data = {0, loop};

    /* Create a repeating timer with a long interval */
    pd_timer_t *timer = pd_timer_create(loop, 500, 500,
                                         timer_test_callback, &test_data);
    ASSERT_NE(timer, nullptr);

    /* Stop the timer before it fires */
    pd_error_t err = pd_timer_stop(timer);
    EXPECT_EQ(err, PD_OK);

    /* Run the loop for a short while */
    pd_loop_run_once(loop, 10);

    /* The timer should not have fired */
    EXPECT_EQ(test_data.callback_count.load(), 0);

    pd_timer_destroy(timer);
    pd_loop_destroy(loop);
}

/* Test: Start timer twice is idempotent */
TEST(TimerTest, StartTwice) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer_test_data_t test_data = {0, loop};

    pd_timer_t *timer = pd_timer_create(loop, 1000, 0,
                                         timer_test_callback, &test_data);
    ASSERT_NE(timer, nullptr);

    pd_error_t err = pd_timer_start(timer);
    EXPECT_EQ(err, PD_OK);

    /* Starting again should be OK (already active) */
    err = pd_timer_start(timer);
    EXPECT_EQ(err, PD_OK);

    pd_timer_stop(timer);
    pd_timer_destroy(timer);
    pd_loop_destroy(loop);
}

/* Test: Stop timer twice is idempotent */
TEST(TimerTest, StopTwice) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer_test_data_t test_data = {0, loop};

    pd_timer_t *timer = pd_timer_create(loop, 1000, 0,
                                         timer_test_callback, &test_data);
    ASSERT_NE(timer, nullptr);

    pd_error_t err = pd_timer_start(timer);
    EXPECT_EQ(err, PD_OK);

    err = pd_timer_stop(timer);
    EXPECT_EQ(err, PD_OK);

    /* Stopping again should be OK (already stopped) */
    err = pd_timer_stop(timer);
    EXPECT_EQ(err, PD_OK);

    pd_timer_destroy(timer);
    pd_loop_destroy(loop);
}

/* Test: Invalid arguments */
TEST(TimerTest, InvalidArguments) {
    /* NULL loop */
    pd_timer_t *timer = pd_timer_create(nullptr, 100, 0, timer_test_callback, nullptr);
    EXPECT_EQ(timer, nullptr);

    /* NULL callback */
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer = pd_timer_create(loop, 100, 0, nullptr, nullptr);
    EXPECT_EQ(timer, nullptr);

    /* NULL timer for start/stop/destroy */
    EXPECT_EQ(pd_timer_start(nullptr), PD_ERR_INVALID_ARG);
    EXPECT_EQ(pd_timer_stop(nullptr), PD_ERR_INVALID_ARG);
    EXPECT_EQ(pd_timer_destroy(nullptr), PD_ERR_INVALID_ARG);

    pd_loop_destroy(loop);
}

/* Test: Immediate timeout (timeout_ms = 0) */
TEST(TimerTest, ImmediateTimeout) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    timer_test_data_t test_data = {0, loop};

    /* Create a one-shot timer that fires immediately */
    pd_timer_t *timer = pd_timer_create(loop, 0, 0,
                                         timer_test_callback, &test_data);
    ASSERT_NE(timer, nullptr);

    pd_error_t err = pd_timer_start(timer);
    EXPECT_EQ(err, PD_OK);

    /* The timer should fire quickly */
    for (int i = 0; i < 5; i++) {
        pd_loop_run_once(loop, 50);
        if (test_data.callback_count.load() > 0) {
            break;
        }
    }

    EXPECT_GE(test_data.callback_count.load(), 1);

    pd_timer_stop(timer);
    pd_timer_destroy(timer);
    pd_loop_destroy(loop);
}