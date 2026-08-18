#include "feed/ws_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <utility>

namespace engine {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = boost::asio::ssl;
using tcp           = net::ip::tcp;

WebSocketClient::WebSocketClient(WsConfig cfg)
    : cfg_(std::move(cfg)),
      ssl_ctx_(ssl::context::tlsv12_client),
      resolver_(ioc_),
      reconnect_timer_(ioc_) {
    // verify the server against the system CA bundle - a market data feed
    // is an input to trading logic, so we don't skip certificate checks
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

void WebSocketClient::run() {
    start_connect();
    // blocks until there is no pending async work: after stop(), handlers
    // unwind without rearming and run() returns naturally
    ioc_.run();
}

void WebSocketClient::stop() {
    net::post(ioc_, [this] {
        stopping_ = true;
        reconnect_timer_.cancel();
        if (ws_ && ws_->is_open()) {
            // polite close frame; the pending read completes with
            // websocket::error::closed and unwinds
            ws_->async_close(websocket::close_code::normal,
                             [](beast::error_code) {});
        } else if (ws_) {
            beast::get_lowest_layer(*ws_).close();
        }
    });
}

void WebSocketClient::send(std::string text) {
    net::post(ioc_, [this, msg = std::move(text)]() mutable {
        write_queue_.push_back(std::move(msg));
        if (ws_ && ws_->is_open() && !writing_) {
            do_write();
        }
        // not connected yet: the frame stays queued and is flushed right
        // after the websocket handshake completes
    });
}

void WebSocketClient::start_connect() {
    if (stopping_) {
        return;
    }

    // fresh transport for every attempt; also drop frames queued for the
    // dead connection - the connect handler re-sends subscriptions
    ws_.emplace(ioc_, ssl_ctx_);
    writing_ = false;
    write_queue_.clear();
    buffer_.clear();

    resolver_.async_resolve(
        cfg_.host, cfg_.port,
        [this](beast::error_code ec, tcp::resolver::results_type results) {
            if (ec) return schedule_reconnect(ec, "resolve");

            beast::get_lowest_layer(*ws_).expires_after(cfg_.handshake_timeout);
            beast::get_lowest_layer(*ws_).async_connect(
                results,
                [this](beast::error_code ec2, const tcp::endpoint&) {
                    if (ec2) return schedule_reconnect(ec2, "tcp_connect");
                    on_tcp_connected();
                });
        });
}

void WebSocketClient::on_tcp_connected() {
    // SNI: modern servers host many certificates per IP and need the
    // hostname inside the TLS ClientHello to present the right one.
    // OpenSSL's macro expands to a C-style cast, hence the local suppression
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    const long sni_ok = SSL_set_tlsext_host_name(
        ws_->next_layer().native_handle(), cfg_.host.c_str());
#pragma GCC diagnostic pop
    if (sni_ok != 1) {
        beast::error_code ec(static_cast<int>(::ERR_get_error()),
                             net::error::get_ssl_category());
        return schedule_reconnect(ec, "sni");
    }

    ws_->next_layer().async_handshake(
        ssl::stream_base::client,
        [this](beast::error_code ec) {
            if (ec) return schedule_reconnect(ec, "tls_handshake");
            on_tls_handshake_done();
        });
}

void WebSocketClient::on_tls_handshake_done() {
    // hand liveness over to the websocket layer: it pings the peer when
    // the connection is idle and errors out if nothing comes back
    beast::get_lowest_layer(*ws_).expires_never();

    websocket::stream_base::timeout timeouts{};
    timeouts.handshake_timeout = cfg_.handshake_timeout;
    timeouts.idle_timeout      = cfg_.idle_timeout;
    timeouts.keep_alive_pings  = true;
    ws_->set_option(timeouts);

    ws_->set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
            req.set(beast::http::field::user_agent, "matching-engine/0.1");
        }));

    ws_->async_handshake(
        cfg_.host, cfg_.target,
        [this](beast::error_code ec) {
            if (ec) return schedule_reconnect(ec, "ws_handshake");

            reconnect_attempts_ = 0;  // healthy again: reset the backoff
            if (connect_handler_) {
                connect_handler_();
            }
            if (!write_queue_.empty() && !writing_) {
                do_write();
            }
            do_read();
        });
}

void WebSocketClient::do_read() {
    ws_->async_read(
        buffer_,
        [this](beast::error_code ec, std::size_t) {
            if (ec) return schedule_reconnect(ec, "read");

            if (message_handler_) {
                const auto data = buffer_.data();
                message_handler_(std::string_view(
                    static_cast<const char*>(data.data()), data.size()));
            }
            buffer_.consume(buffer_.size());
            do_read();
        });
}

void WebSocketClient::do_write() {
    writing_ = true;
    ws_->async_write(
        net::buffer(write_queue_.front()),
        [this](beast::error_code ec, std::size_t) {
            writing_ = false;
            if (ec) return schedule_reconnect(ec, "write");

            write_queue_.pop_front();
            if (!write_queue_.empty()) {
                do_write();
            }
        });
}

void WebSocketClient::schedule_reconnect(beast::error_code ec, const char* where) {
    // operation_aborted means WE cancelled it (shutdown or a competing
    // failure already tearing down this connection) - never reconnect on it
    if (stopping_ || ec == net::error::operation_aborted) {
        return;
    }

    if (disconnect_handler_) {
        std::string reason = std::string(where) + ": " + ec.message();
        disconnect_handler_(reason);
    }

    // capped exponential backoff: 500ms, 1s, 2s, ... up to the max. The
    // shift itself is clamped so 1u << n can't overflow on long outages
    const uint32_t shift = std::min<uint32_t>(reconnect_attempts_, 16);
    auto delay = cfg_.reconnect_base_delay * (1u << shift);
    delay      = std::min(delay, cfg_.reconnect_max_delay);
    ++reconnect_attempts_;

    // kill the old transport; pending ops on it complete with
    // operation_aborted and are ignored by the check above
    if (ws_) {
        beast::get_lowest_layer(*ws_).close();
    }

    reconnect_timer_.expires_after(delay);
    reconnect_timer_.async_wait([this](beast::error_code tec) {
        if (tec || stopping_) {
            return;  // cancelled (shutdown or superseded by a newer failure)
        }
        start_connect();
    });
}

} // namespace engine
