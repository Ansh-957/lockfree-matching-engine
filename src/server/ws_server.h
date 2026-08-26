#pragma once

// Plain (non-TLS) WebSocket broadcast server for the dashboard. Runs its
// own io_context on its own thread; every broadcast_interval it calls
// snapshot_fn() and pushes the resulting JSON frame to all connected
// browsers. Localhost tooling - hence no TLS and no client authentication
//
// Threading: everything inside runs on the single server thread, so
// sessions need no locks. start()/stop() are called from main

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

namespace engine {

class DashboardSession;

class DashboardServer {
public:
    // called on the server thread each broadcast tick; returns the frame
    using SnapshotFn = std::function<std::string()>;

    DashboardServer(uint16_t port, SnapshotFn snapshot_fn,
                    std::chrono::milliseconds broadcast_interval
                        = std::chrono::milliseconds(100));
    ~DashboardServer();

    DashboardServer(const DashboardServer&)            = delete;
    DashboardServer& operator=(const DashboardServer&) = delete;

    // returns false if the port could not be bound
    bool start();
    void stop();

    [[nodiscard]] size_t client_count() const {
        return clients_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint16_t port() const { return port_; }

private:
    friend class DashboardSession;

    void do_accept();
    void arm_timer();
    void drop_session(const std::shared_ptr<DashboardSession>& s);

    uint16_t                  port_;
    SnapshotFn                snapshot_fn_;
    std::chrono::milliseconds interval_;

    boost::asio::io_context        ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer      timer_;

    std::set<std::shared_ptr<DashboardSession>> sessions_;
    std::atomic<size_t> clients_{0};

    std::thread thread_;
    bool        stopping_ = false;
};

} // namespace engine
