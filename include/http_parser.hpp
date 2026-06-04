/**
 * @file http_parser.hpp
 * @brief HTTP 请求/响应解析器
 */
#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <algorithm>
#include <charconv>

namespace high_perf {

/**
 * @brief HTTP 方法
 */
enum class HttpMethod {
    GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, CONNECT, TRACE,
    UNKNOWN
};

/**
 * @brief HTTP 版本
 */
enum class HttpVersion {
    HTTP_1_0,
    HTTP_1_1,
    HTTP_2,
    UNKNOWN
};

/**
 * @brief HTTP 请求
 */
struct HttpRequest {
    HttpMethod method = HttpMethod::UNKNOWN;
    HttpVersion version = HttpVersion::UNKNOWN;
    std::string uri;           // 原始 URI
    std::string path;          // 路径部分
    std::string query_string;  // 查询字符串
    std::string fragment;      // #fragment

    // 头部
    std::unordered_map<std::string, std::string> headers;
    // URL 参数
    std::unordered_map<std::string, std::string> params;

    // 主体（用于 POST 等）
    std::string body;

    bool keep_alive = false;

    void clear() {
        method = HttpMethod::UNKNOWN;
        version = HttpVersion::UNKNOWN;
        uri.clear(); path.clear(); query_string.clear(); fragment.clear();
        headers.clear(); params.clear(); body.clear();
        keep_alive = false;
    }
};

/**
 * @brief HTTP 响应
 */
struct HttpResponse {
    HttpVersion version = HttpVersion::HTTP_1_1;
    int status_code = 200;
    std::string reason_phrase;

    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // 快捷方法
    void set_content_type(const std::string& ct) {
        headers["Content-Type"] = ct;
    }

    void set_content_length(size_t len) {
        headers["Content-Length"] = std::to_string(len);
    }

    void set_keep_alive(bool keep) {
        headers["Connection"] = keep ? "keep-alive" : "close";
    }

    void set_server(const std::string& sv = "HighPerfServer/1.0") {
        headers["Server"] = sv;
    }

    std::string build_header() const;
};

/**
 * @brief HTTP 解析器
 */
class HttpParser {
public:
    /**
     * @brief 解析请求
     * @param data     原始数据
     * @param len      数据长度
     * @param request  输出解析结果
     * @return 解析的字节数，-1 表示失败，0 表示数据不完整
     */
    static int parse_request(const char* data, size_t len, HttpRequest& request);

    /**
     * @brief 检查是否包含完整请求（以 \r\n\r\n 结尾）
     */
    static bool is_complete(const char* data, size_t len);

    /**
     * @brief 获取 HTTP 方法字符串
     */
    static const char* method_string(HttpMethod method);

    /**
     * @brief 获取方法枚举
     */
    static HttpMethod parse_method(const std::string& method);

    /**
     * @brief URL 解码
     */
    static std::string url_decode(const std::string& encoded);

    /**
     * @brief URL 编码
     */
    static std::string url_encode(const std::string& raw);
};

// ==================== 实现 ====================

inline /* static */ int HttpParser::parse_request(
    const char* data,
    size_t len,
    HttpRequest& request
) {
    if (len == 0) return 0;

    // 1. 解析请求行
    const char* line_end = find_crlf(data, len);
    if (!line_end) return 0;

    std::string request_line(data, line_end - data);

    // 解析: METHOD URI HTTP/VERSION
    std::vector<std::string> parts = split(request_line, ' ');
    if (parts.size() < 3) return -1;

    request.method = parse_method(parts[0]);
    request.uri = parts[1];
    request.version = parse_http_version(parts[2]);

    // 解析 URI
    size_t query_pos = request.uri.find('?');
    if (query_pos != std::string::npos) {
        request.path = request.uri.substr(0, query_pos);
        request.query_string = request.uri.substr(query_pos + 1);
        // 解析 query params
        parse_query_string(request.query_string, request.params);
    } else {
        request.path = request.uri;
    }

    size_t fragment_pos = request.path.find('#');
    if (fragment_pos != std::string::npos) {
        request.fragment = request.path.substr(fragment_pos + 1);
        request.path = request.path.substr(0, fragment_pos);
    }

    // 2. 解析头部
    const char* header_start = line_end + 2; // 跳过 \r\n
    const char* body_start = find_double_crlf(header_start, len - (header_start - data));

    if (!body_start) {
        // 没有 body，检查是否至少收到了所有 header
        const char* headers_end = find_double_crlf(header_start, len - (header_start - data));
        if (!headers_end) return 0; // 数据不完整
    }

    const char* header_end = body_start ? body_start : find_double_crlf(header_start, len - (header_start - data));
    if (!header_end) return 0;

    parse_headers(header_start, header_end - header_start, request.headers);

    // 3. 解析 body（如果有）
    if (body_start) {
        const char* body_begin = body_start + 4; // 跳过 \r\n\r\n
        size_t body_len = len - (body_begin - data);
        request.body.assign(body_begin, body_len);

        // 如果是 POST，解析 body 中的参数
        if (request.method == HttpMethod::POST) {
            auto it = request.headers.find("Content-Type");
            if (it != request.headers.end() &&
                it->second.find("application/x-www-form-urlencoded") != std::string::npos) {
                parse_query_string(request.body, request.params);
            }
        }
    }

    // 4. Keep-Alive
    auto conn_it = request.headers.find("Connection");
    if (conn_it != request.headers.end()) {
        request.keep_alive = (conn_it->second.find("keep-alive") != std::string::npos);
    } else {
        request.keep_alive = (request.version == HttpVersion::HTTP_1_1);
    }

    // URL 解码 path
    request.path = url_decode(request.path);

    return body_start ? (body_start + 4 + request.body.size() - data)
                      : (header_end + 2 - data);
}

inline /* static */ bool HttpParser::is_complete(const char* data, size_t len) {
    return len >= 4 && memmem(data, len, "\r\n\r\n", 4) != nullptr;
}

inline /* static */ const char* HttpParser::method_string(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::PATCH:   return "PATCH";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::CONNECT: return "CONNECT";
        case HttpMethod::TRACE:   return "TRACE";
        default:                 return "UNKNOWN";
    }
}

