#include "httplib/server.hpp"
#include "httplib/html.h"
#include "httplib/use_awaitable.hpp"
#include <boost/asio/detached.hpp>
#include <boost/beast/core/detect_ssl.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace httplib {

namespace detail {

template<class Body>
net::awaitable<void> async_read_http_body(http::request_parser<http::empty_body> &&header_parser,
                                          stream::http_stream_variant_type &stream,
                                          beast::flat_buffer &buffer, httplib::request &req,
                                          boost::system::error_code &ec) {

    http::request_parser<Body> body_parser(std::move(header_parser));

    while (!body_parser.is_done()) {
        stream.expires_after(std::chrono::seconds(30));
        co_await http::async_read_some(stream, buffer, body_parser, net_awaitable[ec]);
        stream.expires_never();
        if (ec) {
            co_return;
        }
    }
    req = std::move(body_parser.release());
    co_return;
}
} // namespace detail

server::server(uint32_t num_threads /*= std::thread::hardware_concurrency()*/)
    : pool_(num_threads),
      acceptor_(pool_),
      logger_(spdlog::stdout_color_mt("server")),
      router_(logger_) {

    logger_->set_level(spdlog::level::trace);
    ssl_config_ =
        ssl_config{R"(D:\code\http\lib\server.crt)", R"(D:\code\http\lib\server.key)", "test"};
    // ssl_config_ = ssl_config{};
}

httplib::server &server::listen(std::string_view host, uint16_t port,
                                int backlog /*= net::socket_base::max_listen_connections*/) {
    tcp::resolver resolver(pool_);
    auto results = resolver.resolve(host, std::to_string(port));

    tcp::endpoint endp(*results.begin());
    acceptor_.open(endp.protocol());
    acceptor_.bind(endp);
    acceptor_.listen(backlog);
    logger_->info("Server Listen on: [{}:{}]", endp.address().to_string(), endp.port());
    return *this;
}

void server::run() {
    async_run();
    pool_.wait();
}

void server::async_run() {
    net::co_spawn(pool_, do_listen(), net::detached);
}

net::awaitable<void> server::do_listen() {
    boost::system::error_code ec;

    const auto &executor = co_await net::this_coro::executor;
    for (;;) {
        tcp::socket sock(executor);
        co_await acceptor_.async_accept(sock, net_awaitable[ec]);
        if (ec)
            co_return;

        net::co_spawn(
            executor,
            [this, sock = std::move(sock)]() mutable -> net::awaitable<void> {
                auto remote_endp = sock.remote_endpoint();
                logger_->trace("accept new connection [{}:{}]", remote_endp.address().to_string(),
                               remote_endp.port());
                co_await do_session(std::move(sock));
                logger_->trace("close connection [{}:{}]", remote_endp.address().to_string(),
                               remote_endp.port());
            },
            net::detached);
    }
}

std::shared_ptr<ssl::context> server::create_ssl_context() {
    try {
        unsigned long ssl_options = ssl::context::default_workarounds | ssl::context::no_sslv2 |
                                    ssl::context::single_dh_use;

        auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
        ssl_ctx->set_options(ssl_options);

        if (!ssl_config_->passwd.empty()) {
            ssl_ctx->set_password_callback([this](auto, auto) { return ssl_config_->passwd; });
        }
        ssl_ctx->use_certificate_chain_file(ssl_config_->cert_file.string());
        ssl_ctx->use_private_key_file(ssl_config_->key_file.string(), ssl::context::pem);
        return ssl_ctx;
    } catch (const std::exception &e) {
        logger_->error("create_ssl_context: {}", e.what());
        return nullptr;
    }
}

