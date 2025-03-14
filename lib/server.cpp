
#include "httplib/server.hpp"

#include "body/compressor.hpp"
#include "httplib/body/body.hpp"
#include "httplib/html.hpp"
#include "httplib/request.hpp"
#include "httplib/response.hpp"
#include "httplib/router.hpp"
#include "proxy_conn.hpp"
#include "stream/http_stream.hpp"
#include "websocket_conn_impl.hpp"
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core/detect_ssl.hpp>
#include <boost/beast/http/serializer.hpp>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace httplib {
using namespace std::chrono_literals;

namespace detail {
template<class Body>
httplib::response
make_respone(const http::request<Body>& req)
{
    httplib::response resp;
    resp.result(http::status::not_found);
    resp.version(req.version());
    resp.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    resp.set(http::field::date, html::format_http_current_gmt_date());
    resp.keep_alive(req.keep_alive());
    return resp;
}
} // namespace detail

class server::impl : public std::enable_shared_from_this<server::impl> {
public:
    impl(uint32_t num_threads /*= std::thread::hardware_concurrency()*/)
        : pool_(num_threads)
        , acceptor_(pool_)
        , logger_(spdlog::stdout_color_mt("httplib.server"))
        , router_(logger_)
    {
        logger_->set_level(spdlog::level::info);
    }

public:
    std::shared_ptr<spdlog::logger> logger_;
    router router_;
    net::thread_pool pool_;
    tcp::acceptor acceptor_;
    std::optional<ssl_config> ssl_config_;

    websocket_conn::message_handler_type websocket_message_handler_;
    websocket_conn::open_handler_type websocket_open_handler_;
    websocket_conn::close_handler_type websocket_close_handler_;

    std::chrono::steady_clock::duration timeout_ = 30s;

#ifdef HTTPLIB_ENABLED_SSL
    std::shared_ptr<ssl::context>
    create_ssl_context()
    {
        try {
            unsigned long ssl_options = ssl::context::default_workarounds |
                                        ssl::context::no_sslv2 |
                                        ssl::context::single_dh_use;

            auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
            ssl_ctx->set_options(ssl_options);

            if (!ssl_config_->passwd.empty()) {
                ssl_ctx->set_password_callback(
                    [this](auto, auto) { return ssl_config_->passwd; });
            }
            ssl_ctx->use_certificate_chain_file(ssl_config_->cert_file.string());
            ssl_ctx->use_private_key_file(ssl_config_->key_file.string(),
                                          ssl::context::pem);
            return ssl_ctx;
        } catch (const std::exception& e) {
            logger_->error("create_ssl_context: {}", e.what());
            return nullptr;
        }
    }
#endif
    net::awaitable<std::optional<http_variant_stream_type>>
    async_create_http_variant_stream(tcp::socket&& sock, beast::flat_buffer& buffer)
    {
        http_stream no_ssl_stream(std::move(sock));
#ifndef HTTPLIB_ENABLED_SSL
        co_return std::move(no_ssl_stream);
#else
        boost::system::error_code ec;
        no_ssl_stream.expires_after(timeout_);
        bool is_ssl =
            co_await beast::async_detect_ssl(no_ssl_stream, buffer, net_awaitable[ec]);
        if (ec) {
            logger_->error("async_detect_ssl failed: {}", ec.message());
            co_return std::nullopt;
        }
        if (!is_ssl) co_return std::move(no_ssl_stream);

        auto ssl_ctx = create_ssl_context();
        if (!ssl_ctx) co_return std::nullopt;

        ssl_http_stream use_ssl_stream(std::move(no_ssl_stream), ssl_ctx);
        auto bytes_used = co_await use_ssl_stream.async_handshake(
            ssl::stream_base::server, buffer.data(), net_awaitable[ec]);
        if (ec) {
            logger_->error("ssl handshake failed: {}", ec.message());
            co_return std::nullopt;
        }
        buffer.consume(bytes_used);
        co_return std::move(use_ssl_stream);

#endif
    }
    void
    listen(std::string_view host, uint16_t port, int backlog)
    {
        tcp::resolver resolver(pool_);
        auto results = resolver.resolve(host, std::to_string(port));

        tcp::endpoint endp(*results.begin());
        acceptor_.open(endp.protocol());
        acceptor_.bind(endp);
        acceptor_.listen(backlog);
        logger_->info(
            "Server Listen on: [{}:{}]", endp.address().to_string(), endp.port());
    }

