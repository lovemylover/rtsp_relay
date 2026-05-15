#pragma once

#include <string>
#include <cstdint>

namespace rtsp_relay {

// 全局配置
struct ServerConfig {
    uint16_t rtsp_port = 8554;       // RTSP 服务监听端口
    uint16_t http_port = 18080;      // HTTP API 端口
    int io_threads = 4;               // IO 线程数
    int backlog = 1024;               // listen backlog
    size_t max_connections = 2000;    // 最大并发连接数
    size_t max_sources = 500;         // 最大上游拉流路数
    int stats_interval_sec = 10;      // 统计打印间隔(秒)
};

// 简单的配置文件解析器
// 格式(类INI)：
//
// [server]
// rtsp_port=8554
// http_port=18080
// io_threads=4
// backlog=1024
// max_connections=2000
// max_sources=500
// stats_interval_sec=10

class ConfigParser {
public:
    static ServerConfig parse(const std::string& file_path);
    static ServerConfig parseString(const std::string& content);
};

} // namespace rtsp_relay
