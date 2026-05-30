#include "xdpg/server/app/ds_server.h"

#include "protocol/xdg_protocol.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::atomic_bool g_stop_requested{false};

void HandleSignal(int) {
    // 信号处理函数里只设置 atomic flag，不做复杂清理。
    // 真正的 socket/ODE 析构交给主线程从 Run 返回后自然完成。
    g_stop_requested.store(true);
}

bool ParseUint16(const char* text, std::uint16_t* out) {
    // CLI 参数解析保持最小依赖，避免第一阶段引入额外第三方库。
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 65535ul) {
        return false;
    }
    *out = static_cast<std::uint16_t>(value);
    return true;
}

bool ParseUint32(const char* text, std::uint32_t* out) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

void PrintUsage(const char* exe) {
    std::cout << "Usage: " << exe << " [options]\n"
              << "Options:\n"
              << "  --port <udp_port>          default: 40000\n"
              << "  --tick-rate <hz>           default: 60\n"
              << "  --snapshot-rate <hz>       default: 20\n"
              << "  --small-cubes <count>      default: 15, max snapshot v1 entities = 16\n"
              << "  --help                     show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    xdpg::server::DsServerConfig config;

    // 手写简单参数解析，便于 Windows/Ubuntu 都能直接运行。
    // 后续参数变多时，可以再换成更系统的 CLI parser。
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            std::cerr << "缺少参数值: " << arg << '\n';
            return 1;
        }

        if (arg == "--port") {
            if (!ParseUint16(argv[++i], &config.port)) {
                std::cerr << "无效端口\n";
                return 1;
            }
        } else if (arg == "--tick-rate") {
            if (!ParseUint32(argv[++i], &config.tick_rate) || config.tick_rate == 0) {
                std::cerr << "无效 tick rate\n";
                return 1;
            }
        } else if (arg == "--snapshot-rate") {
            if (!ParseUint32(argv[++i], &config.snapshot_rate) || config.snapshot_rate == 0) {
                std::cerr << "无效 snapshot rate\n";
                return 1;
            }
        } else if (arg == "--small-cubes") {
            if (!ParseUint32(argv[++i], &config.small_cube_count)) {
                std::cerr << "无效 small cube 数量\n";
                return 1;
            }
        } else {
            std::cerr << "未知参数: " << arg << '\n';
            return 1;
        }
    }

    // 这个限制来自协议 v1 的单包 snapshot 设计，不是 ODE world 本身的限制。
    // 本地 viewer 可以跑 180 个 cube；网络 server 第一版先避免 UDP 分片。
    if (config.small_cube_count + 1 > xdpg::kMaxSnapshotEntities) {
        std::cerr << "当前 snapshot v1 最多发送 " << xdpg::kMaxSnapshotEntities
                  << " 个 entity。请将 --small-cubes 设置为 "
                  << (xdpg::kMaxSnapshotEntities - 1) << " 或更小。\n";
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    xdpg::server::DsServer server(config);
    return server.Run(g_stop_requested);
}
