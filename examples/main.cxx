
#include "httplib/server.hpp"
#include <filesystem>
#include <format>
#include <iostream>
//日志切面
struct log_t {
    httplib::net::awaitable<bool> before(httplib::request &req, httplib::response &res) {
        start_ = std::chrono::steady_clock::now();
        co_return true;
    }

    bool after(httplib::request &req, httplib::response &res) {
        auto span = std::chrono::steady_clock::now() - start_;
        std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(span) << std::endl;
        return true;
    }

private:
    std::chrono::steady_clock::time_point start_;
};
int main() { // HTTP
    using namespace std::string_view_literals;
    httplib::server svr;
    svr.listen("127.0.0.1", 8808);
    svr.set_websocket_open_handler(
        [](httplib::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> { co_return; });
    svr.set_websocket_close_handler(
        [](httplib::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> { co_return; });
    svr.set_websocket_message_handler(
        [](httplib::websocket_conn::weak_ptr hdl,
           httplib::websocket_conn::message msg) -> boost::asio::awaitable<void> {
            auto conn = hdl.lock();
            conn->send_message(msg);
            co_return;
        });
    /*   svr.set_http_handler<httplib::http::verb::post, httplib::http::verb::get>(
        "/中文",
        [](httplib::request &req, httplib::response &resp) -> httplib::net::awaitable<void> {
            req.is_body_type<httplib::body::form_data_body>();

            resp.base().result(httplib::http::status::ok);
            resp.set_body<httplib::body::form_data_body>(req.body<httplib::body::form_data_body>());
            co_return;
        },
        log_t{});*/
    auto &router = svr.get_router();
    router.set_http_handler<httplib::http::verb::post>(
        "/json",
        [](httplib::request &req, httplib::response &resp) -> httplib::net::awaitable<void> {
            auto &doc = req.body<httplib::body::json_body>();

 /*           const auto &obj = doc.get_object();
            for (const auto &item : obj.at("statuses").as_array()) {
                std::string_view created_at = item.at("created_at").as_string();
                resp.set_string_content(std::string(created_at), "text/html");
                co_return;
            }*/
            resp.set_json_content(doc);
            co_return;
        },
        log_t{});
    router.set_default_handler([](httplib::request &req, httplib::response &resp) {
        resp.set_string_content("1000"sv, "text/html");
    });
    //svr.set_http_handler<httplib::http::verb::post, httplib::http::verb::get>(
    //    "/hello/:w",
    //    [](httplib::request &req, httplib::response &resp) {
    //        req.is_body_type<httplib::body::form_data_body>();

    //        resp.base().result(httplib::http::status::ok);
    //        resp.set_string_content("1000", "text/html");
    //        return;
    //    },
    //    log_t{});
    //svr.set_http_handler<httplib::http::verb::post>(
    //    "/",
    //    [](httplib::request &req, httplib::response &resp) -> boost::asio::awaitable<void> {
    //        req.is_body_type<httplib::form_data_body>();

    //        resp.base().result(httplib::http::status::ok);
    //        resp.set_body<httplib::form_data_body>(req.body<httplib::form_data_body>());
    //        co_return;
    //    },
    //    log_t{});
    router.set_mount_point("/", R"(D:\)");

    
    svr.run();
}