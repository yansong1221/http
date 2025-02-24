#pragma once
#include "httplib/stream/websocket_stream.hpp"
#include "httplib/use_awaitable.hpp"
#include "httplib/util/misc.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <queue>
#include <span>
#include <spdlog/spdlog.h>
#include "request.hpp"
#include "response.hpp"

namespace httplib
{

class websocket_conn : public std::enable_shared_from_this<websocket_conn>
{
public:
    class message
    {
    public:
        enum class data_type
        {
            text,
            binary
        };

    public:
        explicit message(std::string&& payload, data_type type = data_type::text)
            : payload_(std::move(payload)), type_(type) {};
        explicit message(std::string_view payload, data_type type = data_type::text)
            : payload_(payload), type_(type) {};
        message(const message&) = default;
        message(message&&) = default;
        message& operator=(message&&) = default;
        message& operator=(const message&) = default;

    public:
        const std::string& payload() const { return payload_; }
        data_type type() const { return type_; }

    private:
        std::string payload_;
        data_type type_;
    };

    using weak_ptr = std::weak_ptr<websocket_conn>;

    using open_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
    using close_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
    using message_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr, message)>;

public:
    websocket_conn(std::shared_ptr<spdlog::logger> logger, http_variant_stream_type&& stream)
        : logger_(logger), strand_(stream.get_executor()), ws_(create_websocket_variant_stream(std::move(stream)))
    {
    }

    void set_message_handler(message_handler_type handler) { message_handler_ = handler; }
    void set_open_handler(open_handler_type handler) { open_handler_ = handler; }
    void set_close_handler(close_handler_type handler) { close_handler_ = handler; }
    void send_message(const message& msg) { send_message(message(msg)); }
    void send_message(message&& msg)
    {
        if (!ws_.is_open()) return;

        net::post(strand_,
                  [this, msg = std::move(msg), self = shared_from_this()]() mutable
                  {
                      bool send_in_process = !send_que_.empty();
                      send_que_.push(std::move(msg));
                      if (send_in_process) return;
                      net::co_spawn(strand_, process_write_data(), net::detached);
                  });
    }
    void close()
    {
        if (!ws_.is_open()) return;

        net::co_spawn(
            strand_,
            [this, self = shared_from_this()]() -> net::awaitable<void>
            {
                boost::system::error_code ec;
                co_await net::post(strand_, net_awaitable[ec]);
                if (ec) co_return;

                websocket::close_reason reason("normal");
                co_await ws_.async_close(reason, net_awaitable[ec]);
            },
            net::detached);
    }

public:
    net::awaitable<void> process_write_data()
    {
        auto self = shared_from_this();

        for (;;)
        {
            boost::system::error_code ec;
            co_await net::post(strand_, net_awaitable[ec]);
            if (ec) co_return;
            if (send_que_.empty()) co_return;

            message msg = std::move(send_que_.front());
            send_que_.pop();
            if (msg.type() == message::data_type::text)
                ws_.text(true);
            else
                ws_.binary(true);
            co_await ws_.async_write(net::buffer(msg.payload()), net_awaitable[ec]);
            if (ec) co_return;
        }
    }
    net::awaitable<void> run(const http::request<body::any_body>& req)
    {
        boost::system::error_code ec;
        auto remote_endp = ws_.remote_endpoint(ec);
        co_await ws_.async_accept(req, net_awaitable[ec]);
        if (ec)
        {
            logger_->error("websocket handshake failed: {}", ec.message());
            co_return;
        }

        if (open_handler_) co_await open_handler_(weak_from_this());

        logger_->debug("websocket new connection: [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

        beast::flat_buffer buffer;
        for (;;)
        {
            auto bytes = co_await ws_.async_read(buffer, net_awaitable[ec]);
            if (ec)
            {
                logger_->debug("websocket disconnect: [{}:{}] what: {}",
                               remote_endp.address().to_string(),
                               remote_endp.port(),
                               ec.message());
                if (close_handler_) co_await close_handler_(weak_from_this());
                co_return;
            }

            if (message_handler_)
            {
                message msg(util::buffer_to_string_view(buffer.data()),
                            ws_.got_text() ? message::data_type::text : message::data_type::binary);
                net::co_spawn(co_await net::this_coro::executor,
                              message_handler_(weak_from_this(), std::move(msg)),
                              net::detached);
            }
            buffer.consume(bytes);
        }
    }

private:
    net::strand<net::any_io_executor> strand_;
    std::shared_ptr<spdlog::logger> logger_;
    websocket_variant_stream_type ws_;

    std::queue<message> send_que_;

    message_handler_type message_handler_;
    open_handler_type open_handler_;
    close_handler_type close_handler_;
};

} // namespace httplib