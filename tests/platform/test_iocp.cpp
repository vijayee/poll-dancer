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

/* Build a connected named-pipe pair (client, server), both opened with
 * FILE_FLAG_OVERLAPPED and PIPE_WAIT. The CLIENT handle is the one the test
 * registers with the loop (so overlapped I/O on it completes to the loop's
 * IOCP port, mirroring the offs_client transport); the SERVER handle is the
 * peer used to drive a read completion on the client. */
static void make_pipe_pair(HANDLE *out_client, HANDLE *out_server) {
    const char *name = "\\\\.\\pipe\\pd-test-iocp-write";
    HANDLE srv = CreateNamedPipeA(
        name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL);
    ASSERT_NE(srv, INVALID_HANDLE_VALUE);

    HANDLE cli = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10; attempt++) {
        cli = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (cli != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeA(name, 100);
    }
    ASSERT_NE(cli, INVALID_HANDLE_VALUE);

    /* Complete the server-side connect (overlapped). The client's CreateFile
     * may have already connected the instance, in which case ConnectNamedPipe
     * returns ERROR_PIPE_CONNECTED (232). */
    OVERLAPPED cov = {};
    BOOL ok = ConnectNamedPipe(srv, &cov);
    if (!ok) {
        DWORD err = GetLastError();
        ASSERT_TRUE(err == ERROR_IO_PENDING || err == ERROR_PIPE_CONNECTED)
            << "ConnectNamedPipe err=" << err;
        if (err == ERROR_IO_PENDING) {
            DWORD bytes = 0;
            ASSERT_TRUE(GetOverlappedResult(srv, &cov, &bytes, TRUE));
        }
    }

    *out_client = cli;
    *out_server = srv;
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

/* Test: a peer close is delivered as PD_EVENT_HANGUP, not a bare READ with
 * 0 bytes. This gates the #1 event-classification fix. */
TEST_F(IocpTest, PeerCloseDeliveredAsHangup) {
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

    /* Send some data, let the loop process it, then close the peer. */
    const char msg[] = "hello";
    ASSERT_EQ(send(cli, msg, (int)sizeof(msg) - 1, 0), (int)(sizeof(msg) - 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_EQ(closesocket(cli), 0);
    cli = INVALID_SOCKET;

    /* Wait (up to ~1s) for the HANGUP to arrive. */
    for (int i = 0; i < 100 && !log.saw_hangup.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    pd_loop_stop(loop);
    loop_thread.join();

    EXPECT_TRUE(log.saw_hangup.load())
        << "expected a HANGUP event on peer close";

    /* The event stream should contain at least one READ (the "hello") before
     * the HANGUP, and the HANGUP must not carry drained bytes. */
    auto evs = log.snapshot();
    ASSERT_GE(evs.size(), 1u);
    bool saw_read = false;
    for (size_t i = 0; i < evs.size(); i++) {
        if (evs[i] & PD_EVENT_READ) saw_read = true;
        if (evs[i] & PD_EVENT_HANGUP) {
            EXPECT_EQ(log.drained[i], 0u) << "HANGUP must not deliver bytes";
        }
    }
    EXPECT_TRUE(saw_read) << "expected a READ event for the data";

    pd_watcher_destroy(watcher);
    if (cli != INVALID_SOCKET) closesocket(cli);
    if (srv != INVALID_SOCKET) closesocket(srv);
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

/* Test: a client-issued overlapped WriteFile on an IOCP-bound handle is
 * delivered to the watcher callback as PD_EVENT_WRITE (not misclassified as
 * READ with the write's byte count), and a read armed on the same handle stays
 * in flight across the write completion. This gates the overlapped-async
 * named-pipe write path: the offs_client issues WriteFile with its own
 * OVERLAPPED on the pipe handle that the recv-thread loop already monitors for
 * reads, so the write completion lands in iocp_loop_run and must be demuxed by
 * overlapped pointer (not by the watcher's pending_operation). The handle is
 * IOCP-bound, so the caller cannot wait on the overlapped's hEvent; it reads
 * the result via GetOverlappedResult(bWait=FALSE) after the loop drains the
 * completion. Run under --gtest_repeat to stress the demux + read-in-flight
 * interaction. */
TEST_F(IocpTest, WriteCompletionDelivered) {
    pd_loop_t *loop = pd_loop_create(nullptr);
    ASSERT_NE(loop, nullptr);

    HANDLE cli = INVALID_HANDLE_VALUE, srv = INVALID_HANDLE_VALUE;
    ASSERT_NO_FATAL_FAILURE(make_pipe_pair(&cli, &srv));

    CallbackLog log;
    /* Register the CLIENT handle (the IOCP-bound one) for READ|WRITE.
     * Registration issues a ReadFile, so a read is in flight when the write
     * below is issued — exactly the case that would corrupt the read path if
     * the write completion were classified by pending_operation. */
    pd_watcher_t *watcher = pd_watcher_create_for_handle(
        loop, (void *)cli,
        static_cast<pd_event_t>(PD_EVENT_READ | PD_EVENT_WRITE),
        logging_callback, &log);
    ASSERT_NE(watcher, nullptr);

    std::thread loop_thread([&] { pd_loop_run(loop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    /* Issue an overlapped WriteFile on the client handle with a caller-owned
     * OVERLAPPED. The completion is posted to the loop's IOCP port (the handle
     * is bound there), not signaled via hEvent. */
    const char msg[] = "overlapped-write";
    const DWORD msg_len = (DWORD)(sizeof(msg) - 1);
    OVERLAPPED wov = {};
    DWORD written = 0;
    BOOL ok = WriteFile(cli, msg, msg_len, &written, &wov);
    if (!ok) {
        DWORD err = GetLastError();
        ASSERT_TRUE(err == ERROR_IO_PENDING || err == ERROR_MORE_DATA)
            << "WriteFile err=" << err;
    }

    /* Wait (up to ~1s) for the loop to deliver PD_EVENT_WRITE. */
    bool saw_write = false;
    for (int i = 0; i < 100; i++) {
        auto evs = log.snapshot();
        for (auto ev : evs) {
            if (ev & PD_EVENT_WRITE) saw_write = true;
        }
        if (saw_write) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(saw_write) << "expected a PD_EVENT_WRITE completion";

    /* The loop has dequeued the completion, so the kernel has finalized the
     * OVERLAPPED; GetOverlappedResult(bWait=FALSE) returns the byte count. */
    DWORD bytes = 0;
    BOOL gor = GetOverlappedResult(cli, &wov, &bytes, FALSE);
    EXPECT_TRUE(gor) << "GetOverlappedResult failed err=" << GetLastError();
    EXPECT_EQ(bytes, msg_len);

    /* Read-non-regression: the read armed at registration must still be in
     * flight. Drive it by writing from the peer (server) handle; the client's
     * pending ReadFile completes and the callback logs a READ with the bytes. */
    const char reply[] = "peer-reply";
    const DWORD reply_len = (DWORD)(sizeof(reply) - 1);
    DWORD swritten = 0;
    ASSERT_TRUE(WriteFile(srv, reply, reply_len, &swritten, nullptr))
        << "server WriteFile err=" << GetLastError();
    ASSERT_EQ(swritten, reply_len);

    bool saw_read = false;
    for (int i = 0; i < 100; i++) {
        auto evs = log.snapshot();
        for (size_t i2 = 0; i2 < evs.size(); i2++) {
            if ((evs[i2] & PD_EVENT_READ) && log.drained[i2] == reply_len) {
                saw_read = true;
            }
        }
        if (saw_read) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(saw_read) << "read was corrupted by the write completion";

    /* Destroy the watcher BEFORE stopping the loop: the read re-armed after
     * the peer reply is still in flight, so iocp_watcher_unregister takes the
     * deferred-free + iocp_drain_sync path, which needs a live loop thread to
     * process the CancelIoEx abort. Destroying after join would hit the
     * drain_sync 5s timeout (no loop thread to drain). pd_watcher_destroy
     * drops watcher_count to 0, so pd_loop_run exits on its own. */
    pd_watcher_destroy(watcher);
    pd_loop_stop(loop);
    loop_thread.join();

    CloseHandle(cli);
    CloseHandle(srv);
    pd_loop_destroy(loop);
}