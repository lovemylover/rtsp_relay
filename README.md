---
AIGC:
  ContentProducer: '001191110102MAD55U9H0F10002'
  ContentPropagator: '001191110102MAD55U9H0F10002'
  Label: '1'
  ProduceID: '5fbe0d90-b35e-48d6-a5fb-ef40cef9ff61'
  PropagateID: '5fbe0d90-b35e-48d6-a5fb-ef40cef9ff61'
  ReservedCode1: '76803cba-5529-4f20-a15b-7733d032f650'
  ReservedCode2: '76803cba-5529-4f20-a15b-7733d032f650'
---

# rtsp-relay

纯 C++17 实现的 RTSP 拉流转发服务器，零外部依赖。通过 HTTP API 动态管理流，支持单节点 1000 路转发、500 路接入。

## 项目结构

```
rtsp-relay/
├── CMakeLists.txt          # CMake 构建脚本
├── README.md               # 本文件
├── conf/
│   └── rtsp_relay.conf     # 配置文件模板
└── src/
    ├── main.cpp            # 入口：启动 RTSP/HTTP 服务、注册 API 路由
    ├── config.h/cpp        # INI 风格配置文件解析
    ├── event_loop.h/cpp    # 事件循环（epoll/kqueue 双平台）
    ├── buffer.h            # ByteBuffer / RingBuffer / ThreadSafeBuffer
    ├── tcp_connection.h/cpp # 非阻塞 TCP 连接（线程安全写缓冲）
    ├── tcp_server.h/cpp    # TCP 服务器（多 IO 线程 round-robin）
    ├── tcp_client.h/cpp    # 非阻塞 TCP 客户端（自动重连）
    ├── http_server.h/cpp   # 轻量 HTTP 服务器（路由 + JSON 响应）
    ├── rtsp_protocol.h/cpp # RTSP 消息解析构建 + RTP 包解析构建
    ├── rtsp_session.h/cpp  # 下游播放端会话（OPTIONS/DESCRIBE/SETUP/PLAY）
    ├── rtsp_source.h/cpp   # 上游拉流源（自动拉流、断线重连）
    └── relay_manager.h/cpp # 转发管理器（1 Source -> N Sink 扇出）
```

## 架构

```
  上游 RTSP 源                rtsp-relay                      下游播放端
  (rtsp://...)           ┌──────────────┐                  (VLC/FFplay)

  HTTP API ─────────────►│              │
  POST /api/streams      │  RelayManager│
  DELETE /api/streams    │              │
                         │  ┌─────────┐ │
  ┌──────┐  TCP拉流  ┌──►│  │Source 1│ │──┐
  │ 上游1 │──────────┘   │  │  ↓扇出 │ │  │
  └──────┘               │  │Sinks...│ │  ├──► Session1 (interleaved)
                         │  └─────────┘ │  ├──► Session2
  ┌──────┐  TCP拉流  ┌──►│  ┌─────────┐ │  ├──► Session3
  │ 上游2 │──────────┘   │  │Source 2│ │  │
  └──────┘               │  │  ↓扇出 │ │──┴──► SessionN
                         │  │Sinks...│ │
                         │  └─────────┘ │
                         └──────────────┘
```

- 流完全通过 HTTP API 动态管理，无需配置文件或重启
- 上游通过 TCP interleaved 拉流，信令与 RTP 共用同一连接
- RelayManager 按 stream_path 映射：1 个 RtspSource 扇出到 N 个 RtspSession
- 下游同样走 TCP interleaved，避免 NAT 穿透问题

## 编译

要求 C++17，无第三方依赖，支持 Linux / macOS。

```bash
cd rtsp-relay
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)              # Linux
make -j$(sysctl -n hw.ncpu)  # macOS
```

产物为单二进制 `rtsp-relay`。

## 运行

```bash
# 使用默认配置
./rtsp-relay

# 指定配置文件
./rtsp-relay /path/to/rtsp_relay.conf
```

Ctrl+C 停止服务。

## 配置

INI 格式，`#` 开头为注释。

```ini
[server]
rtsp_port=8554             # RTSP 监听端口
http_port=8080             # HTTP API 端口
io_threads=4               # IO 线程数，建议 CPU 核数
backlog=1024               # listen backlog
max_connections=2000       # 最大并发连接数
max_sources=500            # 最大上游拉流路数
stats_interval_sec=10      # 统计打印间隔（秒）
```

## HTTP API

所有接口返回 JSON，短连接模式。

### 添加流（动态拉流）

