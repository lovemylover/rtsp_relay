#include "event_loop.h"
#include "tcp_server.h"
#include "http_server.h"
#include "rtsp_session.h"
#include "relay_manager.h"
#include "config.h"

#include <cstdio>
#include <csignal>
#include <atomic>
#include <sstream>

using namespace rtsp_relay;

static EventLoop* g_main_loop = nullptr;

static void signalHandler(int sig) {
    printf("\n[Signal] caught signal %d, shutting down...\n", sig);
    if (g_main_loop) {
        g_main_loop->quit();
    }
}

// ==================== 简易 JSON 转义 ====================
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

// ==================== 注册 HTTP API 路由 ====================
static void registerHttpRoutes(HttpServer& http, RelayManager& relay_manager, const ServerConfig& cfg) {

    // ---- POST /api/streams ---- 添加流（动态拉流）
    // Body: {"path":"/stream1","upstream":"rtsp://..."}
    http.route("POST", "/api/streams", [&relay_manager, cfg](const HttpRequest& req, HttpResponse& resp) {
        // 简易 JSON 解析：提取 path 和 upstream
        auto extractStringValue = [](const std::string& json, const std::string& key) -> std::string {
            std::string pattern = "\"" + key + "\"";
            size_t pos = json.find(pattern);
            if (pos == std::string::npos) return "";
            pos = json.find(':', pos + pattern.size());
            if (pos == std::string::npos) return "";
            // 跳过空白
            size_t valStart = json.find('"', pos + 1);
            if (valStart == std::string::npos) return "";
            size_t valEnd = json.find('"', valStart + 1);
            if (valEnd == std::string::npos) return "";
            return json.substr(valStart + 1, valEnd - valStart - 1);
        };

        std::string path = extractStringValue(req.body, "path");
        std::string upstream = extractStringValue(req.body, "upstream");

        if (path.empty() || upstream.empty()) {
            resp.setStatus(400, "Bad Request");
            resp.setJson("{\"error\":\"missing 'path' or 'upstream' field\"}");
            return;
        }

        // 规范化 path：确保以 / 开头
        if (path[0] != '/') path = "/" + path;

        bool ok = relay_manager.addStream(path, upstream);
        if (!ok) {
            resp.setStatus(409, "Conflict");
            resp.setJson("{\"error\":\"stream already exists or max sources reached\"}");
            return;
        }

        resp.setStatus(201, "Created");
        // 构造代理流播放地址
        std::string proxy_url = "rtsp://<server_ip>:" + std::to_string(cfg.rtsp_port) + path;
        resp.setJson("{\"code\":0,\"message\":\"stream added\",\"data\":{"
                     "\"path\":\"" + jsonEscape(path) + "\","
                     "\"upstream\":\"" + jsonEscape(upstream) + "\","
                     "\"proxy_url\":\"" + jsonEscape(proxy_url) + "\"}}");
    });

    // ---- DELETE /api/streams ---- 删除流
    // Body: {"path":"/stream1"} 或通过 path 查询参数
    http.route("DELETE", "/api/streams", [&relay_manager](const HttpRequest& req, HttpResponse& resp) {
        auto extractStringValue = [](const std::string& json, const std::string& key) -> std::string {
            std::string pattern = "\"" + key + "\"";
            size_t pos = json.find(pattern);
            if (pos == std::string::npos) return "";
            pos = json.find(':', pos + pattern.size());
            if (pos == std::string::npos) return "";
            size_t valStart = json.find('"', pos + 1);
            if (valStart == std::string::npos) return "";
            size_t valEnd = json.find('"', valStart + 1);
            if (valEnd == std::string::npos) return "";
            return json.substr(valStart + 1, valEnd - valStart - 1);
        };

        std::string path = extractStringValue(req.body, "path");

        // 也支持从 URL 查询参数读: DELETE /api/streams?path=/stream1
        if (path.empty()) {
            size_t qpos = req.path.find("?path=");
            if (qpos != std::string::npos) {
                path = req.path.substr(qpos + 6);
            }
        }

        if (path.empty()) {
            resp.setStatus(400, "Bad Request");
            resp.setJson("{\"error\":\"missing 'path' field\"}");
            return;
        }

        bool ok = relay_manager.removeStream(path);
        if (!ok) {
            resp.setStatus(404, "Not Found");
            resp.setJson("{\"error\":\"stream not found\"}");
            return;
        }

        resp.setJson("{\"code\":0,\"message\":\"stream removed\"}");
    });

    // ---- GET /api/streams ---- 查询所有流
    http.route("GET", "/api/streams", [&relay_manager](const HttpRequest& req, HttpResponse& resp) {
        auto infos = relay_manager.getAllStreamInfo();
        std::ostringstream oss;
        oss << "{\"code\":0,\"data\":{\"total\":" << infos.size() << ",\"streams\":[";
        for (size_t i = 0; i < infos.size(); ++i) {
            auto& info = infos[i];
            if (i > 0) oss << ",";
            oss << "{"
                << "\"path\":\"" << jsonEscape(info.stream_path) << "\","
                << "\"upstream\":\"" << jsonEscape(info.upstream_url) << "\","
                << "\"source_state\":\"" << jsonEscape(info.source_state) << "\","
                << "\"sink_count\":" << info.sink_count << ","
                << "\"has_sdp\":" << (info.has_sdp ? "true" : "false")
                << "}";
        }
        oss << "]}}";
        resp.setJson(oss.str());
    });

    // ---- GET /api/streams/ ---- 查询单条流（路径 /api/streams/<path>）
    http.route("GET", "/api/streams/", [&relay_manager](const HttpRequest& req, HttpResponse& resp) {
        // 从请求路径中提取流路径: /api/streams/stream1 -> /stream1
        std::string prefix = "/api/streams";
        std::string stream_path;
        if (req.path.size() > prefix.size()) {
            stream_path = req.path.substr(prefix.size());
        }
        if (stream_path.empty()) {
            // 退化到列表
            auto infos = relay_manager.getAllStreamInfo();
            std::ostringstream oss;
            oss << "{\"code\":0,\"data\":{\"total\":" << infos.size() << ",\"streams\":[";
            for (size_t i = 0; i < infos.size(); ++i) {
                auto& info = infos[i];
                if (i > 0) oss << ",";
                oss << "{"
                    << "\"path\":\"" << jsonEscape(info.stream_path) << "\","
                    << "\"upstream\":\"" << jsonEscape(info.upstream_url) << "\","
                    << "\"source_state\":\"" << jsonEscape(info.source_state) << "\","
                    << "\"sink_count\":" << info.sink_count << ","
                    << "\"has_sdp\":" << (info.has_sdp ? "true" : "false")
                    << "}";
            }
            oss << "]}}";
            resp.setJson(oss.str());
            return;
        }

        // 确保 / 开头
        if (stream_path[0] != '/') stream_path = "/" + stream_path;

        StreamInfo info;
        if (!relay_manager.getStreamInfo(stream_path, info)) {
            resp.setStatus(404, "Not Found");
            resp.setJson("{\"error\":\"stream not found\"}");
            return;
        }

        std::ostringstream oss;
        oss << "{\"code\":0,\"data\":{"
            << "\"path\":\"" << jsonEscape(info.stream_path) << "\","
            << "\"upstream\":\"" << jsonEscape(info.upstream_url) << "\","
            << "\"source_state\":\"" << jsonEscape(info.source_state) << "\","
            << "\"sink_count\":" << info.sink_count << ","
            << "\"has_sdp\":" << (info.has_sdp ? "true" : "false")
            << "}}";
        resp.setJson(oss.str());
    });

    // ---- GET /api/stats ---- 服务整体统计
    http.route("GET", "/api/stats", [&relay_manager](const HttpRequest& req, HttpResponse& resp) {
        std::ostringstream oss;
        oss << "{\"code\":0,\"data\":{"
            << "\"stream_count\":" << relay_manager.streamCount() << ","
            << "\"total_sinks\":" << relay_manager.totalSinks()
            << "}}";
        resp.setJson(oss.str());
    });

    // ---- 默认 404 ----
    http.setDefaultHandler([](const HttpRequest& req, HttpResponse& resp) {
        resp.setStatus(404, "Not Found");
        resp.setJson("{\"error\":\"not found\",\"hint\":\"available: POST /api/streams, DELETE /api/streams, GET /api/streams, GET /api/streams/<path>, GET /api/stats\"}");
    });
}

