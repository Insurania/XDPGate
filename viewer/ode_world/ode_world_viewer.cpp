#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "protocol/xdg_protocol.h"
#include "xdpg/net/udp_socket.h"

#include <drawstuff/drawstuff.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

struct ViewerConfig {
    std::string server_host = "127.0.0.1";
    std::uint16_t server_port = 40000;
    std::uint64_t client_id = 1;
    std::uint32_t input_rate = 60;
};

struct RenderEntity {
    std::uint32_t entity_id = 0;
    xdpg::EntityType entity_type = xdpg::EntityType::SmallCube;
    std::uint16_t flags = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct SnapshotAssembly {
    std::uint64_t tick = 0;
    xdpg::SnapshotEncodingMode encoding_mode = xdpg::SnapshotEncodingMode::Full;
    std::uint16_t expected_chunks = 0;
    std::uint16_t total_entities = 0;
    std::vector<bool> seen_chunks;
    std::vector<std::vector<xdpg::EntitySnapshot>> chunks;

    void Reset(const xdpg::SnapshotPayload& payload) {
        tick = payload.server_tick;
        encoding_mode = payload.encoding_mode;
        expected_chunks = payload.chunk_count;
        total_entities = payload.total_entity_count;
        seen_chunks.assign(expected_chunks, false);
        chunks.clear();
        chunks.resize(expected_chunks);
    }

    bool AddChunk(
        const xdpg::SnapshotPayload& payload,
        std::vector<xdpg::EntitySnapshot>* out_entities) {
        // 第一版只重组“当前最新 tick”。如果 UDP 乱序带来旧 tick 的尾包，直接忽略；
        // 后续做 interpolation buffer 时，会改成按 tick 保存多个 assembly。
        if (expected_chunks == 0 || payload.server_tick > tick) {
            Reset(payload);
        } else if (payload.server_tick < tick ||
                   payload.chunk_count != expected_chunks ||
                   payload.encoding_mode != encoding_mode) {
            return false;
        }

        if (payload.chunk_index >= chunks.size()) {
            return false;
        }

        chunks[payload.chunk_index] = payload.entities;
        seen_chunks[payload.chunk_index] = true;

        for (bool seen : seen_chunks) {
            if (!seen) {
                return false;
            }
        }

        out_entities->clear();
        out_entities->reserve(total_entities);
        for (const auto& chunk : chunks) {
            for (const auto& snapshot : chunk) {
                out_entities->push_back(snapshot);
            }
        }
        return true;
    }
};

ViewerConfig g_config;
xdpg::net::UdpSocket g_socket;
xdpg::net::Endpoint g_server_endpoint;
SnapshotAssembly g_snapshot_assembly;
std::vector<RenderEntity> g_entities;
bool g_has_full_snapshot = false;
std::uint32_t g_packet_sequence = 1;
std::uint32_t g_input_sequence = 1;
bool g_previous_boost_down = false;
std::chrono::steady_clock::time_point g_next_input_time;
std::chrono::steady_clock::time_point g_next_ping_time;
std::chrono::steady_clock::time_point g_next_stats_time;
std::chrono::steady_clock::time_point g_last_stats_time;
std::uint64_t g_received_snapshot_chunks = 0;
std::uint64_t g_completed_snapshots = 0;
std::uint64_t g_decode_errors = 0;
std::uint64_t g_rx_bytes = 0;
std::uint64_t g_tx_bytes = 0;
std::uint64_t g_last_rx_bytes = 0;
std::uint64_t g_last_tx_bytes = 0;
std::uint64_t g_last_snapshot_chunks = 0;
std::uint64_t g_last_completed_snapshots = 0;

#if defined(_WIN32)
BOOL CALLBACK FindCurrentProcessDrawstuffWindow(HWND hwnd, LPARAM lparam) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id != GetCurrentProcessId()) {
        return TRUE;
    }

    char class_name[64] = {};
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    if (std::string(class_name) == "SimAppClass") {
        *reinterpret_cast<HWND*>(lparam) = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindDrawstuffWindow() {
    HWND hwnd = nullptr;
    EnumWindows(&FindCurrentProcessDrawstuffWindow, reinterpret_cast<LPARAM>(&hwnd));
    return hwnd;
}
#endif

std::string FormatMbps(double bytes_per_second) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << (bytes_per_second * 8.0 / 1000000.0);
    return out.str();
}