```bash
curl -X POST http://localhost:8080/api/streams \
  -H 'Content-Type: application/json' \
  -d '{"path":"/cam01","upstream":"rtsp://192.168.1.100:554/cam01"}'
```

响应：

```json
{
  "code": 0,
  "message": "stream added",
  "data": {
    "path": "/cam01",
    "upstream": "rtsp://192.168.1.100:554/cam01",
    "proxy_url": "rtsp://<server_ip>:8554/cam01"
  }
}
```

错误码：
- `409` 流已存在或达到 max_sources 上限
- `400` 缺少 path 或 upstream 字段

### 删除流

```bash
curl -X DELETE http://localhost:8080/api/streams \
  -H 'Content-Type: application/json' \
  -d '{"path":"/cam01"}'
```

响应：

```json
{"code": 0, "message": "stream removed"}
```

错误码：`404` 流不存在

### 查询所有流

```bash
curl http://localhost:8080/api/streams
```

响应：

```json
{
  "code": 0,
  "data": {
    "total": 1,
    "streams": [
      {
        "path": "/cam01",
        "upstream": "rtsp://192.168.1.100:554/cam01",
        "source_state": "Playing",
        "sink_count": 3,
        "has_sdp": true
      }
    ]
  }
}
```

source_state 取值：`Idle` / `Connecting` / `Options` / `Describe` / `Setup` / `Playing` / `Error`

### 查询单条流

```bash
curl http://localhost:8080/api/streams/cam01
```

### 服务整体统计

```bash
curl http://localhost:8080/api/stats
```

响应：

```json
{"code": 0, "data": {"stream_count": 5, "total_sinks": 23}}
```

### 典型工作流

```bash
# 1. 添加流
curl -X POST http://localhost:8080/api/streams \
  -d '{"path":"/live/cam01","upstream":"rtsp://10.0.1.100:554/live"}'

# 2. 播放代理流
ffplay rtsp://<server_ip>:8554/live/cam01

# 3. 查看状态
curl http://localhost:8080/api/streams/live/cam01

# 4. 停止转发
curl -X DELETE http://localhost:8080/api/streams \
  -d '{"path":"/live/cam01"}'
```

## 播放验证

```bash
ffplay rtsp://localhost:8554/cam01
ffprobe rtsp://localhost:8554/cam01
vlc rtsp://localhost:8554/cam01
```

## 并发设计

| 维度     | 方案                                        | 指标       |
|----------|---------------------------------------------|------------|
| IO 模型  | epoll(Linux) / kqueue(macOS) + 非阻塞 IO    | 1000+ 路   |
| 线程模型 | 1 accept 线程 + N IO 线程 (round-robin)      | CPU 充分   |
| 转发方式 | RTP over TCP interleaved                    | 非 NAT 敏感 |
| 扇出机制 | Source 收包 -> 遍历 Sink 直接推送            | 最小延迟   |
| 重连     | 断线 3s 自动重连，404 流 5s 重试              | 高可用     |
| 背压     | 写缓冲满时监听可写事件，不阻塞 IO 线程        | 防内存暴涨 |

## 关键模块说明

### event_loop
事件循环核心，epoll/kqueue 双平台适配。支持 fd 事件监听、跨线程任务投递（runInLoop）、定时器。wakeup 机制：Linux 用 eventfd，macOS 用 pipe。

### tcp_server
多 IO 线程 TCP 服务器。accept 线程接收新连接后 round-robin 分发到 IO 线程，避免单线程瓶颈。

### tcp_connection
非阻塞 TCP 连接，内置读缓冲和线程安全写缓冲。回调持有 shared_ptr 保证生命周期安全。写缓冲满时自动注册可写事件，写完即移除。

### http_server
轻量 HTTP 服务器，解析请求行/头部/Body，路由匹配（精确 + 前缀），自动设置 Content-Length 和 Connection: close。短连接模式，适合 API 调用。

### rtsp_session
下游播放端 RTSP 会话，状态机驱动：OPTIONS -> DESCRIBE -> SETUP -> PLAY -> TEARDOWN。支持 interleaved RTP 收发。

### rtsp_source
上游拉流源，通过 TcpClient 非阻塞连接上游 RTSP 服务器，完成 OPTIONS/DESCRIBE/SETUP/PLAY 信令交互后接收 interleaved RTP 数据。断线自动重连。

### relay_manager
转发管理器，维护 stream_path -> StreamRelay 映射。支持动态添加/删除流，RTP 数据从 Source 扇出到所有 Sink。提供结构化的 StreamInfo 接口供 HTTP API 查询。