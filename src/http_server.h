#pragma once

#include "event_loop.h"
#include "tcp_connection.h"
#include "tcp_server.h"
#include <memory>
#include <functional>
#include <string>
#include <unordered_map>

namespace rtsp_relay {

// ==================== HttpRequest ====================

struct HttpRequest {
    std::string method;     // GET / POST / DELETE / PUT
    std::string path;       // 请求路径，如 /api/streams
    std::string version;    // HTTP/1.1
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // 便捷方法
    std::string header(const std::string& key, const std::string& default_val = "") const {
        auto it = headers.find(key);
        return (it != headers.end()) ? it->second : default_val;
    }
};

// ==================== HttpResponse ====================

struct HttpResponse {
    int status_code = 200;
    std::string reason = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    void setJson(const std::string& json) {
        headers["Content-Type"] = "application/json; charset=utf-8";
        body = json;
    }

    void setStatus(int code, const std::string& r) {
        status_code = code;
        reason = r;
    }

    std::string serialize() const;
};

// ==================== HttpServer ====================

class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
    using Ptr = std::shared_ptr<HttpServer>;

    HttpServer(EventLoop* loop, uint16_t port, int backlog = 256);
    ~HttpServer();

    void start();

    /// 注册路由处理器
    /// method: "GET" / "POST" / "DELETE" / "PUT"
    /// path: 精确匹配路径，如 "/api/streams"
    void route(const std::string& method, const std::string& path, Handler handler);

    /// 设置默认处理器（无精确匹配时调用）
    void setDefaultHandler(Handler handler);

private:
    void onConnection(const TcpConnection::Ptr& conn);
    void onMessage(const TcpConnection::Ptr& conn, ByteBuffer& buf);
    bool parseRequest(ByteBuffer& buf, HttpRequest& req, size_t& consumed);
    void dispatch(const TcpConnection::Ptr& conn, const HttpRequest& req);
    std::string extractPathParam(const std::string& pattern, const std::string& actual) const;

    struct Route {
        std::string method;
        std::string path;
        Handler handler;
    };

    EventLoop* loop_;
    uint16_t port_;
    int backlog_;
    std::unique_ptr<TcpServer> server_;

    std::vector<Route> routes_;
    Handler default_handler_;
};

} // namespace rtsp_relay
