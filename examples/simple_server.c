/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Simple TCP echo server example.
 */

#include "poll-dancer/poll-dancer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <fcntl.h>
    #define closesocket close
    #define SOCKET int
    #define INVALID_SOCKET -1
#endif

#define PORT 8080
#define BUFFER_SIZE 4096

/* Connection state */
typedef struct {
    pd_watcher_t *watcher;
    int fd;
    char buffer[BUFFER_SIZE];
    size_t buffer_len;
} connection_t;

/* Forward declarations */
static void accept_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                            pd_event_t events, void *user_data);
static void read_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                         pd_event_t events, void *user_data);

/* Accept callback */
static void accept_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                            pd_event_t events, void *user_data) {
    SOCKET listen_fd = pd_watcher_get_fd(watcher);

    /* Accept new connection */
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    SOCKET client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

    if (client_fd < 0) {
        perror("accept");
        return;
    }

    printf("New connection accepted\n");

    /* Create connection state */
    connection_t *conn = calloc(1, sizeof(connection_t));
    if (!conn) {
        closesocket(client_fd);
        return;
    }

    conn->fd = client_fd;

    /* Create watcher for client socket */
    conn->watcher = pd_watcher_create(loop, client_fd, PD_EVENT_READ,
                                      read_callback, conn);
    if (!conn->watcher) {
        free(conn);
        closesocket(client_fd);
        return;
    }
}

/* Read callback */
static void read_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                         pd_event_t events, void *user_data) {
    connection_t *conn = (connection_t *)user_data;
    int fd = pd_watcher_get_fd(watcher);

    if (events & PD_EVENT_READ) {
        /* Read data from client */
        ssize_t bytes = recv(fd, conn->buffer + conn->buffer_len,
                            BUFFER_SIZE - conn->buffer_len, 0);

        if (bytes <= 0) {
            /* Connection closed or error */
            printf("Connection closed\n");
            pd_watcher_destroy(watcher);
            closesocket(fd);
            free(conn);
            return;
        }

        conn->buffer_len += bytes;

        /* Echo data back */
        send(fd, conn->buffer, conn->buffer_len, 0);
        conn->buffer_len = 0;
    }

    if (events & PD_EVENT_HANGUP) {
        printf("Client disconnected\n");
        pd_watcher_destroy(watcher);
        closesocket(fd);
        free(conn);
    }

    if (events & PD_EVENT_ERROR) {
        fprintf(stderr, "Socket error\n");
        pd_watcher_destroy(watcher);
        closesocket(fd);
        free(conn);
    }
}

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

    /* Create listening socket */
    SOCKET listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd == INVALID_SOCKET) {
        perror("socket");
        pd_loop_destroy(loop);
        return 1;
    }

    /* Set socket options */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    /* Bind socket */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        closesocket(listen_fd);
        pd_loop_destroy(loop);
        return 1;
    }

    /* Listen */
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        closesocket(listen_fd);
        pd_loop_destroy(loop);
        return 1;
    }

    printf("Listening on port %d\n", PORT);

    /* Create watcher for listening socket */
    pd_watcher_t *watcher = pd_watcher_create(loop, listen_fd, PD_EVENT_READ,
                                               accept_callback, NULL);
    if (!watcher) {
        fprintf(stderr, "Failed to create watcher\n");
        closesocket(listen_fd);
        pd_loop_destroy(loop);
        return 1;
    }

    /* Run event loop */
    printf("Server started, waiting for connections...\n");
    pd_loop_run(loop);

    /* Cleanup */
    pd_watcher_destroy(watcher);
    closesocket(listen_fd);
    pd_loop_destroy(loop);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}