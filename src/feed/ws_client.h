#pragma once

// Async WebSocket client over TLS (Boost.Beast on Boost.Asio)
//
// Design:
//   - single-threaded: every async handler runs on the thread that called
//     run(), so no locks are needed inside the client. send() and stop()
//     are safe to call from other threads - they post work onto the
//     io_context instead of touching state directly
//   - auto-reconnect: any failure (resolve, connect, handshake, read,
//     write) tears down the transport and schedules a retry with capped
//     exponential backoff. The connect handler fires after every
//     successful (re)connect, so subscriptions can be re-sent there
//   - liveness: websocket idle timeout with automatic keep-alive pings;
//     Beast also answers server pings with pongs on its own
//
// The client delivers raw text frames via the message handler; it knows
// nothing about Coinbase or JSON - that is the next layer's job

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>  // teardown support for TLS streams

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace engine {

struct WsConfig {
    std::string host;            // e.g. "ws-feed.exchange.coinbase.com"
    std::string port   = "443";
    std::string target = "/";    // HTTP path used in the websocket handshake

    std::chrono::milliseconds reconnect_base_delay{500};
    std::chrono::milliseconds reconnect_max_delay{30'000};
    std::chrono::seconds      handshake_timeout{30};
    std::chrono::seconds      idle_timeout{30};   // no traffic for this long = dead peer
};

class WebSocketClient {
public:
    using MessageHandler    = std::function<void(std::string_view)>;
    using ConnectHandler    = std::function<void()>;
    using DisconnectHandler = std::function<void(std::string_view reason)>;

    explicit WebSocketClient(WsConfig cfg);
    ~WebSocketClient() = default;

    // owns the io_context and streams; handlers capture `this`
    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&)                 = delete;
    WebSocketClient& operator=(WebSocketClient&&)      = delete;

    // called once per received text frame; the view is valid only for the
    // duration of the call (the buffer is reused for the next frame)
    void set_message_handler(MessageHandler h)       { message_handler_ = std::move(h); }

    // called after every successful (re)connect - send subscriptions here.
    // The write queue is cleared on disconnect, so anything the previous
    // connection didn't get must be re-sent by this handler
    void set_connect_handler(ConnectHandler h)       { connect_handler_ = std::move(h); }

    // called with a human-readable reason on every connection loss
    void set_disconnect_handler(DisconnectHandler h) { disconnect_handler_ = std::move(h); }

    // queue a text frame for sending; thread-safe (posts to the io thread).
    // Frames queued while disconnected are dropped at the next reconnect
    void send(std::string text);

    // connect and process events; blocks until stop(). Single-shot: create
    // a new client rather than calling run() twice
    void run();

    // graceful shutdown (websocket close frame, then unwind); thread-safe
    void stop();

private:
    using WsStream = boost::beast::websocket::stream<
        boost::asio::ssl::stream<boost::beast::tcp_stream>>;

    void start_connect();
    void on_tcp_connected();
    void on_tls_handshake_done();
    void do_read();
    void do_write();
    void schedule_reconnect(boost::beast::error_code ec, const char* where);

    WsConfig cfg_;

    boost::asio::io_context        ioc_;
    boost::asio::ssl::context      ssl_ctx_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::steady_timer      reconnect_timer_;

    // recreated for every connection attempt: a closed TLS stream cannot
    // be handshaken again, so optional::emplace gives us a fresh one
    std::optional<WsStream> ws_;

    boost::beast::flat_buffer buffer_;

    // websocket frames must not interleave, so writes are serialized
    // through this queue; writing_ marks an async_write in flight
    std::deque<std::string> write_queue_;
    bool writing_ = false;

    MessageHandler    message_handler_;
    ConnectHandler    connect_handler_;
    DisconnectHandler disconnect_handler_;

    uint32_t reconnect_attempts_ = 0;
    bool     stopping_           = false;
};

} // namespace engine