int main(int argc, char* argv[]) {
    // 解析配置
    std::string config_path = "rtsp_relay.conf";
    if (argc > 1) {
        config_path = argv[1];
    }

    ServerConfig cfg = ConfigParser::parse(config_path);

    printf("========================================\n");
    printf("  RTSP Relay Server v1.0\n");
    printf("========================================\n");
    printf("  RTSP Port:      %u\n", cfg.rtsp_port);
    printf("  HTTP Port:      %u\n", cfg.http_port);
    printf("  IO Threads:     %d\n", cfg.io_threads);
    printf("  Listen Backlog: %d\n", cfg.backlog);
    printf("  Max Conn:       %zu\n", cfg.max_connections);
    printf("  Max Sources:    %zu\n", cfg.max_sources);
    printf("========================================\n");

    // 创建主事件循环
    EventLoop main_loop;
    g_main_loop = &main_loop;

    // 信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 创建转发管理器
    RelayManager relay_manager(&main_loop);
    relay_manager.maxSources = cfg.max_sources;

    // 创建 RTSP 服务器
    TcpServer rtsp_server(&main_loop, cfg.rtsp_port, cfg.backlog);

    // 添加 IO 线程
    for (int i = 0; i < cfg.io_threads; ++i) {
        rtsp_server.addIoThread();
    }
    printf("[Main] started %d IO threads\n", cfg.io_threads);

    // RTSP 连接回调
    rtsp_server.setConnectionCallback([&relay_manager](const TcpConnection::Ptr& conn) {
        printf("[Main] new RTSP connection from %s\n", conn->peerAddr().c_str());
        auto session = std::make_shared<RtspSession>(conn, &relay_manager);
        conn->setContext(static_cast<void*>(session.get()));
        session->start();
    });

    // 创建 HTTP API 服务器（复用主 loop）
    HttpServer http_server(&main_loop, cfg.http_port, 256);
    registerHttpRoutes(http_server, relay_manager, cfg);

    // 定时统计
    main_loop.addTimer(cfg.stats_interval_sec * 1000, [&relay_manager]() {
        std::vector<std::string> stats;
        relay_manager.getStats(stats);
        printf("[Stats] === %zu streams ===\n", stats.size());
        for (auto& s : stats) {
            printf("[Stats]   %s\n", s.c_str());
        }
        printf("[Stats] total sinks: %zu\n", relay_manager.totalSinks());
    }, true);

    // 启动服务
    rtsp_server.start();
    http_server.start();
    printf("[Main] RTSP Relay Server running\n");
    printf("[Main]   RTSP:  port %u\n", cfg.rtsp_port);
    printf("[Main]   HTTP:  port %u\n", cfg.http_port);
    printf("[Main] Press Ctrl+C to stop\n");

    // 进入主事件循环
    main_loop.loop();

    printf("[Main] shutting down...\n");
    g_main_loop = nullptr;
    return 0;
}
