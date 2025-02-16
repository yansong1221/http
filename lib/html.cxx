#include "httplib/html.h"

namespace httplib::html {
namespace detail {

inline constexpr auto head_fmt =
    LR"(<html><head><meta charset="UTF-8"><title>Index of {}</title></head><body bgcolor="white"><h1>Index of {}</h1><hr><pre>)";
inline constexpr auto tail_fmt = L"</pre><hr></body></html>";
inline constexpr auto body_fmt = L"<a href=\"{}\">{}</a>{} {}       {}\r\n";

template<typename Path>
inline std::string make_unc_path(const Path &path) {
    auto ret = path.string();

#ifdef WIN32
    if (ret.size() > MAX_PATH) {
        boost::replace_all(ret, "/", "\\");
        return "\\\\?\\" + ret;
    }
#endif

    return ret;
}
inline std::tuple<std::string, fs::path> file_last_wirte_time(const fs::path &file) {
    static auto loc_time = [](auto t) -> struct tm * {
        using time_type = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<time_type, std::filesystem::file_time_type>) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                t - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            auto time = std::chrono::system_clock::to_time_t(sctp);
            return std::localtime(&time);
        } else if constexpr (std::is_same_v<time_type, std::time_t>) {
            return std::localtime(&t);
        } else {
            static_assert(!std::is_same_v<time_type, time_type>, "time type required!");
        }
    };

    boost::system::error_code ec;
    std::string time_string;
    fs::path unc_path;

    auto ftime = fs::last_write_time(file, ec);
    if (ec) {
#ifdef WIN32
        if (file.string().size() > MAX_PATH) {
            unc_path = make_unc_path(file);
            ftime = fs::last_write_time(unc_path, ec);
        }
#endif
    }

    if (!ec) {
        auto tm = loc_time(ftime);

        char tmbuf[64] = {0};
        std::strftime(tmbuf, sizeof(tmbuf), "%m-%d-%Y %H:%M", tm);

        time_string = tmbuf;
    }

    return {time_string, unc_path};
}
inline std::vector<std::wstring> format_path_list(const fs::path &path,
                                                  boost::system::error_code &ec) {
    fs::directory_iterator end;
    fs::directory_iterator it(path, ec);
    if (ec) {
        return {};
    }

    std::vector<std::wstring> path_list;
    std::vector<std::wstring> file_list;

    for (; it != end; it++) {
        const auto &item = it->path();

        auto [ftime, unc_path] = file_last_wirte_time(item);
        std::wstring time_string = strutil::string_to_wstring(ftime);

        std::wstring rpath;

        if (fs::is_directory(unc_path.empty() ? item : unc_path, ec)) {
            auto leaf = strutil::wstring_to_string(item.filename().wstring());
            leaf = leaf + "/";
            rpath = strutil::string_to_wstring(leaf);
            int width = 50 - static_cast<int>(rpath.size());
            width = width < 0 ? 0 : width;
            std::wstring space(width, L' ');
            auto show_path = rpath;
            if (show_path.size() > 50) {
                show_path = show_path.substr(0, 47);
                show_path += L"..&gt;";
            }
            auto str = std::format(body_fmt, rpath, show_path, space, time_string, L"-");

            path_list.push_back(str);
        } else {
            auto leaf = strutil::wstring_to_string(item.filename().wstring());
            rpath = strutil::string_to_wstring(leaf);
            int width = 50 - (int)rpath.size();
            width = width < 0 ? 0 : width;
            std::wstring space(width, L' ');
            std::wstring filesize;
            if (unc_path.empty())
                unc_path = item;
            auto sz = static_cast<float>(fs::file_size(unc_path, ec));
            if (ec)
                sz = 0;
            filesize = strutil::string_to_wstring(strutil::add_suffix(sz));
            auto show_path = rpath;
            if (show_path.size() > 50) {
                show_path = show_path.substr(0, 47);
                show_path += L"..&gt;";
            }
            auto str = std::format(body_fmt, rpath, show_path, space, time_string, filesize);

            file_list.push_back(str);
        }
    }

    ec = {};

    path_list.insert(path_list.end(), file_list.begin(), file_list.end());

    return path_list;
}
inline std::wstring make_target_path(std::string_view target) {
    std::string url = "http://example.com";
    if (target.starts_with("/"))
        url += target;
    else {
        url += "/";
        url += target;
    }

    auto result = boost::urls::parse_uri(url);
    if (result.has_error())
        return strutil::string_to_wstring(target);

    return strutil::string_to_wstring(result->path());
}
} // namespace detail

std::string format_dir_to_html(std::string_view target, const fs::path &path,
                               boost::system::error_code ec) {
    auto path_list = detail::format_path_list(path, ec);
    if (ec)
        return {};

    auto target_path = detail::make_target_path(target);
    std::wstring head = std::format(detail::head_fmt, target_path, target_path);

    std::wstring body = std::format(detail::body_fmt, L"../", L"../", L"", L"", L"");

    for (auto &s : path_list)
        body += s;
    body = head + body + detail::tail_fmt;

    return strutil::wstring_to_string(body);
}

std::string fromat_error_content(int status, std::string_view reason, std::string_view server) {
    return std::format(
        R"x*x*x(<html>
<head><title>{0} {1}</title></head>
<body bgcolor="white">
<center><h1>{0} {1}</h1></center>
<hr><center>{2}</center>
</body>
</html>)x*x*x",
        status, reason, server);
}

std::string format_http_date() {
    using namespace std::chrono;

    auto now = utc_clock::now();
    std::time_t tt = system_clock::to_time_t(utc_clock::to_sys(now));
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    return oss.str();
}

http_ranges parser_http_ranges(std::string_view range) noexcept { // 去掉前后空白.
    range = boost::trim_copy(range);

    // range 必须以 bytes= 开头, 否则返回空数组.
    if (!range.starts_with("bytes=")) 
        return {};

    // 去掉开头的 bytes= 字符串.
    range.remove_prefix(6);

    http_ranges results;

    // 获取其中所有 range 字符串.
    auto ranges = strutil::split(range, ",");
    for (const auto &str : ranges) {
        auto r = strutil::split(std::string(str), "-");

        // range 只有一个数值.
        if (r.size() == 1) {
            if (str.front() == '-') {
                auto pos = std::atoll(r.front().data());
                results.emplace_back(-1, pos);
            } else {
                auto pos = std::atoll(r.front().data());
                results.emplace_back(pos, -1);
            }
        } else if (r.size() == 2) {
            // range 有 start 和 end 的情况, 解析成整数到容器.
            auto &start_str = r[0];
            auto &end_str = r[1];

            if (start_str.empty() && !end_str.empty()) {
                auto end = std::atoll(end_str.data());
                results.emplace_back(-1, end);
            } else {
                auto start = std::atoll(start_str.data());
                auto end = std::atoll(end_str.data());
                if (end_str.empty())
                    end = -1;

                results.emplace_back(start, end);
            }
        } else {
            // 在一个 range 项中不应该存在3个'-', 否则则是无效项.
            return {};
        }
    }

    return results;
}

} // namespace httplib::html