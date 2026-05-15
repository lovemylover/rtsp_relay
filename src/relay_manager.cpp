#include "relay_manager.h"
#include <cstdio>
#include <algorithm>

namespace rtsp_relay {

const char* RelayManager::sourceStateToString(RtspSource::State s) {
    switch (s) {
    case RtspSource::State::kIdle:       return "Idle";
    case RtspSource::State::kConnecting: return "Connecting";
    case RtspSource::State::kOptions:    return "Options";
    case RtspSource::State::kDescribe:   return "Describe";
    case RtspSource::State::kSetup:      return "Setup";
    case RtspSource::State::kPlaying:    return "Playing";
    case RtspSource::State::kError:      return "Error";
    default: return "Unknown";
    }
}

RelayManager::RelayManager(EventLoop* loop) : loop_(loop) {
}

RelayManager::~RelayManager() {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& [path, relay] : streams_) {
        if (relay->source) relay->source->stop();
    }
    streams_.clear();
}

bool RelayManager::addStream(const std::string& stream_path, const std::string& upstream_url) {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    if (streams_.count(stream_path)) {
        printf("[RelayManager] stream %s already exists, skipping\n", stream_path.c_str());
        return false;
    }

    if (streams_.size() >= maxSources) {
        printf("[RelayManager] max sources %zu reached, cannot add %s\n",
               maxSources, stream_path.c_str());
        return false;
    }

    auto relay = std::make_unique<StreamRelay>();
    relay->upstream_url = upstream_url;
    auto source = std::make_shared<RtspSource>(loop_, upstream_url, stream_path, this);

    source->setDataCallback([this, stream_path](const uint8_t* data, size_t len) {
        onRtpData(stream_path, data, len);
    });

    relay->source = source;
    streams_[stream_path] = std::move(relay);

    source->start();
    printf("[RelayManager] added stream %s -> %s\n", stream_path.c_str(), upstream_url.c_str());
    return true;
}

bool RelayManager::removeStream(const std::string& stream_path) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_path);
    if (it == streams_.end()) {
        printf("[RelayManager] stream %s not found, cannot remove\n", stream_path.c_str());
        return false;
    }
    it->second->source->stop();
    streams_.erase(it);
    printf("[RelayManager] removed stream %s\n", stream_path.c_str());
    return true;
}

void RelayManager::addSink(const std::string& stream_path, const RtspSession::Ptr& sink) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_path);
    if (it == streams_.end()) {
        printf("[RelayManager] stream %s not found, cannot add sink\n", stream_path.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> sink_lock(it->second->sinks_mutex);
        it->second->sinks.push_back(sink);
    }

    printf("[RelayManager] sink added to stream %s, total sinks=%zu\n",
           stream_path.c_str(), it->second->sinks.size());
}

void RelayManager::removeSink(const std::string& stream_path, const RtspSession::Ptr& sink) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_path);
    if (it == streams_.end()) return;

    {
        std::lock_guard<std::mutex> sink_lock(it->second->sinks_mutex);
        auto& sinks = it->second->sinks;
        sinks.erase(
            std::remove(sinks.begin(), sinks.end(), sink),
            sinks.end()
        );
    }

    printf("[RelayManager] sink removed from stream %s, remaining sinks=%zu\n",
           stream_path.c_str(), it->second->sinks.size());
}

std::string RelayManager::getStreamSdp(const std::string& stream_path) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_path);
    if (it == streams_.end()) return "";

    if (it->second->source) {
        return it->second->source->sdp();
    }
    return "";
}

bool RelayManager::hasStream(const std::string& stream_path) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    return streams_.count(stream_path) > 0;
}

bool RelayManager::getStreamInfo(const std::string& stream_path, StreamInfo& info) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_path);
    if (it == streams_.end()) return false;

    auto& relay = it->second;
    info.stream_path = it->first;
    info.upstream_url = relay->upstream_url;
    info.source_state = relay->source ? sourceStateToString(relay->source->state()) : "None";
    info.sdp = relay->source ? relay->source->sdp() : "";
    info.has_sdp = !info.sdp.empty();

    {
        std::lock_guard<std::mutex> sink_lock(relay->sinks_mutex);
        info.sink_count = relay->sinks.size();
    }

    return true;
}

std::vector<StreamInfo> RelayManager::getAllStreamInfo() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    std::vector<StreamInfo> result;

    for (auto& [path, relay] : streams_) {
        StreamInfo info;
        info.stream_path = path;
        info.upstream_url = relay->upstream_url;
        info.source_state = relay->source ? sourceStateToString(relay->source->state()) : "None";
        info.sdp = relay->source ? relay->source->sdp() : "";
        info.has_sdp = !info.sdp.empty();

        {
            std::lock_guard<std::mutex> sink_lock(relay->sinks_mutex);
            info.sink_count = relay->sinks.size();
        }

        result.push_back(std::move(info));
    }

    return result;
}

void RelayManager::onRtpData(const std::string& stream_path, const uint8_t* data, size_t len) {
    StreamRelay* relay = nullptr;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_path);
        if (it == streams_.end()) return;
        relay = it->second.get();
    }

    std::lock_guard<std::mutex> sink_lock(relay->sinks_mutex);
    for (auto& sink : relay->sinks) {
        sink->onRtpData(data, len);
    }
}

void RelayManager::getStats(std::vector<std::string>& stats) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& [path, relay] : streams_) {
        char buf[256];
        size_t n_sinks = 0;
        {
            std::lock_guard<std::mutex> sink_lock(relay->sinks_mutex);
            n_sinks = relay->sinks.size();
        }
        snprintf(buf, sizeof(buf), "stream=%s sinks=%zu source_state=%s",
                 path.c_str(), n_sinks,
                 relay->source ? sourceStateToString(relay->source->state()) : "None");
        stats.push_back(buf);
    }
}

size_t RelayManager::totalSinks() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    size_t total = 0;
    for (auto& [path, relay] : streams_) {
        std::lock_guard<std::mutex> sink_lock(relay->sinks_mutex);
        total += relay->sinks.size();
    }
    return total;
}

size_t RelayManager::streamCount() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    return streams_.size();
}

} // namespace rtsp_relay
