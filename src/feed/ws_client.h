#pragma once

/// @file ws_client.h
/// @brief WebSocket client abstraction for market data feeds.
///
/// This is a Phase 3 component (Weeks 5-6). The interface is defined now
/// so that dependent code (CoinbaseFeed, FeedHandler) can be structured
/// against it. The implementation will use Boost.Beast / Boost.Asio.

#include <functional>
#include <string>
#include <string_view>

namespace engine {

/// @brief WebSocket client for connecting to exchange feeds.
///
/// Provides an async WebSocket connection with a callback-based message
/// delivery model. The run() method blocks on the Asio io_context.
class WebSocketClient {
public:
    WebSocketClient() = default;
    ~WebSocketClient() = default;

    // Non-copyable, non-movable (will own Asio objects).
    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&)                 = delete;
    WebSocketClient& operator=(WebSocketClient&&)      = delete;

    /// @brief Connect to a WebSocket endpoint.
    /// @param uri  The WebSocket URI (e.g., "wss://ws-feed.exchange.coinbase.com").
    void connect(const std::string& uri);

    /// @brief Disconnect from the WebSocket endpoint.
    void disconnect();

    /// @brief Set the callback invoked for each received message.
    /// @param handler  Callback receiving the raw message payload.
    void set_message_handler(std::function<void(std::string_view)> handler);

    /// @brief Run the event loop (blocks until disconnect is called).
    void run();

private:
    // TODO: Phase 3 — Add Boost.Beast / Boost.Asio members:
    //   boost::asio::io_context io_context_;
    //   boost::beast::websocket::stream<boost::asio::ssl::stream<
    //       boost::asio::ip::tcp::socket>> ws_;
    //   std::function<void(std::string_view)> message_handler_;
    //   bool connected_ = false;

    std::function<void(std::string_view)> message_handler_;
};

} // namespace engine