inline /* static */ HttpMethod HttpParser::parse_method(const std::string& method) {
    if (method == "GET")    return HttpMethod::GET;
    if (method == "POST")   return HttpMethod::POST;
    if (method == "PUT")    return HttpMethod::PUT;
    if (method == "DELETE") return HttpMethod::DELETE;
    if (method == "PATCH")  return HttpMethod::PATCH;
    if (method == "HEAD")   return HttpMethod::HEAD;
    if (method == "OPTIONS") return HttpMethod::OPTIONS;
    if (method == "CONNECT") return HttpMethod::CONNECT;
    if (method == "TRACE")  return HttpMethod::TRACE;
    return HttpMethod::UNKNOWN;
}

inline HttpVersion parse_http_version(const std::string& ver) {
    if (ver == "HTTP/1.1") return HttpVersion::HTTP_1_1;
    if (ver == "HTTP/1.0") return HttpVersion::HTTP_1_0;
    if (ver == "HTTP/2")   return HttpVersion::HTTP_2;
    return HttpVersion::UNKNOWN;
}

inline std::string HttpParser::url_decode(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int value;
            std::from_chars(encoded.c_str() + i + 1,
                           encoded.c_str() + i + 3, value, 16);
            decoded += static_cast<char>(value);
            i += 2;
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    return decoded;
}

inline std::string HttpParser::url_encode(const std::string& raw) {
    std::string encoded;
    encoded.reserve(raw.size() * 2);

    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0F];
        }
    }
    return encoded;
}

inline void parse_headers(const char* data, size_t len,
                          std::unordered_map<std::string, std::string>& headers) {
    const char* end = data + len;
    const char* line_start = data;

    while (line_start < end) {
        const char* line_end = strchr(line_start, '\r');
        if (!line_end || line_end >= end) break;

        if (line_end == line_start) {
            // 空行，header 结束
            break;
        }

        // 解析一行: Key: Value
        const char* colon = strchr(line_start, ':');
        if (colon && colon < line_end) {
            std::string key(line_start, colon - line_start);

            // 跳过 ": "
            const char* value_start = colon + 1;
            while (value_start < line_end && (*value_start == ' ' || *value_start == '\t')) {
                ++value_start;
            }

            std::string value(value_start, line_end - value_start);

            // 转小写 key
            std::transform(key.begin(), key.end(), key.begin(),
                         [](unsigned char c){ return std::tolower(c); });

            headers[key] = value;
        }

        line_start = line_end + 2; // 跳过 \r\n
    }
}

inline void parse_query_string(
    const std::string& query,
    std::unordered_map<std::string, std::string>& params
) {
    size_t start = 0;
    while (start < query.size()) {
        size_t eq = query.find('=', start);
        size_t amp = query.find('&', start);

        if (amp == std::string::npos) amp = query.size();

        if (eq != std::string::npos && eq < amp) {
            std::string key = query.substr(start, eq - start);
            std::string value = query.substr(eq + 1, amp - eq - 1);
            params[HttpParser::url_decode(key)] = HttpParser::url_decode(value);
        }

        start = amp + 1;
    }
}

inline const char* find_crlf(const char* data, size_t len) {
    return static_cast<const char*>(memmem(data, len, "\r\n", 2));
}

inline const char* find_double_crlf(const char* data, size_t len) {
    return static_cast<const char*>(memmem(data, len, "\r\n\r\n", 4));
}

inline std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string part;
    while (std::getline(iss, part, delim)) {
        parts.push_back(part);
    }
    return parts;
}

inline std::string HttpResponse::build_header() const {
    std::string header;
    header.reserve(512);

    header += (version == HttpVersion::HTTP_1_1 ? "HTTP/1.1 " : "HTTP/1.0 ");
    header += std::to_string(status_code);
    header += " ";
    header += reason_phrase.empty() ? status_message(status_code) : reason_phrase;
    header += "\r\n";

    for (const auto& [key, val] : headers) {
        header += key;
        header += ": ";
        header += val;
        header += "\r\n";
    }

    header += "\r\n";
    return header;
}

inline const char* status_message(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

} // namespace high_perf

#endif // HTTP_PARSER_HPP
