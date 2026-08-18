// Connectivity smoke test for WebSocketClient - the Commit 8 gate
//
// Connects to the Coinbase Exchange public feed over TLS, subscribes to
// the ticker channel for BTC-USD, prints the first N messages, then
// closes cleanly. The subscribe JSON lives here (not in the client): the
// client is exchange-agnostic, and the real Coinbase protocol layer
// arrives in Commit 9
//
// Usage: ws-smoke [num_messages]   (default 10)

#include "feed/ws_client.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    const int max_messages = (argc > 1) ? std::atoi(argv[1]) : 10;

    engine::WsConfig cfg;
    cfg.host = "ws-feed.exchange.coinbase.com";

    engine::WebSocketClient client(cfg);
    std::atomic<int> count{0};

    client.set_connect_handler([&client] {
        std::cout << "[connected] TLS + websocket handshake OK, subscribing\n";
        client.send(R"({"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker"]})");
    });

    client.set_disconnect_handler([](std::string_view reason) {
        std::cout << "[disconnected] " << reason << " (will retry)\n";
    });

    client.set_message_handler([&](std::string_view msg) {
        const int n = ++count;
        std::cout << "[msg " << n << "] " << msg.substr(0, 160)
                  << (msg.size() > 160 ? "..." : "") << "\n";
        if (n >= max_messages) {
            client.stop();
        }
    });

    std::cout << "connecting to wss://" << cfg.host << cfg.target << "\n";
    client.run();
    std::cout << "clean shutdown after " << count.load() << " messages\n";
    return 0;
}
