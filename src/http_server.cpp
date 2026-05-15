#include "http_server.h"
#include <cstdio>
#include <cstring>
#include <sstream>

namespace rtsp_relay {

// ==================== HttpResponse ====================

std::string HttpResponse::serialize() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << reason << "\r\n";

    // 自动计算 Content-Length
    bool has_content_length = false;
    for (auto& [key, _] : headers) {
        if (key == "Content-Length") { has_content_length = true; break; }
    }
    if (!has_content_length && !body.empty()) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }

    // Connection: close（简单短连接模型）
    if (headers.find("Connection") == headers.end()) {
        oss << "Connection: close\r\n";
    }

    for (auto& [key, val] : headers) {
        oss << key << ": " << val << "\r\n";
    }

    oss << "\r\n";
    if (!body.empty()) {
        oss << body;
    }

    return oss.str();
}

// ==================== HttpServer ====================

HttpServer::HttpServer(EventLoop* loop, uint16_t port, int backlog)
    : loop_(loop), port_(port), backlog_(backlog),
      server_(std::make_unique<TcpServer>(loop, port, backlog)) {
}

HttpServer::~HttpServer() = default;

void HttpServer::start() {
    server_->setConnectionCallback([this](const TcpConnection::Ptr& conn) {
        // 在连接回调中同时设置消息回调
        conn->setMessageCallback([this](const TcpConnection::Ptr& c, ByteBuffer& buf) {
            onMessage(c, buf);
        });
        onConnection(conn);
    });

    server_->start();
    printf("[HttpServer] listening on port %u\n", port_);
}

void HttpServer::route(const std::string& method, const std::string& path, Handler handler) {
    routes_.push_back({method, path, std::move(handler)});
}

void HttpServer::setDefaultHandler(Handler handler) {
    default_handler_ = std::move(handler);
}

void HttpServer::onConnection(const TcpConnection::Ptr& conn) {
    printf("[HttpServer] %s connection from %s\n",
           conn->connected() ? "new" : "close", conn->peerAddr().c_str());
}

void HttpServer::onMessage(const TcpConnection::Ptr& conn, ByteBuffer& buf) {
    while (buf.readable() > 0) {
        HttpRequest req;
        size_t consumed = 0;
        if (!parseRequest(buf, req, consumed)) {
            return; // 数据不完整
        }
        buf.skip(consumed);
        dispatch(conn, req);
    }
}

bool HttpServer::parseRequest(ByteBuffer& buf, HttpRequest& req, size_t& consumed) {
    consumed = 0;

    // 找到 header 结束
    int headerEnd = buf.find("\r\n\r\n", 4);
    if (headerEnd < 0) return false;

    size_t headerLen = headerEnd + 4;
    std::string header(buf.readPtr(), headerLen);

    // 解析请求行
    size_t lineEnd = header.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::string requestLine = header.substr(0, lineEnd);

    // GET /api/streams HTTP/1.1
    size_t sp1 = requestLine.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = requestLine.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    req.method = requestLine.substr(0, sp1);
    req.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = requestLine.substr(sp2 + 1);

    // 解析 headers
    size_t pos = lineEnd + 2;
    while (pos < headerLen - 2) {
        size_t nextLine = header.find("\r\n", pos);
        if (nextLine == std::string::npos || nextLine == pos) break;
        std::string line = header.substr(pos, nextLine - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            size_t valStart = colon + 1;
            while (valStart < line.size() && line[valStart] == ' ') ++valStart;
            std::string val = line.substr(valStart);
            req.headers[key] = val;
        }
        pos = nextLine + 2;
    }

    // 解析 body
    size_t contentLength = 0;
    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) {
        contentLength = static_cast<size_t>(std::atoll(it->second.c_str()));
    }

    if (headerLen + contentLength > buf.readable()) {
        return false; // body 不完整
    }

    if (contentLength > 0) {
        req.body = std::string(buf.readPtr() + headerLen, contentLength);
    }

    consumed = headerLen + contentLength;
    return true;
}

void HttpServer::dispatch(const TcpConnection::Ptr& conn, const HttpRequest& req) {
    printf("[HttpServer] %s %s\n", req.method.c_str(), req.path.c_str());

    HttpResponse resp;

    // 路由匹配：先精确匹配，再尝试路径前缀匹配
    bool matched = false;
    for (auto& route : routes_) {
        if (route.method != req.method) continue;

        // 精确匹配
        if (route.path == req.path) {
            route.handler(req, resp);
            matched = true;
            break;
        }

        // 路径前缀匹配（带尾段参数）：如 /api/streams/ 匹配 /api/streams/xxx
        if (req.path.size() > route.path.size() &&
            req.path.compare(0, route.path.size(), route.path) == 0 &&
            (route.path.back() == '/' || req.path[route.path.size()] == '/')) {
            route.handler(req, resp);
            matched = true;
            break;
        }
    }

    if (!matched) {
        if (default_handler_) {
            default_handler_(req, resp);
        } else {
            resp.setStatus(404, "Not Found");
            resp.setJson("{\"error\":\"not found\"}");
        }
    }

    conn->send(resp.serialize());

    // 短连接模式，发送完就关写端
    conn->shutdown();
}

} // namespace rtsp_relay
