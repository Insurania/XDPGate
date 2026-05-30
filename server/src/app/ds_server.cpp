#include "xdpg/server/app/ds_server.h"

#include "protocol/xdg_protocol.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace xdpg::server {
namespace {

std::uint64_t NowUsec() {
    // 这里使用 steady_clock，而不是 system_clock。
    // steady_clock 不会因为系统时间校准而倒退，适合做 ping/pong 的相对时间观测。
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

xdpg::EntityType ToProtocolEntityType(physics::EntityKind kind) {
    // physics 层不直接依赖 protocol 层，避免 ODE 逻辑被网络协议污染。
    // server 作为边界层负责把内部 entity kind 映射成 wire format 枚举。
    switch (kind) {
        case physics::EntityKind::PlayerCube:
            return xdpg::EntityType::PlayerCube;
        case physics::EntityKind::SmallCube:
            return xdpg::EntityType::SmallCube;
        case physics::EntityKind::StaticGround:
            return xdpg::EntityType::StaticGround;
    }
    return xdpg::EntityType::SmallCube;
}

float ToFloat(double value) {
    // ODE 本地构建目前使用 double precision，但 UDP snapshot v1 使用 float32。
    // 在 server 边界显式收窄，方便以后统一检查精度和带宽取舍。
    return static_cast<float>(value);
}

}  // namespace

DsServer::DsServer(DsServerConfig config)
    : config_(config),
      world_(physics::OdeWorldConfig{
          config.small_cube_count,
          1.0 / static_cast<double>(config.tick_rate),
      }),
      next_stats_log_(std::chrono::steady_clock::now() + std::chrono::seconds(1)) {
}

int DsServer::Run(const std::atomic_bool& stop_requested) {
    std::string error;
    if (!socket_.Open(config_.port, &error)) {
        std::cerr << "启动 UDP socket 失败: " << error << '\n';
        return 1;
    }

    std::cout << "XDPGate DS server started\n"
              << "  udp_port=" << config_.port << '\n'
              << "  tick_rate=" << config_.tick_rate << '\n'
              << "  snapshot_rate=" << config_.snapshot_rate << '\n'
              << "  small_cube_count=" << config_.small_cube_count << '\n';

    // accumulated fixed timestep loop：
    // 网络收包和墙钟时间是不稳定的，但 ODE simulation 必须按固定 dt 推进。
    // 因此每轮先累积真实经过时间，再按 fixed_dt 追赶一个或多个 simulation step。
    const auto fixed_dt =
        std::chrono::duration<double>(1.0 / static_cast<double>(config_.tick_rate));
    const auto snapshot_dt =
        std::chrono::duration<double>(1.0 / static_cast<double>(config_.snapshot_rate));
    auto previous_time = std::chrono::steady_clock::now();
    auto accumulated = std::chrono::duration<double>::zero();
    auto next_snapshot = previous_time + snapshot_dt;

    while (!stop_requested.load()) {
        const auto now = std::chrono::steady_clock::now();
        accumulated += now - previous_time;
        previous_time = now;

        DrainSocket();

        // 如果某一帧被系统调度拖慢，这里会连续 Step 多次追赶 tick。
        // 第一阶段没有加最大追赶步数；后续 benchmark 时可以记录 tick drift 并加保护。
        while (accumulated >= fixed_dt) {
            StepSimulation();
            accumulated -= fixed_dt;
        }

        if (now >= next_snapshot) {
            SendSnapshot();
            next_snapshot += snapshot_dt;
        }

        LogStatsIfDue();

        // 第一版 server 使用简单 sleep，避免空转吃满 CPU。
        // 后续做 benchmark 时会把收包策略拆成 recvfrom/recvmmsg 模式。
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "XDPGate DS server stopped\n";
    return 0;
}

void DsServer::DrainSocket() {
    std::array<std::uint8_t, xdpg::kMaxUdpPayloadSize> buffer{};
    for (;;) {
        Endpoint from;
        std::string error;
        const int received = socket_.Receive(buffer.data(), buffer.size(), &from, &error);
        if (received == 0) {
            // non-blocking socket 当前已经没有可读包。返回主循环去推进 ODE。
            return;
        }
        if (received < 0) {
            std::cerr << "UDP receive error: " << error << '\n';
            return;
        }

        ++counters_.received_packets;
        HandlePacket(buffer.data(), static_cast<std::size_t>(received), from);
    }
}

void DsServer::HandlePacket(const std::uint8_t* data, std::size_t size, const Endpoint& from) {
    // 先只解公共 header，可以快速过滤明显非法包，也避免把错误 packet 交给
    // packet-specific decoder 产生误导性的错误原因。
    const auto header = xdpg::DecodeHeaderOnly(data, size);
    if (header.error != xdpg::DecodeError::None) {
        ++counters_.invalid_packets;
        return;
    }

    switch (header.header.packet_type) {
        case xdpg::PacketType::Input:
            HandleInput(data, size, from);
            break;
        case xdpg::PacketType::Ping:
            HandlePing(data, size, from);
            break;
        default:
            // server 第一阶段只处理 INPUT 和 PING。其他合法包先忽略，避免协议扩展时
            // 老 server 把未知行为当成错误。
            break;
    }
}

void DsServer::HandleInput(const std::uint8_t* data, std::size_t size, const Endpoint& from) {
    const auto decoded = xdpg::DecodeInput(data, size);
    if (decoded.error != xdpg::DecodeError::None) {
        ++counters_.invalid_packets;
        return;
    }

    // 第一版采用“最近输入覆盖”策略：client 以 60Hz 发送 input，server 每个 tick
    // 消费最新值。这不是最终竞技游戏网络模型，但足够验证 DS 权威物理主流程。
    latest_input_.input_sequence = decoded.payload.input_sequence;
    latest_input_.move_x = decoded.payload.move_x;
    latest_input_.move_z = decoded.payload.move_z;
    latest_input_.buttons = decoded.payload.buttons;
    // 最近给 server 发送合法 input 的 endpoint 被认为是当前 snapshot 目标。
    // 这让本地 toy client 不需要额外注册流程。
    latest_client_ = from;
    has_client_ = true;
    ++counters_.valid_inputs;
}

void DsServer::HandlePing(const std::uint8_t* data, std::size_t size, const Endpoint& from) {
    const auto decoded = xdpg::DecodePing(data, size);
    if (decoded.error != xdpg::DecodeError::None) {
        ++counters_.invalid_packets;
        return;
    }

    // PONG 原样带回 client timestamp，并附加 server steady timestamp。
    // 后续工具可以用它估算 RTT 和 server 收包路径是否仍然活着。
    const auto pong = xdpg::EncodePong(outbound_sequence_++, xdpg::PongPayload{
        decoded.payload.timestamp_usec,
        NowUsec(),
    });

    std::string error;
    if (socket_.Send(pong.data(), pong.size(), from, &error)) {
        ++counters_.sent_pongs;
    } else {
        std::cerr << "send PONG failed: " << error << '\n';
    }
}

void DsServer::StepSimulation() {
    // input 在这里真正进入权威模拟。注意：packet decode 阶段不直接修改 ODE body。
    world_.ApplyInput(latest_input_);
    world_.Step();
}

void DsServer::SendSnapshot() {
    if (!has_client_) {
        return;
    }

    xdpg::SnapshotPayload snapshot;
    snapshot.server_tick = world_.tick();
    snapshot.last_processed_input_sequence = world_.last_processed_input_sequence();

    const auto states = world_.CollectEntityStates();

    // snapshot v1 目标是单 UDP 包稳定传输，因此只发送前 kMaxSnapshotEntities 个。
    // 当前 world 创建顺序保证 player 在第 0 个，后面是 small cubes。
    // 大量 entity 后续要通过 chunked snapshot 或区域裁剪解决。
    const std::size_t count = std::min<std::size_t>(states.size(), xdpg::kMaxSnapshotEntities);
    snapshot.entities.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& state = states[i];
        xdpg::EntitySnapshot entity;
        entity.entity_id = state.entity_id;
        entity.entity_type = ToProtocolEntityType(state.kind);
        // interacting flag 让 viewer/client 能区分“普通灰色 cube”和“正在被玩家影响的 cube”。
        entity.flags = state.is_interacting ? xdpg::kEntityFlagInteracting : 0;
        entity.position[0] = ToFloat(state.position.x);
        entity.position[1] = ToFloat(state.position.y);
        entity.position[2] = ToFloat(state.position.z);
        entity.rotation[0] = ToFloat(state.rotation.x);
        entity.rotation[1] = ToFloat(state.rotation.y);
        entity.rotation[2] = ToFloat(state.rotation.z);
        entity.rotation[3] = ToFloat(state.rotation.w);
        entity.linear_velocity[0] = ToFloat(state.linear_velocity.x);
        entity.linear_velocity[1] = ToFloat(state.linear_velocity.y);
        entity.linear_velocity[2] = ToFloat(state.linear_velocity.z);
        entity.angular_velocity[0] = ToFloat(state.angular_velocity.x);
        entity.angular_velocity[1] = ToFloat(state.angular_velocity.y);
        entity.angular_velocity[2] = ToFloat(state.angular_velocity.z);
        snapshot.entities.push_back(entity);
    }

    const auto packet = xdpg::EncodeSnapshot(outbound_sequence_++, snapshot);
    std::string error;
    if (socket_.Send(packet.data(), packet.size(), latest_client_, &error)) {
        ++counters_.sent_snapshots;
    } else {
        std::cerr << "send SNAPSHOT failed: " << error << '\n';
    }
}

void DsServer::LogStatsIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_stats_log_) {
        return;
    }
    next_stats_log_ = now + std::chrono::seconds(1);

    // 打印的是最近一秒增量，而不是进程累计值，更方便肉眼观察当前负载。
    const auto rx = counters_.received_packets - last_logged_counters_.received_packets;
    const auto inputs = counters_.valid_inputs - last_logged_counters_.valid_inputs;
    const auto invalid = counters_.invalid_packets - last_logged_counters_.invalid_packets;
    const auto snapshots = counters_.sent_snapshots - last_logged_counters_.sent_snapshots;
    const auto pongs = counters_.sent_pongs - last_logged_counters_.sent_pongs;
    last_logged_counters_ = counters_;

    std::cout << "[stats] tick=" << world_.tick()
              << " rx=" << rx
              << " inputs=" << inputs
              << " invalid=" << invalid
              << " snapshots=" << snapshots
              << " pongs=" << pongs;
    if (has_client_) {
        std::cout << " client=" << latest_client_.ToString();
    }
    std::cout << '\n';
}

}  // namespace xdpg::server