void UpdateRuntimeBandwidthDisplay(
    double rx_bytes_per_second,
    double tx_bytes_per_second,
    std::uint64_t completed_snapshots_per_second,
    std::uint64_t snapshot_chunks_per_second,
    xdpg::SnapshotEncodingMode mode) {
    const std::string rx_mbps = FormatMbps(rx_bytes_per_second);
    const std::string tx_mbps = FormatMbps(tx_bytes_per_second);

#if defined(_WIN32)
    // drawstuff 没有 2D 文本 HUD API。Windows 下先把实时带宽写到图形窗口标题，
    // 再同步到控制台标题；这样看画面或看终端都能直接看到当前 RX/TX 速率。
    std::ostringstream title;
    title << "XDPGate RX " << rx_mbps << " Mbps TX " << tx_mbps
          << " Mbps mode " << xdpg::SnapshotEncodingModeName(mode)
          << " snapshots " << completed_snapshots_per_second
          << "/s chunks " << snapshot_chunks_per_second << "/s";
    const std::string title_text = title.str();
    if (HWND window = FindDrawstuffWindow()) {
        SetWindowTextA(window, title_text.c_str());
    }
    SetConsoleTitleA(title_text.c_str());
#endif
}

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
    std::cout << "Usage: " << exe << " [viewer-options] [drawstuff-options]\n"
              << "Viewer options:\n"
              << "  --server <ipv4>            default: 127.0.0.1\n"
              << "  --port <udp_port>          default: 40000\n"
              << "  --client-id <id>           default: 1\n"
              << "  --input-rate <hz>          default: 60\n"
              << "  --help                     show this help\n"
              << "Drawstuff example:\n"
              << "  -notex                     disable texture loading\n";
}

bool ParseViewerArgs(int argc, char** argv, ViewerConfig* config,
                     std::vector<std::string>* draw_args) {
    draw_args->clear();
    draw_args->push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        }

        auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "缺少参数值: " << name << '\n';
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--server") {
            const char* value = require_value("--server");
            if (value == nullptr) {
                return false;
            }
            config->server_host = value;
        } else if (arg == "--port") {
            const char* value = require_value("--port");
            if (value == nullptr || !ParseUint16(value, &config->server_port)) {
                std::cerr << "无效端口\n";
                return false;
            }
        } else if (arg == "--client-id") {
            const char* value = require_value("--client-id");
            if (value == nullptr || !ParseUint64(value, &config->client_id)) {
                std::cerr << "无效 client-id\n";
                return false;
            }
        } else if (arg == "--input-rate") {
            const char* value = require_value("--input-rate");
            if (value == nullptr || !ParseUint32(value, &config->input_rate) ||
                config->input_rate == 0) {
                std::cerr << "无效 input-rate\n";
                return false;
            }
        } else {
            // 未识别参数保留给 drawstuff，例如 -notex。这样 viewer 自己的参数不会污染
            // drawstuff，而 drawstuff 原有调试开关仍然可用。
            draw_args->push_back(arg);
        }
    }
    return true;
}

bool IsKeyDown(int key) {
#if defined(_WIN32)
    return (GetAsyncKeyState(key) & 0x8000) != 0;
#else
    (void)key;
    return false;
#endif
}

xdpg::InputPayload BuildInputPayload() {
    xdpg::InputPayload input;
    input.client_id = g_config.client_id;
    input.input_sequence = g_input_sequence++;
    input.client_timestamp_usec = NowUsec();

    // viewer 不再本地推进 ODE。这里的按键只会变成网络 INPUT，最终画面必须等
    // server 处理输入、推进物理世界、再把 snapshot 发回来。
    input.move_x = (IsKeyDown('D') ? 1.0f : 0.0f) - (IsKeyDown('A') ? 1.0f : 0.0f);
    input.move_z = (IsKeyDown('W') ? 1.0f : 0.0f) - (IsKeyDown('S') ? 1.0f : 0.0f);

#if defined(_WIN32)
    const bool boost_down = IsKeyDown(VK_SPACE);
#else
    const bool boost_down = false;
#endif
    input.buttons = (boost_down && !g_previous_boost_down) ? 1u : 0u;
    g_previous_boost_down = boost_down;
    return input;
}

void SendInputIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_input_time) {
        return;
    }

    const auto input_dt =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(g_config.input_rate)));
    const auto payload = BuildInputPayload();
    const auto packet = xdpg::EncodeInput(g_packet_sequence++, payload);
    std::string error;
    if (!g_socket.Send(packet.data(), packet.size(), g_server_endpoint, &error)) {
        std::cerr << "send INPUT failed: " << error << '\n';
    } else {
        g_tx_bytes += packet.size();
    }
    g_next_input_time += input_dt;
}

void SendPingIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_ping_time) {
        return;
    }

    const auto packet = xdpg::EncodePing(g_packet_sequence++, xdpg::PingPayload{NowUsec()});
    std::string error;
    if (!g_socket.Send(packet.data(), packet.size(), g_server_endpoint, &error)) {
        std::cerr << "send PING failed: " << error << '\n';
    } else {
        g_tx_bytes += packet.size();
    }
    g_next_ping_time += std::chrono::seconds(1);
}

void DrainSocket() {
    std::array<std::uint8_t, xdpg::kMaxUdpPayloadSize> buffer{};
    for (;;) {
        xdpg::net::Endpoint from;
        std::string error;
        const int received = g_socket.Receive(buffer.data(), buffer.size(), &from, &error);
        if (received == 0) {
            return;
        }
        if (received < 0) {
            std::cerr << "UDP receive error: " << error << '\n';
            return;
        }
        g_rx_bytes += static_cast<std::uint64_t>(received);

        const auto header = xdpg::DecodeHeaderOnly(buffer.data(), static_cast<std::size_t>(received));
        if (header.error != xdpg::DecodeError::None) {
            ++g_decode_errors;
            continue;
        }

        if (header.header.packet_type == xdpg::PacketType::Snapshot) {
            const auto snapshot =
                xdpg::DecodeSnapshot(buffer.data(), static_cast<std::size_t>(received));
            if (snapshot.error != xdpg::DecodeError::None) {
                ++g_decode_errors;
                continue;
            }
            ++g_received_snapshot_chunks;
            std::vector<xdpg::EntitySnapshot> assembled_entities;
            if (g_snapshot_assembly.AddChunk(snapshot.payload, &assembled_entities)) {
                if (snapshot.payload.encoding_mode == xdpg::SnapshotEncodingMode::Full) {
                    g_entities.clear();
                    g_entities.reserve(assembled_entities.size());
                    for (const auto& snapshot_entity : assembled_entities) {
                        RenderEntity entity;
                        entity.entity_id = snapshot_entity.entity_id;
                        entity.entity_type = snapshot_entity.entity_type;
                        entity.flags = snapshot_entity.flags;
                        std::copy(std::begin(snapshot_entity.position),
                                  std::end(snapshot_entity.position),
                                  std::begin(entity.position));
                        std::copy(std::begin(snapshot_entity.rotation),
                                  std::end(snapshot_entity.rotation),
                                  std::begin(entity.rotation));
                        g_entities.push_back(entity);
                    }
                    g_has_full_snapshot = true;
                } else if (g_has_full_snapshot) {
                    for (const auto& snapshot_entity : assembled_entities) {
                        auto it = std::find_if(g_entities.begin(), g_entities.end(),
                                               [&snapshot_entity](const RenderEntity& entity) {
                                                   return entity.entity_id ==
                                                          snapshot_entity.entity_id;
                                               });
                        if (it == g_entities.end()) {
                            continue;
                        }
                        it->entity_type = snapshot_entity.entity_type;
                        it->flags = snapshot_entity.flags;
                        std::copy(std::begin(snapshot_entity.position),
                                  std::end(snapshot_entity.position),
                                  std::begin(it->position));
                        std::copy(std::begin(snapshot_entity.rotation),
                                  std::end(snapshot_entity.rotation),
                                  std::begin(it->rotation));
                    }
                }
                ++g_completed_snapshots;
            }
        } else if (header.header.packet_type == xdpg::PacketType::Pong) {
            const auto pong = xdpg::DecodePong(buffer.data(), static_cast<std::size_t>(received));
            if (pong.error != xdpg::DecodeError::None) {
                ++g_decode_errors;
            }
        }
    }
}

