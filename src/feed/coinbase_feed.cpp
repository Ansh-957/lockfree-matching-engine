#include "feed/coinbase_feed.h"

namespace engine {

CoinbaseFeed::CoinbaseFeed(std::string product_id)
    : product_id_(std::move(product_id)),
      ws_(WsConfig{.host = "ws-feed.exchange.coinbase.com"}) {
    batch_.reserve(4096);
}

void CoinbaseFeed::start(SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>& queue) {
    queue_ = &queue;

    ws_.set_connect_handler([this] {
        // Re-sent on every reconnect so we get a fresh snapshot after a drop.
        // `level2` now requires auth on Coinbase Exchange; `level2_batch` is
        // the public equivalent (same snapshot/l2update schema, coalesced
        // every 50ms). `matches` is the public trade tape.
        std::string sub = R"({"type":"subscribe","product_ids":[")"
                        + product_id_
                        + R"("],"channels":["level2_batch","matches"]})";
        ws_.send(std::move(sub));
    });

    ws_.set_message_handler([this](std::string_view raw) {
        batch_.clear();
        handler_.parse(raw, batch_);
        for (const auto& msg : batch_) {
            if (queue_->try_push(msg)) {
                pushed_.fetch_add(1, std::memory_order_relaxed);
            } else {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    ws_.run();
}

void CoinbaseFeed::stop() {
    ws_.stop();
}

} // namespace engine
