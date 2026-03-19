/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Timer example using socketpair for async notification.
 */

#include "poll-dancer/poll-dancer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #define closesocket close
    #define SOCKET int
#endif

/* Global variables for signal handling */
static pd_loop_t *g_loop = NULL;
static volatile int g_running = 1;

/* Timer callback */
static void timer_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                          pd_event_t events, void *user_data) {
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

    /* Create a timer using a socket pair */
    /* Note: This is a simplified example. A real implementation would use
     * platform-specific timer mechanisms like timerfd on Linux or
     * dispatch timers on macOS */
    int fds[2];
#ifdef _WIN32
    /* On Windows, you'd use a different mechanism */
    /* For simplicity, we'll just demonstrate the loop structure */
    printf("Timer example: Running event loop\n");
    printf("Note: Real timers require platform-specific implementation\n");
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        pd_loop_destroy(loop);
        return 1;
    }
#endif

    int timer_count = 0;

#ifndef _WIN32
    /* Create watcher for timer socket */
    pd_watcher_t *watcher = pd_watcher_create(loop, fds[0], PD_EVENT_READ,
                                             timer_callback, &timer_count);
    if (!watcher) {
        fprintf(stderr, "Failed to create watcher\n");
        closesocket(fds[0]);
        closesocket(fds[1]);
        pd_loop_destroy(loop);
        return 1;
    }

    /* Simulate timer by writing to the socket periodically */
    /* In a real application, you'd use a proper timer mechanism */
    printf("Timer example started\n");
    printf("This example demonstrates event loop structure\n");
    printf("Press Ctrl+C to stop\n");

    /* For demonstration, we'll just run the loop */
    /* A real timer implementation would write to fds[1] periodically */
#endif

    /* Run event loop */
    pd_loop_run(loop);

    /* Cleanup */
#ifndef _WIN32
    pd_watcher_destroy(watcher);
    closesocket(fds[0]);
    closesocket(fds[1]);
#endif

    pd_loop_destroy(loop);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}