    net::awaitable<void>
    do_listen()
    {
        boost::system::error_code ec;
        const auto& executor = co_await net::this_coro::executor;
        for (;;) {
            tcp::socket sock(executor);
            co_await acceptor_.async_accept(sock, net_awaitable[ec]);
            if (ec) co_return;

            net::co_spawn(
                executor,
                [this, sock = std::move(sock)]() mutable -> net::awaitable<void> {
                    auto self        = shared_from_this();
                    auto remote_endp = sock.remote_endpoint();
                    logger_->trace("accept new connection [{}:{}]",
                                   remote_endp.address().to_string(),
                                   remote_endp.port());
                    co_await do_session(std::move(sock));
                    logger_->trace("close connection [{}:{}]",
                                   remote_endp.address().to_string(),
                                   remote_endp.port());
                },
                net::detached);
        }
    }

    net::awaitable<void>
    do_session(tcp::socket&& sock)
    {
        try {
            net::ip::tcp::endpoint remote_endpoint = sock.remote_endpoint();
            net::ip::tcp::endpoint local_endpoint  = sock.local_endpoint();

            beast::flat_buffer buffer;

            auto http_variant_stream =
                co_await async_create_http_variant_stream(std::move(sock), buffer);
            if (!http_variant_stream) co_return;

            for (;;) {
                boost::system::error_code ec;
                http::request_parser<http::empty_body> header_parser;
                header_parser.body_limit(std::numeric_limits<unsigned long long>::max());
                while (!header_parser.is_header_done()) {
                    http_variant_stream->expires_after(timeout_);
                    co_await http::async_read_some(
                        *http_variant_stream, buffer, header_parser, net_awaitable[ec]);
                    http_variant_stream->expires_never();
                    if (ec) {
                        logger_->trace("read http header failed: {}", ec.message());
                        co_return;
                    }
                }

                const auto& header = header_parser.get();

                // websocket
                if (websocket::is_upgrade(header)) {
#ifdef HTTPLIB_ENABLED_WEBSOCKET
                    request req(header_parser.release());
                    co_await handle_websocket(std::move(*http_variant_stream),
                                              std::move(req));
#endif // HTTPLIB_ENABLED_WEBSOCKET
                    co_return;
                }
                // http proxy
                else if (header.method() == http::verb::connect) {
                    request req(header_parser.release());
                    co_await handle_connect(std::move(*http_variant_stream),
                                            std::move(req));
                    co_return;
                }
                httplib::response resp = detail::make_respone(header);
                httplib::request req;
                if (router_.has_handler(header.method(), header.target())) {
                    switch (header.method()) {
                        case http::verb::get:
                        case http::verb::head:
                        case http::verb::trace:
                        case http::verb::connect:
                            req = httplib::request(header_parser.release());
                            break;
                        default: {
                            http::request_parser<body::any_body> body_parser(
                                std::move(header_parser));
                            while (!body_parser.is_done()) {
                                http_variant_stream->expires_after(timeout_);
                                co_await http::async_read_some(*http_variant_stream,
                                                               buffer,
                                                               body_parser,
                                                               net_awaitable[ec]);
                                http_variant_stream->expires_never();
                                if (ec) {
                                    logger_->trace("read http body failed: {}",
                                                   ec.message());
                                    co_return;
                                }
                            }
                            req = body_parser.release();
                        } break;
                    }
                    // init request
                    req.local_endpoint  = local_endpoint;
                    req.remote_endpoint = remote_endpoint;

                    auto tokens = util::split(req.target(), "?");
                    if (tokens.empty() || tokens.size() > 2) {
                        resp.set_empty_content(http::status::bad_request);
                        co_return;
                    }
                    req.path = util::url_decode(tokens[0]);
                    if (tokens.size() >= 2) {
                        bool is_valid = true;
                        req.query_params =
                            html::parse_http_query_params(tokens[1], is_valid);
                        if (!is_valid) {
                            resp.set_empty_content(http::status::bad_request);
                            co_return;
                        }
                    }

                    auto start_time = std::chrono::steady_clock::now();

                    co_await router_.routing(req, resp);

                    auto span_time = std::chrono::steady_clock::now() - start_time;

                    logger_->info(
                        "{} {} ({} -> {}) {} {}ms",
                        req.method_string(),
                        req.target(),
                        remote_endpoint.address().to_string(),
                        local_endpoint.address().to_string(),
                        resp.result_int(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(span_time)
                            .count());
                }

                for (const auto& encoding :
                     util::split(req[http::field::accept_encoding], ",")) {
                    if (body::compressor_factory::instance().is_supported_encoding(
                            encoding)) {
                        resp.set(http::field::content_encoding, encoding);
                        resp.chunked(true);
                        break;
                    }
                }

                if (!resp.has_content_length()) resp.prepare_payload();

                co_await http::async_write(*http_variant_stream, resp, net_awaitable[ec]);
                if (ec) {
                    logger_->trace("write http body failed: {}", ec.message());
                    co_return;
                }

                if (!resp.keep_alive()) {
                    // This means we should close the connection, usually
                    // because the response indicated the "Connection: close"
                    // semantic.
                    boost::system::error_code ec;
                    http_variant_stream->shutdown(net::socket_base::shutdown_send, ec);
                    co_return;
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("do_session: {}", e.what());
        }
    }
    net::awaitable<void>
    handle_connect(http_variant_stream_type&& http_variant_stream, request&& req)
    {
        auto target = req.target();
        auto pos    = target.find(":");
        if (pos == std::string_view::npos) co_return;

        auto host = target.substr(0, pos);
        auto port = target.substr(pos + 1);

        boost::system::error_code ec;
        tcp::resolver resolver(co_await net::this_coro::executor);
        auto results = co_await resolver.async_resolve(host, port, net_awaitable[ec]);
        if (ec) co_return;

        tcp::socket proxy_socket(co_await net::this_coro::executor);
        co_await net::async_connect(proxy_socket, results, net_awaitable[ec]);
        if (ec) co_return;

        auto resp = detail::make_respone(req);
        resp.reason("Connection Established");
        resp.result(http::status::ok);
        co_await http::async_write(http_variant_stream, resp, net_awaitable[ec]);
        if (ec) co_return;

        auto conn = std::make_shared<httplib::proxy_conn>(std::move(http_variant_stream),
                                                          std::move(proxy_socket));

        co_await conn->run();
        co_return;
    }
#ifdef HTTPLIB_ENABLED_WEBSOCKET
    net::awaitable<void>
    handle_websocket(http_variant_stream_type&& http_variant_stream, request&& req)
    {
        auto conn = std::make_shared<httplib::websocket_conn_impl>(
            logger_, std::move(http_variant_stream));
        conn->set_open_handler(websocket_open_handler_);
        conn->set_close_handler(websocket_close_handler_);
        conn->set_message_handler(websocket_message_handler_);
        co_await conn->run(req);
        co_return;
    }
#endif //  HTTPLIB_ENABLED_WEBSOCKET
};

server::server(uint32_t num_threads /*= std::thread::hardware_concurrency()*/)
    : impl_(std::make_shared<impl>(num_threads))
{
}

server::~server() { }

net::any_io_executor
server::get_executor() noexcept
{
    return impl_->pool_.get_executor();
}

std::shared_ptr<spdlog::logger>
server::get_logger() noexcept
{
    return impl_->logger_;
}

void
server::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    impl_->logger_ = logger;
}

void
server::set_ssl_config(const ssl_config& config)
{
    impl_->ssl_config_ = config;
}

server&
server::listen(std::string_view host,
               uint16_t port,
               int backlog /*= net::socket_base::max_listen_connections*/)
{
    impl_->listen(host, port, backlog);
    return *this;
}

void
server::run()
{
    async_run();
    impl_->pool_.wait();
}

void
server::async_run()
{
    net::co_spawn(impl_->pool_, impl_->do_listen(), net::detached);
}
void
server::set_websocket_message_handler(
    httplib::websocket_conn::message_handler_type&& handler)
{
    impl_->websocket_message_handler_ = handler;
}
void
server::set_websocket_open_handler(httplib::websocket_conn::open_handler_type&& handler)
{
    impl_->websocket_open_handler_ = handler;
}
void
server::set_websocket_close_handler(httplib::websocket_conn::close_handler_type&& handler)
{
    impl_->websocket_close_handler_ = handler;
}
router&
server::get_router()
{
    return impl_->router_;
}
} // namespace httplib