net::awaitable<void> server::do_session(tcp::socket sock) {
    try {
        net::ip::tcp::endpoint remote_endpoint = sock.remote_endpoint();
        net::ip::tcp::endpoint local_endpoint = sock.local_endpoint();

        std::unique_ptr<stream::http_stream_variant_type> http_variant_stream;
        beast::flat_buffer buffer;
        boost::system::error_code ec;
        bool is_ssl = co_await beast::async_detect_ssl(sock, buffer, net_awaitable[ec]);
        if (ec) {
            logger_->error("async_detect_ssl failed: {}", ec.message());
            co_return;
        }
        if (is_ssl) {
            auto ssl_ctx = create_ssl_context();
            if (!ssl_ctx)
                co_return;

            stream::ssl_http_stream stream(std::move(sock), ssl_ctx);
            auto bytes_used = co_await stream.async_handshake(ssl::stream_base::server,
                                                              buffer.data(), net_awaitable[ec]);
            if (ec) {
                logger_->error("ssl handshake failed: {}", ec.message());
                co_return;
            }
            buffer.consume(bytes_used);
            http_variant_stream =
                std::make_unique<stream::http_stream_variant_type>(std::move(stream));
        } else {
            stream::http_stream stream(std::move(sock));
            http_variant_stream =
                std::make_unique<stream::http_stream_variant_type>(std::move(stream));
        }

        for (;;) {
            http::request_parser<http::empty_body> header_parser;
            while (!header_parser.is_header_done()) {
                http_variant_stream->expires_after(std::chrono::seconds(30));
                co_await http::async_read_some(*http_variant_stream, buffer, header_parser,
                                               net_awaitable[ec]);
                http_variant_stream->expires_never();
                if (ec) {
                    logger_->trace("read http header failed: {}", ec.message());
                    co_return;
                }
            }

            const auto &header = header_parser.get();

            // websocket
            if (websocket::is_upgrade(header)) {
                co_await handle_websocket(std::move(*http_variant_stream), header_parser.release());
                co_return;
            }
            //http proxy
            else if (header.method() == http::verb::connect) {
                co_await handle_connect(std::move(*http_variant_stream), header_parser.release());
                co_return;
            }

            httplib::request req;
            switch (header.base().method()) {
            case http::verb::get:
            case http::verb::head:
            case http::verb::trace:
            case http::verb::connect:
                req = header_parser.release();
                break;
            default: {

                auto content_type = header[http::field::content_type];
                if (content_type.starts_with("multipart/form-data")) {

                    co_await detail::async_read_http_body<form_data_body>(
                        std::move(header_parser), *http_variant_stream, buffer, req, ec);
                } else {
                    co_await detail::async_read_http_body<http::string_body>(
                        std::move(header_parser), *http_variant_stream, buffer, req, ec);
                }
                if (ec) {
                    logger_->trace("read http body failed: {}", ec.message());
                    co_return;
                }
            } break;
            }

            req.local_endpoint = local_endpoint;
            req.remote_endpoint = remote_endpoint;

            httplib::response resp;
            resp.base().result(http::status::not_found);
            resp.base().version(req.base().version());
            resp.base().set(http::field::server, BOOST_BEAST_VERSION_STRING);
            resp.base().set(http::field::date, html::format_http_date());
            resp.keep_alive(req.keep_alive());

            co_await router_.routing(req, resp);
            co_await resp.async_write(*http_variant_stream, ec,
                                      req.base().method() == http::verb::head);
            if (ec)
                co_return;

            if (!resp.keep_alive()) {
                // This means we should close the connection, usually because
                // the response indicated the "Connection: close" semantic.
                boost::system::error_code ec;
                http_variant_stream->shutdown(net::socket_base::shutdown_send, ec);
                co_return;
            }
        }
    } catch (const std::exception &e) {
        spdlog::error("do_session: {}", e.what());
    }
}

net::awaitable<void> server::handle_connect(stream::http_stream_variant_type http_variant_stream,
                                            http::request<http::empty_body> req) {
    auto target = req.target();
    auto pos = target.find(":");
    if (pos == std::string_view::npos)
        co_return;

    auto host = target.substr(0, pos);
    auto port = target.substr(pos + 1);

    boost::system::error_code ec;
    tcp::resolver resolver(co_await net::this_coro::executor);
    auto results = co_await resolver.async_resolve(host, port, net_awaitable[ec]);
    if (ec)
        co_return;

    tcp::socket proxy_socket(co_await net::this_coro::executor);
    co_await net::async_connect(proxy_socket, results, net_awaitable[ec]);
    if (ec)
        co_return;

    http::response<http::empty_body> resp(http::status::ok, req.version());
    resp.base().reason("Connection Established");
    resp.base().set(http::field::server, BOOST_BEAST_VERSION_STRING);
    resp.base().set(http::field::date, html::format_http_date());
    co_await http::async_write(http_variant_stream, resp, net_awaitable[ec]);
    if (ec)
        co_return;

    auto conn = std::make_shared<httplib::proxy_conn>(std::move(http_variant_stream),
                                                      std::move(proxy_socket));

    co_await conn->run();
    co_return;
}

net::awaitable<void> server::handle_websocket(stream::http_stream_variant_type http_variant_stream,
                                              http::request<http::empty_body> req) {
    auto conn = std::make_shared<httplib::websocket_conn>(logger_, std::move(http_variant_stream));
    conn->set_open_handler(websocket_open_handler_);
    conn->set_close_handler(websocket_close_handler_);
    conn->set_message_handler(websocket_message_handler_);
    co_await conn->run(req);
    co_return;
}

} // namespace httplib