#include "protocol/xdg_protocol.h"
#include "xdpg/net/udp_socket.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

struct ClientConfig {
    std::string server_host = "127.0.0.1";
    std::uint16_t server_port = 40000;
    std::uint64_t client_id = 1;
    std::uint32_t input_rate = 60;
    std::uint32_t ping_interval_ms = 1000;

    // duration_sec=0 表示一直运行，适合手动按键验证；非 0 适合自动化冒烟测试。
    std::uint32_t duration_sec = 0;

    // scripted 模式不读键盘，而是持续发送一个简单移动轨迹，方便无人值守测试服务器。
    bool scripted = false;
};

struct Counters {
    std::uint64_t sent_inputs = 0;
    std::uint64_t sent_pings = 0;
    std::uint64_t received_packets = 0;
    std::uint64_t snapshot_chunks = 0;
    std::uint64_t completed_snapshots = 0;
    std::uint64_t received_pongs = 0;
    std::uint64_t decode_errors = 0;
};

struct SnapshotAssembly {
    std::uint64_t tick = 0;
    std::uint16_t expected_chunks = 0;
    std::uint16_t total_entities = 0;
    std::uint32_t last_processed_input_sequence = 0;
    std::vector<bool> seen_chunks;

    void Reset(const xdpg::SnapshotPayload& payload) {
        tick = payload.server_tick;
        expected_chunks = payload.chunk_count;
        total_entities = payload.total_entity_count;
        last_processed_input_sequence = payload.last_processed_input_sequence;
        seen_chunks.assign(expected_chunks, false);
    }

    bool AddChunk(const xdpg::SnapshotPayload& payload) {
        // 分包重组这里只做观测，不保存所有 entity。真正的网络 viewer 后续会把 entity
        // 状态缓存在 tick buffer 中，再做插值/预测等渲染策略。
        if (payload.server_tick != tick || payload.chunk_count != expected_chunks) {
            Reset(payload);
        }
        if (payload.chunk_index >= seen_chunks.size()) {
            return false;
        }
        seen_chunks[payload.chunk_index] = true;

        for (bool seen : seen_chunks) {
            if (!seen) {
                return false;
            }
        }
        return true;
    }
};

std::uint64_t NowUsec() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool ParseUint16(const char* text, std::uint16_t* out) {
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

bool ParseUint64(const char* text, std::uint64_t* out) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = static_cast<std::uint64_t>(value);
    return true;
}

void PrintUsage(const char* exe) {
    std::cout << "Usage: " << exe << " [options]\n"
              << "Options:\n"
              << "  --server <ipv4>            default: 127.0.0.1\n"
              << "  --port <udp_port>          default: 40000\n"
              << "  --client-id <id>           default: 1\n"
              << "  --input-rate <hz>          default: 60\n"
              << "  --duration-sec <seconds>   default: 0, run forever\n"
              << "  --scripted                 send generated movement, do not read keyboard\n"
              << "  --help                     show this help\n";
}

bool ParseArgs(int argc, char** argv, ClientConfig* config) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        }
        if (arg == "--scripted") {
            config->scripted = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "缺少参数值: " << arg << '\n';
            return false;
        }

        if (arg == "--server") {
            config->server_host = argv[++i];
        } else if (arg == "--port") {
            if (!ParseUint16(argv[++i], &config->server_port)) {
                std::cerr << "无效端口\n";
                return false;
            }
        } else if (arg == "--client-id") {
            if (!ParseUint64(argv[++i], &config->client_id)) {
                std::cerr << "无效 client-id\n";
                return false;
            }
        } else if (arg == "--input-rate") {
            if (!ParseUint32(argv[++i], &config->input_rate) || config->input_rate == 0) {
                std::cerr << "无效 input-rate\n";
                return false;
            }
        } else if (arg == "--duration-sec") {
            if (!ParseUint32(argv[++i], &config->duration_sec)) {
                std::cerr << "无效 duration-sec\n";
                return false;
            }
        } else {
            std::cerr << "未知参数: " << arg << '\n';
            return false;
        }
    }
    return true;
}

