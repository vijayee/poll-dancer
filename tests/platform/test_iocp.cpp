/*
 * poll-dancer - Cross-platform event loop library
 * Copyright (C) 2026
 *
 * Windows IOCP-specific tests using Google Test.
 */

#include <gtest/gtest.h>
#include <poll-dancer/poll-dancer.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

/* Records the event mask delivered to a watcher callback, plus the drained
 * byte count, behind a mutex so the test thread can read it safely while the
 * loop thread fires callbacks. */
struct CallbackLog {
    std::mutex mu;
    std::vector<pd_event_t> events;
    std::vector<size_t> drained;
    std::atomic<bool> saw_hangup{false};
    std::atomic<bool> saw_error{false};

    void record(pd_event_t ev, size_t bytes) {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(ev);
        drained.push_back(bytes);
        if (ev & PD_EVENT_HANGUP) saw_hangup = true;
        if (ev & PD_EVENT_ERROR) saw_error = true;
    }

    std::vector<pd_event_t> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return events;
    }
};

/* Loop-thread callback: drain the read buffer (so the backend re-arms) and
 * log the event. */
static void logging_callback(pd_loop_t *loop, pd_watcher_t *watcher,
                              pd_event_t events, void *user_data) {
    (void)loop;
    CallbackLog *log = (CallbackLog *)user_data;
    char buf[4096];
    size_t bytes = 0;
    if (events & PD_EVENT_READ) {
        bytes = pd_watcher_drain_read(watcher, buf, sizeof(buf));
    }
    log->record(events, bytes);
}

/* Build a connected TCP loopback socket pair (client, server). Both sockets
 * are left in blocking mode, which is fine for overlapped I/O. */
static void make_loopback_pair(SOCKET *out_client, SOCKET *out_server) {
    SOCKET lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(lst, INVALID_SOCKET);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(bind(lst, (sockaddr *)&addr, (int)sizeof(addr)), 0);
    ASSERT_EQ(listen(lst, 1), 0);

    sockaddr_in bound = {};
    int blen = (int)sizeof(bound);
    ASSERT_EQ(getsockname(lst, (sockaddr *)&bound, &blen), 0);

    SOCKET cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(cli, INVALID_SOCKET);
    sockaddr_in srv = {};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv.sin_port = bound.sin_port;
    ASSERT_EQ(connect(cli, (sockaddr *)&srv, (int)sizeof(srv)), 0);

    SOCKET srv_conn = accept(lst, nullptr, nullptr);
    ASSERT_NE(srv_conn, INVALID_SOCKET);
    closesocket(lst);

    *out_client = cli;
    *out_server = srv_conn;
}

/* Fixture: initialize Winsock for each test. poll-dancer does not call
 * WSAStartup itself, so the test must. */
class IocpTest : public ::testing::Test {
protected:
    WSADATA wsa = {};

    void SetUp() override {
        ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
    }

    void TearDown() override {
        WSACleanup();
    }
};

/* Test: IOCP socket monitoring. Uses a connected loopback pair so the READ
 * arming WSARecv at registration succeeds (WSARecv on an unconnected TCP
 * socket fails with WSAENOTCONN and registration returns NULL). */
TEST_F(IocpTest, SocketMonitoring) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    SOCKET cli = INVALID_SOCKET, srv = INVALID_SOCKET;
    ASSERT_NO_FATAL_FAILURE(make_loopback_pair(&cli, &srv));

    pd_watcher_t *watcher = pd_watcher_create(loop, (int)srv,
                                               PD_EVENT_READ, nullptr, nullptr);
    ASSERT_NE(watcher, nullptr);

    pd_watcher_destroy(watcher);
    closesocket(cli);
    closesocket(srv);
    pd_loop_destroy(loop);
}

/* Test: stop a watcher while a read is in flight (no data yet) from a thread
 * OTHER than the loop thread, then destroy it. This exercises the #2 deferred
 * free + iocp_drain_sync path: unregister must not free the platform data
 * (and its embedded OVERLAPPED) until the loop has processed the CancelIoEx
 * abort. Run under --gtest_repeat to stress the race; a UAF typically crashes
 * within a few iterations (definitively caught under Application Verifier). */
TEST_F(IocpTest, StopWhileReadPendingFromOtherThread) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    SOCKET cli = INVALID_SOCKET, srv = INVALID_SOCKET;
    ASSERT_NO_FATAL_FAILURE(make_loopback_pair(&cli, &srv));

    CallbackLog log;
    pd_watcher_t *watcher = pd_watcher_create(loop, (int)srv,
                                               PD_EVENT_READ, logging_callback,
                                               &log);
    ASSERT_NE(watcher, nullptr);

    std::thread loop_thread([&] { pd_loop_run(loop); });
    /* Give the loop thread time to enter the IOCP wait. */
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    /* The initial WSARecv issued at registration is still pending (no data
     * sent), so this stop hits the pending_operation == 1 path off the loop
     * thread -> stopping + CancelIoEx + iocp_drain_sync. */
    pd_watcher_destroy(watcher);

    /* Destroying the watcher removed it from the loop; pd_loop_run returns
     * once watcher_count == 0. If it has not, stop it explicitly. */
    pd_loop_stop(loop);
    loop_thread.join();

    closesocket(cli);
    closesocket(srv);
    pd_loop_destroy(loop);
}