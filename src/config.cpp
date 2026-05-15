#include "config.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace rtsp_relay {

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

ServerConfig ConfigParser::parseString(const std::string& content) {
    ServerConfig cfg;
    std::istringstream iss(content);
    std::string line;
    std::string section;

    while (std::getline(iss, line)) {
        line = trim(line);

        // 空行或注释
        if (line.empty() || line[0] == '#') continue;

        // section
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos) {
                section = trim(line.substr(1, end - 1));
            }
            continue;
        }

        // key=value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (section == "server") {
            if (key == "rtsp_port")           cfg.rtsp_port = static_cast<uint16_t>(std::atoi(val.c_str()));
            else if (key == "http_port")      cfg.http_port = static_cast<uint16_t>(std::atoi(val.c_str()));
            else if (key == "io_threads")     cfg.io_threads = std::atoi(val.c_str());
            else if (key == "backlog")        cfg.backlog = std::atoi(val.c_str());
            else if (key == "max_connections") cfg.max_connections = static_cast<size_t>(std::atoll(val.c_str()));
            else if (key == "max_sources")    cfg.max_sources = static_cast<size_t>(std::atoll(val.c_str()));
            else if (key == "stats_interval_sec") cfg.stats_interval_sec = std::atoi(val.c_str());
        }
    }

    return cfg;
}

ServerConfig ConfigParser::parse(const std::string& file_path) {
    std::ifstream ifs(file_path);
    if (!ifs.is_open()) {
        fprintf(stderr, "[Config] cannot open config file: %s, using defaults\n",
                file_path.c_str());
        ServerConfig cfg;
        return cfg;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    return parseString(content);
}

} // namespace rtsp_relay