bool IsKeyDown(int virtual_key) {
#if defined(_WIN32)
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
#else
    // Linux 版本的命令行客户端先不抢终端键盘输入。云服务器验证通常使用 scripted 模式；
    // 真正需要交互控制时，后续会做带渲染窗口的 network viewer。
    (void)virtual_key;
    return false;
#endif
}

xdpg::InputPayload BuildInputPayload(
    const ClientConfig& config,
    std::uint32_t input_sequence,
    std::chrono::steady_clock::time_point start_time,
    bool* previous_boost_down) {
    xdpg::InputPayload payload;
    payload.client_id = config.client_id;
    payload.input_sequence = input_sequence;
    payload.client_timestamp_usec = NowUsec();

    if (config.scripted) {
        // 自动化路径故意很简单：让 cube 大体向前移动，同时轻微左右摆动。
        // 这样本地/云服务器上不用键盘，也能看到 server 持续处理 input。
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        payload.move_x = static_cast<float>(std::sin(elapsed * 1.5) * 0.6);
        payload.move_z = 1.0f;
        payload.buttons = 0;
        return payload;
    }

    payload.move_x = (IsKeyDown('D') ? 1.0f : 0.0f) - (IsKeyDown('A') ? 1.0f : 0.0f);
    payload.move_z = (IsKeyDown('W') ? 1.0f : 0.0f) - (IsKeyDown('S') ? 1.0f : 0.0f);

#if defined(_WIN32)
    const bool boost_down = IsKeyDown(VK_SPACE);
#else
    const bool boost_down = false;
#endif
    // physics 层当前把 button bit0 当成一次性 boost，所以这里只在按下瞬间置位。
    payload.buttons = (boost_down && !*previous_boost_down) ? 1u : 0u;
    *previous_boost_down = boost_down;
    return payload;
}

void DrainSocket(
    xdpg::net::UdpSocket* socket,
    SnapshotAssembly* assembly,
    Counters* counters) {
    std::array<std::uint8_t, xdpg::kMaxUdpPayloadSize> buffer{};

    for (;;) {
        xdpg::net::Endpoint from;
        std::string error;
        const int received = socket->Receive(buffer.data(), buffer.size(), &from, &error);
        if (received == 0) {
            return;
        }
        if (received < 0) {
            std::cerr << "UDP receive error: " << error << '\n';
            return;
        }

        ++counters->received_packets;
        const auto header = xdpg::DecodeHeaderOnly(buffer.data(), static_cast<std::size_t>(received));
        if (header.error != xdpg::DecodeError::None) {
            ++counters->decode_errors;
            continue;
        }

        switch (header.header.packet_type) {
            case xdpg::PacketType::Snapshot: {
                const auto snapshot =
                    xdpg::DecodeSnapshot(buffer.data(), static_cast<std::size_t>(received));
                if (snapshot.error != xdpg::DecodeError::None) {
                    ++counters->decode_errors;
                    break;
                }
                ++counters->snapshot_chunks;
                if (assembly->AddChunk(snapshot.payload)) {
                    ++counters->completed_snapshots;
                }
                break;
            }
            case xdpg::PacketType::Pong: {
                const auto pong = xdpg::DecodePong(buffer.data(), static_cast<std::size_t>(received));
                if (pong.error == xdpg::DecodeError::None) {
                    ++counters->received_pongs;
                } else {
                    ++counters->decode_errors;
                }
                break;
            }
            default:
                break;
        }
    }
}

