/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Basic event loop tests using Google Test.
 */

#include <gtest/gtest.h>
#include <poll-dancer/poll-dancer.h>

#include <string>

/* Test: Create and destroy loop */
TEST(LoopTest, CreateDestroy) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);
    pd_loop_destroy(loop);
}

/* Test: Create loop with config */
TEST(LoopTest, CreateWithConfig) {
    pd_loop_config_t config = {
        .max_events_per_wait = 128,
        .enable_thread_safety = 1
    };

    pd_loop_t *loop = pd_loop_create(&config);
    ASSERT_NE(loop, nullptr);
    pd_loop_destroy(loop);
}

/* Test: Get default loop */
TEST(LoopTest, DefaultLoop) {
    pd_loop_t *loop1 = pd_loop_default();
    ASSERT_NE(loop1, nullptr);

    pd_loop_t *loop2 = pd_loop_default();
    EXPECT_EQ(loop1, loop2);

    pd_loop_unref(loop1);
    pd_loop_unref(loop2);
}

/* Test: Reference counting */
TEST(LoopTest, RefCount) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    pd_loop_ref(loop);
    pd_loop_ref(loop);

    pd_loop_unref(loop);
    pd_loop_unref(loop);

    /* Should still be alive */
    EXPECT_NE(loop, nullptr);

    /* Final unref destroys it */
    pd_loop_unref(loop);
}

/* Test: Version information */
TEST(LoopTest, Version) {
    const char *version = pd_version_string();
    ASSERT_NE(version, nullptr);
    EXPECT_GT(strlen(version), 0u);

    int major, minor, patch;
    pd_version_components(&major, &minor, &patch);
    EXPECT_GE(major, 0);
    EXPECT_GE(minor, 0);
    EXPECT_GE(patch, 0);
}

/* Test: Platform name */
TEST(LoopTest, PlatformName) {
    const char *name = pd_platform_name();
    ASSERT_NE(name, nullptr);
    EXPECT_GT(strlen(name), 0u);

    printf("Platform: %s\n", name);
}

/* Test: Error strings */
TEST(LoopTest, ErrorStrings) {
    EXPECT_STREQ("Success", pd_error_string(PD_OK));
    EXPECT_STREQ("Invalid argument", pd_error_string(PD_ERR_INVALID_ARG));
    EXPECT_STREQ("Memory allocation failed", pd_error_string(PD_ERR_NO_MEMORY));
    EXPECT_STREQ("System error", pd_error_string(PD_ERR_SYSTEM));
}