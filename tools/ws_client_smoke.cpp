// Connectivity smoke test for WebSocketClient - the Commit 8 gate
//
// Default: connect to the Coinbase Exchange public feed over TLS, subscribe
// to the ticker channel for BTC-USD, print the first N messages, then close
// cleanly. The subscribe JSON lives here (not in the client): the client is
// exchange-agnostic, and the real Coinbase protocol layer arrives in Commit 9
//
// Usage:
//   ws-smoke [num_messages] [host] [port]
//     defaults: 10 messages, ws-feed.exchange.coinbase.com, 443
//   ws-smoke 0 127.0.0.1 1
//     reconnect demo: num_messages=0 means "don't wait for data". Prints
//     three failed-connect / backoff cycles, then stop()s. Port 1 is
//     almost never listening, so TCP connect fails immediately.

#include "feed/ws_client.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    const int max_messages = (argc > 1) ? std::atoi(argv[1]) : 10;
    const bool reconnect_demo = (max_messages <= 0);

    engine::WsConfig cfg;
    cfg.host = (argc > 2) ? argv[2] : "ws-feed.exchange.coinbase.com";
    cfg.port = (argc > 3) ? argv[3] : "443";
    if (reconnect_demo) {
        // keep the demo short: 200ms, 400ms, 800ms between the three attempts
        cfg.reconnect_base_delay = std::chrono::milliseconds(200);
        cfg.handshake_timeout    = std::chrono::seconds(2);
    }

    engine::WebSocketClient client(cfg);
    std::atomic<int> messages{0};
    std::atomic<int> disconnects{0};
    const auto start = std::chrono::steady_clock::now();

    auto elapsed = [start] {
        return std::chrono::duration_cast<std::chrono::duration<double>>(
                   std::chrono::steady_clock::now() - start).count();
    };

    client.set_connect_handler([&client] {
        std::cout << "[connected] TLS + websocket handshake OK, subscribing\n";
        client.send(R"({"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker"]})");
    });

    client.set_disconnect_handler([&](std::string_view reason) {
        const int n = ++disconnects;
        std::cout << "[disconnected #" << n << " t=" << elapsed() << "s] "
                  << reason << " (will retry)\n";
        if (reconnect_demo && n >= 3) {
            client.stop();
        }
    });

    client.set_message_handler([&](std::string_view msg) {
        const int n = ++messages;
        std::cout << "[msg " << n << "] " << msg.substr(0, 160)
                  << (msg.size() > 160 ? "..." : "") << "\n";
        if (!reconnect_demo && n >= max_messages) {
            client.stop();
        }
    });

    std::cout << "connecting to wss://" << cfg.host << ":" << cfg.port
              << cfg.target
              << (reconnect_demo ? " (reconnect demo, 3 attempts)\n" : "\n");
    client.run();
    std::cout << "clean shutdown after " << messages.load()
              << " messages, " << disconnects.load() << " disconnects\n";
    return 0;
}