void LogStatsIfDue(
    const SnapshotAssembly& assembly,
    const Counters& counters,
    Counters* last_logged,
    std::chrono::steady_clock::time_point* next_log_time) {
    const auto now = std::chrono::steady_clock::now();
    if (now < *next_log_time) {
        return;
    }
    *next_log_time = now + std::chrono::seconds(1);

    const auto sent_inputs = counters.sent_inputs - last_logged->sent_inputs;
    const auto sent_pings = counters.sent_pings - last_logged->sent_pings;
    const auto rx = counters.received_packets - last_logged->received_packets;
    const auto chunks = counters.snapshot_chunks - last_logged->snapshot_chunks;
    const auto snapshots = counters.completed_snapshots - last_logged->completed_snapshots;
    const auto pongs = counters.received_pongs - last_logged->received_pongs;
    const auto errors = counters.decode_errors - last_logged->decode_errors;
    *last_logged = counters;

    std::cout << "[client] sent_inputs=" << sent_inputs
              << " pings=" << sent_pings
              << " rx=" << rx
              << " snapshot_chunks=" << chunks
              << " completed_snapshots=" << snapshots
              << " pongs=" << pongs
              << " decode_errors=" << errors
              << " last_tick=" << assembly.tick
              << " chunks=" << assembly.expected_chunks
              << " entities=" << assembly.total_entities
              << " last_input=" << assembly.last_processed_input_sequence << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    ClientConfig config;
    if (!ParseArgs(argc, argv, &config)) {
        PrintUsage(argv[0]);
        return 1;
    }

    xdpg::net::Endpoint server_endpoint;
    std::string error;
    if (!xdpg::net::Endpoint::FromIpv4(config.server_host, config.server_port, &server_endpoint,
                                       &error)) {
        std::cerr << "解析服务器地址失败: " << error << '\n';
        return 1;
    }

    xdpg::net::UdpSocket socket;
    if (!socket.Open(0, &error)) {
        std::cerr << "启动 UDP socket 失败: " << error << '\n';
        return 1;
    }

    std::cout << "XDPGate toy client started\n"
              << "  server=" << config.server_host << ':' << config.server_port << '\n'
              << "  client_id=" << config.client_id << '\n'
              << "  input_rate=" << config.input_rate << '\n'
              << "  mode=" << (config.scripted ? "scripted" : "keyboard") << '\n';

    const auto start_time = std::chrono::steady_clock::now();
    const auto input_dt =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(config.input_rate)));
    const auto ping_dt = std::chrono::milliseconds(config.ping_interval_ms);
    auto next_input = start_time;
    auto next_ping = start_time;
    auto next_log_time = start_time + std::chrono::seconds(1);

    std::uint32_t packet_sequence = 1;
    std::uint32_t input_sequence = 1;
    bool previous_boost_down = false;
    SnapshotAssembly assembly;
    Counters counters;
    Counters last_logged;

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (config.duration_sec > 0 &&
            now - start_time >= std::chrono::seconds(config.duration_sec)) {
            break;
        }

        if (now >= next_input) {
            const auto payload =
                BuildInputPayload(config, input_sequence++, start_time, &previous_boost_down);
            const auto packet = xdpg::EncodeInput(packet_sequence++, payload);
            if (socket.Send(packet.data(), packet.size(), server_endpoint, &error)) {
                ++counters.sent_inputs;
            } else {
                std::cerr << "send INPUT failed: " << error << '\n';
            }
            next_input += input_dt;
        }

        if (now >= next_ping) {
            const auto packet = xdpg::EncodePing(packet_sequence++, xdpg::PingPayload{NowUsec()});
            if (socket.Send(packet.data(), packet.size(), server_endpoint, &error)) {
                ++counters.sent_pings;
            } else {
                std::cerr << "send PING failed: " << error << '\n';
            }
            next_ping += ping_dt;
        }

        DrainSocket(&socket, &assembly, &counters);
        LogStatsIfDue(assembly, counters, &last_logged, &next_log_time);

        // 与 server 一样，第一版先用短 sleep 控制空转成本；后续压测工具会改成更精确的节拍器。
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    DrainSocket(&socket, &assembly, &counters);
    LogStatsIfDue(assembly, counters, &last_logged, &next_log_time);
    std::cout << "XDPGate toy client stopped\n";
    return 0;
}