void OdeToDrawPosition(const float in[3], float out[3]) {
    // 项目物理坐标为 Y-up；drawstuff 渲染坐标按 Z-up。这里只做坐标轴交换，
    // 不改变 server snapshot 的权威数值。
    out[0] = in[0];
    out[1] = in[2];
    out[2] = in[1];
}

std::array<float, 9> QuaternionToMatrix(const float q[4]) {
    const float x = q[0];
    const float y = q[1];
    const float z = q[2];
    const float w = q[3];
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    return {
        1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz),        2.0f * (xz + wy),
        2.0f * (xy + wz),        1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx),
        2.0f * (xz - wy),        2.0f * (yz + wx),        1.0f - 2.0f * (xx + yy),
    };
}

void OdeToDrawRotation(const float rotation[4], float out[12]) {
    const auto r = QuaternionToMatrix(rotation);

    const int p[3] = {0, 2, 1};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[row * 4 + col] = r[p[row] * 3 + p[col]];
        }
        out[row * 4 + 3] = 0.0f;
    }
}

float CubeSizeFor(const RenderEntity& entity) {
    if (entity.entity_type == xdpg::EntityType::PlayerCube) {
        return 1.0f;
    }
    return 0.32f;
}

void DrawGround() {
    float pos[3]{0.0f, 0.0f, -0.02f};
    float rot[12]{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
    };
    float sides[3]{160.0f, 160.0f, 0.04f};
    dsSetColor(1.0f, 1.0f, 1.0f);
    dsDrawBox(pos, rot, sides);
}

void DrawEntity(const RenderEntity& entity) {
    float pos[3];
    float rot[12];
    const float cube_size = CubeSizeFor(entity);
    float sides[3]{cube_size, cube_size, cube_size};

    OdeToDrawPosition(entity.position, pos);
    OdeToDrawRotation(entity.rotation, rot);

    if (entity.entity_type == xdpg::EntityType::PlayerCube ||
        (entity.flags & xdpg::kEntityFlagInteracting) != 0u) {
        dsSetColor(1.0f, 0.05f, 0.04f);
    } else {
        dsSetColor(0.55f, 0.55f, 0.55f);
    }

    dsDrawBox(pos, rot, sides);
}

void UpdateFollowCamera(const RenderEntity& player) {
    float player_pos[3];
    OdeToDrawPosition(player.position, player_pos);

    constexpr float kFollowDistance = 8.0f;
    constexpr float kFollowHeight = 6.5f;
    constexpr float kViewYawDegrees = 130.0f;
    constexpr float kPitchDegrees = -38.0f;
    constexpr float kRadiansPerDegree = 3.1415926535f / 180.0f;

    const float yaw_radians = kViewYawDegrees * kRadiansPerDegree;
    float xyz[3]{
        player_pos[0] - std::cos(yaw_radians) * kFollowDistance,
        player_pos[1] - std::sin(yaw_radians) * kFollowDistance,
        player_pos[2] + kFollowHeight,
    };
    float hpr[3]{
        kViewYawDegrees,
        kPitchDegrees,
        0.0f,
    };

    dsSetViewpoint(xyz, hpr);
}

void LogStatsIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_stats_time) {
        return;
    }
    g_next_stats_time = now + std::chrono::seconds(1);
    const double elapsed_seconds =
        std::chrono::duration<double>(now - g_last_stats_time).count();
    g_last_stats_time = now;

    const std::uint64_t rx_delta = g_rx_bytes - g_last_rx_bytes;
    const std::uint64_t tx_delta = g_tx_bytes - g_last_tx_bytes;
    const std::uint64_t chunk_delta = g_received_snapshot_chunks - g_last_snapshot_chunks;
    const std::uint64_t completed_delta = g_completed_snapshots - g_last_completed_snapshots;
    g_last_rx_bytes = g_rx_bytes;
    g_last_tx_bytes = g_tx_bytes;
    g_last_snapshot_chunks = g_received_snapshot_chunks;
    g_last_completed_snapshots = g_completed_snapshots;

    const double safe_elapsed = elapsed_seconds > 0.001 ? elapsed_seconds : 1.0;
    const double rx_bytes_per_second = static_cast<double>(rx_delta) / safe_elapsed;
    const double tx_bytes_per_second = static_cast<double>(tx_delta) / safe_elapsed;
    const auto chunks_per_second =
        static_cast<std::uint64_t>(static_cast<double>(chunk_delta) / safe_elapsed + 0.5);
    const auto completed_per_second =
        static_cast<std::uint64_t>(static_cast<double>(completed_delta) / safe_elapsed + 0.5);

    UpdateRuntimeBandwidthDisplay(rx_bytes_per_second, tx_bytes_per_second, completed_per_second,
                                  chunks_per_second, g_snapshot_assembly.encoding_mode);

    std::cout << "[viewer] tick=" << g_snapshot_assembly.tick
              << " entities=" << g_entities.size()
              << " mode=" << xdpg::SnapshotEncodingModeName(g_snapshot_assembly.encoding_mode)
              << " rx=" << FormatMbps(rx_bytes_per_second) << "Mbps"
              << " tx=" << FormatMbps(tx_bytes_per_second) << "Mbps"
              << " chunks/s=" << chunks_per_second
              << " snapshots/s=" << completed_per_second
              << " decode_errors=" << g_decode_errors << '\n';
}

void Start() {
    static float xyz[3] = {6.0f, -6.0f, 6.5f};
    static float hpr[3] = {130.0f, -38.0f, 0.0f};
    dsSetViewpoint(xyz, hpr);
    std::cout << "XDPGate network snapshot viewer\n"
              << "Server: " << g_config.server_host << ':' << g_config.server_port << '\n'
              << "Hold W/A/S/D to send input, tap Space to boost, press Q or Esc to quit.\n";
}

void Step(int pause) {
    (void)pause;

    // pause 只影响 drawstuff 自己的 simulation 概念；network viewer 没有本地 ODE，
    // 所以仍然收包和渲染最后一帧 snapshot。
    SendInputIfDue();
    SendPingIfDue();
    DrainSocket();

    for (const auto& entity : g_entities) {
        if (entity.entity_type == xdpg::EntityType::PlayerCube) {
            UpdateFollowCamera(entity);
            break;
        }
    }

    DrawGround();
    for (const auto& entity : g_entities) {
        DrawEntity(entity);
    }
    LogStatsIfDue();
}

void Command(int cmd) {
    const char key = static_cast<char>(std::tolower(cmd));
    if (key == 'q' || cmd == 27) {
        std::exit(0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> draw_args_storage;
        if (!ParseViewerArgs(argc, argv, &g_config, &draw_args_storage)) {
            PrintUsage(argv[0]);
            return 1;
        }

        std::string error;
        if (!xdpg::net::Endpoint::FromIpv4(g_config.server_host, g_config.server_port,
                                           &g_server_endpoint, &error)) {
            std::cerr << "解析服务器地址失败: " << error << '\n';
            return 1;
        }
        if (!g_socket.Open(0, &error)) {
            std::cerr << "启动 UDP socket 失败: " << error << '\n';
            return 1;
        }

        const auto now = std::chrono::steady_clock::now();
        g_next_input_time = now;
        g_next_ping_time = now;
        g_next_stats_time = now + std::chrono::seconds(1);
        g_last_stats_time = now;

        std::vector<char*> draw_argv;
        draw_argv.reserve(draw_args_storage.size());
        for (std::string& arg : draw_args_storage) {
            draw_argv.push_back(arg.data());
        }

        dsFunctions functions{};
        functions.version = DS_VERSION;
        functions.start = &Start;
        functions.step = &Step;
        functions.command = &Command;
        functions.stop = nullptr;
        functions.path_to_textures = DRAWSTUFF_TEXTURE_PATH;

        dsSimulationLoop(static_cast<int>(draw_argv.size()), draw_argv.data(), 1280, 720,
                         &functions);
    } catch (const std::exception& e) {
        std::cerr << "启动 network snapshot viewer 失败: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
