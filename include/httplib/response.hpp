#pragma once
#include "variant_message.hpp"
#include <filesystem>
namespace httplib
{

struct response : public http::response<body::any_body>
{
    using http::response<body::any_body>::message;

public:
    void set_string_content(std::string_view data,
                            std::string_view content_type,
                            http::status status = http::status::ok);
    void set_string_content(std::string&& data, std::string_view content_type, http::status status = http::status::ok);
    void set_json_content(const body::json_body::value_type& data, http::status status = http::status::ok);
    void set_json_content(body::json_body::value_type&& data, http::status status = http::status::ok);
    void set_file_content(const std::filesystem::path& path);
    void set_file_content(const std::filesystem::path& path, const http_ranges& ranges);
};

} // namespace httplib