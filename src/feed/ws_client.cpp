/// @file ws_client.cpp
/// @brief WebSocket client implementation — Phase 3 (Weeks 5-6).

#include "feed/ws_client.h"

namespace engine {

void WebSocketClient::connect(const std::string& uri) {
    // TODO: Phase 3 — Implement WebSocket connection using Boost.Beast.
    //
    // Steps:
    //   1. Parse the URI to extract host, port, and path.
    //   2. Resolve the host via Asio's resolver.
    //   3. Establish a TCP connection.
    //   4. Perform the TLS handshake (for wss:// URIs).
    //   5. Perform the WebSocket handshake.
    //   6. Start the async read loop.
    (void)uri;  // Suppress unused parameter warning
}

void WebSocketClient::disconnect() {
    // TODO: Phase 3 — Send WebSocket close frame, shutdown TLS, close socket.
    //
    // Steps:
    //   1. ws_.async_close(boost::beast::websocket::close_code::normal, ...);
    //   2. io_context_.stop();
}

void WebSocketClient::set_message_handler(
    std::function<void(std::string_view)> handler) {
    message_handler_ = std::move(handler);
}

void WebSocketClient::run() {
    // TODO: Phase 3 — Run the Asio io_context event loop.
    //
    // This blocks the calling thread and dispatches WebSocket messages
    // to the message_handler_ callback until disconnect() is called.
    //   io_context_.run();
}

} // namespace engine
