#include "server/ws_server.h"

#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <deque>
#include <iostream>
#include <utility>

namespace engine {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

// one connected browser. Owns its socket and a per-connection write queue
// (a slow client must not block the broadcast to others - it buffers and,
// past a cap, gets disconnected)
class DashboardSession : public std::enable_shared_from_this<DashboardSession> {
public:
    static constexpr size_t MAX_QUEUED = 64;  // ~6s of frames at 10Hz

    DashboardSession(tcp::socket socket, DashboardServer& owner)
        : ws_(std::move(socket)), owner_(owner) {}

    void start() {
        ws_.set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(beast::http::field::server, "matching-engine-dashboard");
            }));

        ws_.async_accept(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) return self->owner_.drop_session(self);
                self->ready_ = true;
                self->do_read();
                if (!self->queue_.empty()) self->do_write();
            });
    }

    void send(const std::shared_ptr<const std::string>& frame) {
        if (queue_.size() >= MAX_QUEUED) {
            // client can't keep up: drop it rather than buffer unboundedly
            close();
            return;
        }
        queue_.push_back(frame);
        if (ready_ && !writing_) do_write();
    }

    void close() {
        if (ws_.is_open()) {
            ws_.async_close(websocket::close_code::going_away,
                            [](beast::error_code) {});
        }
    }

private:
    void do_read() {
        // the dashboard protocol is one-way; reads exist only to learn
        // about disconnects (and drain client pings)
        ws_.async_read(
            rbuf_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return self->owner_.drop_session(self);
                self->rbuf_.consume(self->rbuf_.size());
                self->do_read();
            });
    }

    void do_write() {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(
            net::buffer(*queue_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                self->writing_ = false;
                if (ec) return self->owner_.drop_session(self);
                self->queue_.pop_front();
                if (!self->queue_.empty()) self->do_write();
            });
    }

    websocket::stream<beast::tcp_stream> ws_;
    DashboardServer&                     owner_;
    beast::flat_buffer                   rbuf_;
    std::deque<std::shared_ptr<const std::string>> queue_;
    bool ready_   = false;
    bool writing_ = false;
};

DashboardServer::DashboardServer(uint16_t port, SnapshotFn snapshot_fn,
                                 std::chrono::milliseconds broadcast_interval)
    : port_(port),
      snapshot_fn_(std::move(snapshot_fn)),
      interval_(broadcast_interval),
      acceptor_(ioc_),
      timer_(ioc_) {}

DashboardServer::~DashboardServer() { stop(); }

bool DashboardServer::start() {
    beast::error_code ec;
    const tcp::endpoint endpoint{net::ip::make_address("0.0.0.0"), port_};

    acceptor_.open(endpoint.protocol(), ec);
    if (!ec) acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (!ec) acceptor_.bind(endpoint, ec);
    if (!ec) acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "[dashboard] cannot listen on port " << port_ << ": "
                  << ec.message() << "\n";
        return false;
    }

    do_accept();
    arm_timer();

    thread_ = std::thread([this] { ioc_.run(); });
    return true;
}

void DashboardServer::stop() {
    if (!thread_.joinable()) return;

    net::post(ioc_, [this] {
        stopping_ = true;
        timer_.cancel();
        beast::error_code ec;
        acceptor_.close(ec);
        for (const auto& s : sessions_) s->close();
        sessions_.clear();
        clients_.store(0, std::memory_order_relaxed);
    });
    thread_.join();
}

void DashboardServer::do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
            if (ec || stopping_) return;  // acceptor closed on shutdown

            auto session = std::make_shared<DashboardSession>(
                std::move(socket), *this);
            sessions_.insert(session);
            clients_.store(sessions_.size(), std::memory_order_relaxed);
            session->start();

            do_accept();
        });
}

void DashboardServer::arm_timer() {
    timer_.expires_after(interval_);
    timer_.async_wait([this](beast::error_code ec) {
        if (ec || stopping_) return;

        if (!sessions_.empty()) {
            // one shared immutable frame for all sessions - serialized once
            auto frame = std::make_shared<const std::string>(snapshot_fn_());
            for (const auto& s : sessions_) s->send(frame);
        } else {
            // still call it so upstream request flags keep cycling and the
            // first client to connect gets fresh (not startup-stale) data
            (void)snapshot_fn_();
        }

        arm_timer();
    });
}

void DashboardServer::drop_session(const std::shared_ptr<DashboardSession>& s) {
    sessions_.erase(s);
    clients_.store(sessions_.size(), std::memory_order_relaxed);
}

} // namespace engine
