/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Timer example using the pd_timer API.
 */

#include "poll-dancer/poll-dancer.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <unistd.h>
#endif

/* Global variables for signal handling */
static pd_loop_t *g_loop = NULL;
static volatile int g_running = 1;

/* Timer callback */
static void timer_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                          pd_event_t events, void *user_data) {
    (void)watcher;
    (void)events;
    int *count = (int *)user_data;
    printf("Timer fired! Count: %d\n", *count);

    (*count)++;

    if (*count >= 5) {
        printf("Stopping after 5 timer events\n");
        pd_loop_stop(loop);
        g_running = 0;
    }
}

#ifndef _WIN32
/* Signal handler */
static void signal_handler(int sig) {
    (void)sig;
    if (g_loop) {
        pd_loop_stop(g_loop);
    }
    g_running = 0;
}
#endif

int main(void) {
#ifdef _WIN32
    /* Initialize Winsock */
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    /* Create event loop */
    pd_loop_t *loop = pd_loop_create(NULL);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    g_loop = loop;

#ifndef _WIN32
    /* Set up signal handler for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    int timer_count = 0;

    /* Create a repeating timer: first fire at 1000ms, then every 1000ms */
    pd_timer_t *timer = pd_timer_create(loop, 1000, 1000,
                                         timer_callback, &timer_count);
    if (!timer) {
        fprintf(stderr, "Failed to create timer\n");
        pd_loop_destroy(loop);
        return 1;
    }

    /* Start the timer */
    pd_error_t err = pd_timer_start(timer);
    if (err != PD_OK) {
        fprintf(stderr, "Failed to start timer: %s\n", pd_error_string(err));
        pd_timer_destroy(timer);
        pd_loop_destroy(loop);
        return 1;
    }

    printf("Timer example started\n");
    printf("Timer fires every 1000ms, stops after 5 events\n");
    printf("Press Ctrl+C to stop\n");

    /* Run event loop */
    pd_loop_run(loop);

    /* Cleanup */
    pd_timer_destroy(timer);
    pd_loop_destroy(loop);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}