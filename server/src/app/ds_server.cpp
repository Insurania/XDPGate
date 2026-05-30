#include "xdpg/server/app/ds_server.h"

#include "protocol/xdg_protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    // ODE 本地构建目前使用 double precision；协议层会再做位置/旋转压缩。
    // 在 server 边界先显式收窄成 float，方便把“物理精度”和“网络量化”分层观察。
    return static_cast<float>(value);
}

std::uint32_t DenseIndexFromEntityId(std::uint32_t entity_id) {
    return xdpg::SnapshotDenseIndexFromEntityId(entity_id);
}

double DistanceSquared(const xdpg::EntitySnapshot& a, const xdpg::EntitySnapshot& b) {
    const double dx = static_cast<double>(a.position[0] - b.position[0]);
    const double dy = static_cast<double>(a.position[1] - b.position[1]);
    const double dz = static_cast<double>(a.position[2] - b.position[2]);
    return dx * dx + dy * dy + dz * dz;
}

physics::Vec3 SpawnPositionForPlayer(std::uint32_t player_entity_id) {
    // 多客户端第一版先用固定出生环，避免新 player 和已有刚体完全重叠。
    // entity_id 从 1 开始，映射到一圈半径约 2.5m 的点位。
    constexpr double kSpawnRadius = 2.5;
    constexpr double kSpawnHeight = 1.5;
    constexpr double kPi = 3.14159265358979323846;
    const std::uint32_t slot = player_entity_id - xdpg::kFirstPlayerEntityId;
    const double angle = static_cast<double>(slot) * (2.0 * kPi / 8.0);
    const double ring = 1.0 + static_cast<double>(slot / 8u) * 0.7;
    return physics::Vec3{
        std::cos(angle) * kSpawnRadius * ring,
        kSpawnHeight,
        std::sin(angle) * kSpawnRadius * ring,
    };
}

bool SnapshotEntityChanged(const xdpg::EntitySnapshot& previous,
                           const xdpg::EntitySnapshot& current) {
    if (previous.entity_id != current.entity_id ||
        previous.entity_type != current.entity_type ||
        previous.flags != current.flags) {
        return true;
    }

    // 与协议量化精度对齐：水平轴约 7.8mm/bin，高度轴约 0.49mm/bin。
    // 使用略小于一个 bin 的阈值，可以过滤静止物体的浮点微抖动，同时不吞掉可见移动。
    constexpr float kHorizontalPositionEpsilon = 0.006f;
    constexpr float kVerticalPositionEpsilon = 0.0004f;
    if (std::fabs(previous.position[0] - current.position[0]) > kHorizontalPositionEpsilon ||
        std::fabs(previous.position[1] - current.position[1]) > kVerticalPositionEpsilon ||
        std::fabs(previous.position[2] - current.position[2]) > kHorizontalPositionEpsilon) {
        return true;
    }

    constexpr float kRotationEpsilon = 0.00005f;
    for (std::size_t i = 0; i < 4; ++i) {
        if (std::fabs(previous.rotation[i] - current.rotation[i]) > kRotationEpsilon) {
            return true;
        }
    }
    return false;
}

bool SameEntitySet(const std::vector<xdpg::EntitySnapshot>& previous,
                   const std::vector<xdpg::EntitySnapshot>& current) {
    if (previous.size() != current.size()) {
        return false;
    }
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (previous[i].entity_id != current[i].entity_id ||
            previous[i].entity_type != current[i].entity_type) {
            return false;
        }
    }
    return true;
}

const xdpg::EntitySnapshot* FindSnapshotEntity(
    const std::vector<xdpg::EntitySnapshot>& entities,
    std::uint32_t entity_id) {
    const auto it = std::find_if(entities.begin(), entities.end(),
                                 [entity_id](const xdpg::EntitySnapshot& entity) {
                                     return entity.entity_id == entity_id;
                                 });
    return it == entities.end() ? nullptr : &*it;
}

bool CanUseOffsetEncoding(const std::vector<xdpg::EntitySnapshot>& entities,
                          xdpg::SnapshotEncodingMode mode) {
    const std::uint32_t max_offset =
        mode == xdpg::SnapshotEncodingMode::DeltaOffsetU8 ? UINT8_MAX : UINT16_MAX;
    const std::size_t max_per_packet = xdpg::MaxSnapshotEntitiesPerPacket(mode);

    for (std::size_t begin = 0; begin < entities.size(); begin += max_per_packet) {
        const std::size_t end =
            begin + max_per_packet < entities.size() ? begin + max_per_packet : entities.size();
        std::int64_t previous_dense_index = -1;
        for (std::size_t i = begin; i < end; ++i) {
            const std::uint32_t dense_index = DenseIndexFromEntityId(entities[i].entity_id);
            if (static_cast<std::int64_t>(dense_index) <= previous_dense_index) {
                return false;
            }
            const auto offset =
                static_cast<std::uint32_t>(static_cast<std::int64_t>(dense_index) -
                                           previous_dense_index - 1);
            if (offset > max_offset) {
                return false;
            }
            previous_dense_index = dense_index;
        }
    }
    return true;
}

std::size_t ChunkedSnapshotCost(xdpg::SnapshotEncodingMode mode, std::size_t entity_count) {
    const std::size_t max_per_packet = xdpg::MaxSnapshotEntitiesPerPacket(mode);
    const std::size_t chunk_count =
        entity_count == 0 ? 1 : (entity_count + max_per_packet - 1) / max_per_packet;
    return chunk_count * (xdpg::kHeaderSize + xdpg::kSnapshotPayloadHeaderSize) +
           entity_count * xdpg::SnapshotEntityWireSize(mode);
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
              << "  small_cube_count=" << config_.small_cube_count << '\n'
              << "  interest_radius_meters=" << config_.interest_radius_meters << '\n'
              << "  snapshot_entity_budget=" << config_.snapshot_entity_budget << '\n';

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
        net::Endpoint from;
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

void DsServer::HandlePacket(const std::uint8_t* data, std::size_t size, const net::Endpoint& from) {
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

void DsServer::HandleInput(const std::uint8_t* data, std::size_t size, const net::Endpoint& from) {
    const auto decoded = xdpg::DecodeInput(data, size);
    if (decoded.error != xdpg::DecodeError::None) {
        ++counters_.invalid_packets;
        return;
    }

    ClientRecord* client = RegisterInputClient(from, decoded.payload.client_id);
    if (client == nullptr) {
        return;
    }

    // 多客户端阶段仍然采用“每个 session 保留最近一次输入”的轻量模型。
    // 后续做 prediction/reconciliation 时，再把这里替换成按 input_sequence 排队消费。
    client->latest_input.player_entity_id = client->player_entity_id;
    client->latest_input.input_sequence = decoded.payload.input_sequence;
    client->latest_input.move_x = decoded.payload.move_x;
    client->latest_input.move_z = decoded.payload.move_z;
    client->latest_input.buttons = decoded.payload.buttons;
    ++counters_.valid_inputs;
}

void DsServer::HandlePing(const std::uint8_t* data, std::size_t size, const net::Endpoint& from) {
    const auto decoded = xdpg::DecodePing(data, size);
    if (decoded.error != xdpg::DecodeError::None) {
        ++counters_.invalid_packets;
        return;
    }

    if (ClientRecord* client = FindClient(from)) {
        client->last_seen = std::chrono::steady_clock::now();
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
    for (const auto& client : clients_) {
        world_.ApplyInput(client.latest_input);
    }
    world_.Step();
}

void DsServer::SendSnapshot() {
    RemoveStaleClients();
    if (clients_.empty()) {
        return;
    }

    const auto current_entities = BuildCurrentSnapshotEntities();
    for (auto& client : clients_) {
        const auto interest_entities = BuildInterestSnapshotForClient(current_entities, client);
        const auto changed_entities = CollectChangedEntities(client, interest_entities);
        const auto encoding_mode = ChooseSnapshotEncoding(client, interest_entities,
                                                          changed_entities);
        const auto& entities_to_send =
            encoding_mode == xdpg::SnapshotEncodingMode::Full ? interest_entities
                                                              : changed_entities;
        const std::size_t total_entities = interest_entities.size();
        const std::size_t max_per_packet = xdpg::MaxSnapshotEntitiesPerPacket(encoding_mode);
        const std::size_t chunk_count = entities_to_send.empty()
                                            ? 1
                                            : (entities_to_send.size() + max_per_packet - 1) /
                                                  max_per_packet;

        // snapshot chunking：同一个 server_tick 的可见状态会拆成多个 UDP 包。
        // interest set 是按客户端定制的，因此 full/delta 基线也必须按客户端保存。
        for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            const std::size_t begin = chunk_index * max_per_packet;
            const std::size_t end = begin + max_per_packet < entities_to_send.size()
                                        ? begin + max_per_packet
                                        : entities_to_send.size();

            xdpg::SnapshotPayload snapshot;
            snapshot.server_tick = world_.tick();
            snapshot.last_processed_input_sequence = client.latest_input.input_sequence;
            snapshot.encoding_mode = encoding_mode;
            snapshot.chunk_index = static_cast<std::uint16_t>(chunk_index);
            snapshot.chunk_count = static_cast<std::uint16_t>(chunk_count);
            snapshot.total_entity_count = static_cast<std::uint16_t>(total_entities);
            snapshot.entities.assign(entities_to_send.begin() + begin,
                                     entities_to_send.begin() + end);

            const auto packet = xdpg::EncodeSnapshot(outbound_sequence_++, snapshot);
            std::string error;
            if (socket_.Send(packet.data(), packet.size(), client.endpoint, &error)) {
                ++counters_.sent_snapshots;
            } else {
                std::cerr << "send SNAPSHOT failed to " << client.endpoint_key << ": "
                          << error << '\n';
            }
        }

        client.previous_snapshot_entities = interest_entities;
        client.has_previous_snapshot = true;
        client.snapshots_since_full =
            encoding_mode == xdpg::SnapshotEncodingMode::Full ? 0 : client.snapshots_since_full + 1;
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
              << " pongs=" << pongs
              << " clients=" << clients_.size() << '\n';
}

DsServer::ClientRecord* DsServer::FindClient(const net::Endpoint& endpoint) {
    const std::string key = endpoint.ToString();
    for (auto& client : clients_) {
        if (client.endpoint_key == key) {
            return &client;
        }
    }
    return nullptr;
}

DsServer::ClientRecord* DsServer::RegisterInputClient(
    const net::Endpoint& endpoint,
    std::uint64_t client_id) {
    const auto now = std::chrono::steady_clock::now();
    const std::string key = endpoint.ToString();

    for (auto& client : clients_) {
        if (client.endpoint_key == key) {
            client.endpoint = endpoint;
            client.last_seen = now;
            client.client_id = client_id;
            return &client;
        }
    }

    // 这里先给一个保守上限，避免非法来源疯狂换端口时把内存拖大。
    // 后续做正式 client table 时，会加入 token/握手/限速和更明确的驱逐策略。
    constexpr std::size_t kMaxClients = 16;
    if (clients_.size() >= kMaxClients ||
        next_player_entity_id_ >= xdpg::kFirstPlayerEntityId + xdpg::kMaxPlayerEntities) {
        std::cerr << "refuse new client " << key << ": max player sessions reached\n";
        return nullptr;
    }

    const std::uint32_t player_entity_id = next_player_entity_id_;
    if (!world_.AddPlayer(player_entity_id, SpawnPositionForPlayer(player_entity_id))) {
        std::cerr << "refuse new client " << key << ": failed to create player entity\n";
        return nullptr;
    }
    ++next_player_entity_id_;

    ClientRecord record;
    record.endpoint = endpoint;
    record.endpoint_key = key;
    record.last_seen = now;
    record.client_id = client_id;
    record.player_entity_id = player_entity_id;
    record.latest_input.player_entity_id = player_entity_id;
    clients_.push_back(record);

    // 新 session 加入后，玩家列表发生变化；强制所有客户端下一帧发 full snapshot，
    // 让 viewer 立即拿到新的 player 列表和自己的 local-player 标记。
    for (auto& client : clients_) {
        client.has_previous_snapshot = false;
        client.snapshots_since_full = 0;
    }

    std::cout << "[session] client=" << key
              << " client_id=" << client_id
              << " player_entity_id=" << player_entity_id << '\n';
    return &clients_.back();
}

void DsServer::RemoveStaleClients() {
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kClientTimeout = std::chrono::seconds(10);

    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
                       [now, kClientTimeout](const ClientRecord& client) {
                           return now - client.last_seen > kClientTimeout;
                       }),
        clients_.end());
}

std::vector<xdpg::EntitySnapshot> DsServer::BuildCurrentSnapshotEntities() const {
    const auto states = world_.CollectEntityStates();
    std::vector<xdpg::EntitySnapshot> entities;
    entities.reserve(states.size());

    for (const auto& state : states) {
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
        // render snapshot 不再发送线速度/角速度。后续如果需要调试物理能量、
        // 插值或预测，可以新增 DebugSnapshot 或按需字段，而不是让公网 viewer 背负调试带宽。
        entities.push_back(entity);
    }

    std::sort(entities.begin(), entities.end(), [](const auto& lhs, const auto& rhs) {
        return DenseIndexFromEntityId(lhs.entity_id) < DenseIndexFromEntityId(rhs.entity_id);
    });
    return entities;
}

std::vector<xdpg::EntitySnapshot> DsServer::BuildInterestSnapshotForClient(
    const std::vector<xdpg::EntitySnapshot>& entities,
    const ClientRecord& client) const {
    struct Candidate {
        xdpg::EntitySnapshot entity;
        double priority = 0.0;
    };

    const xdpg::EntitySnapshot* local_player =
        FindSnapshotEntity(entities, client.player_entity_id);
    const bool unlimited_budget = config_.snapshot_entity_budget == 0;
    const std::uint32_t budget = unlimited_budget ? UINT32_MAX : config_.snapshot_entity_budget;
    const double radius = config_.interest_radius_meters;
    const double radius2 = radius * radius;

    std::vector<xdpg::EntitySnapshot> selected_players;
    std::vector<Candidate> small_candidates;

    for (auto entity : entities) {
        if (entity.entity_type == xdpg::EntityType::PlayerCube) {
            if (entity.entity_id == client.player_entity_id) {
                entity.flags |= xdpg::kEntityFlagLocalPlayer;
            }
            // 玩家实体数量很小，而且对多人验证很重要，因此始终进入 interest set。
            selected_players.push_back(entity);
            continue;
        }

        if (entity.entity_type != xdpg::EntityType::SmallCube) {
            continue;
        }

        double distance2 = 0.0;
        if (local_player != nullptr) {
            distance2 = DistanceSquared(entity, *local_player);
            if (radius > 0.0 && distance2 > radius2) {
                continue;
            }
        }

        double priority = 0.0;
        if (radius > 0.0 && local_player != nullptr) {
            const double distance = std::sqrt(distance2);
            const double distance_factor = 1.0 - distance / radius;
            priority += (distance_factor > 0.0 ? distance_factor : 0.0) * 100.0;
        }
        if ((entity.flags & xdpg::kEntityFlagInteracting) != 0u) {
            priority += 80.0;
        }
        if (const xdpg::EntitySnapshot* previous =
                FindSnapshotEntity(client.previous_snapshot_entities, entity.entity_id)) {
            if (SnapshotEntityChanged(*previous, entity)) {
                priority += 40.0;
            }
        } else {
            // 新进入兴趣范围的实体需要尽快发给客户端，避免 viewer 长时间看不到它。
            priority += 30.0;
        }

        small_candidates.push_back(Candidate{entity, priority});
    }

    std::sort(selected_players.begin(), selected_players.end(), [](const auto& lhs, const auto& rhs) {
        return DenseIndexFromEntityId(lhs.entity_id) < DenseIndexFromEntityId(rhs.entity_id);
    });
    std::sort(small_candidates.begin(), small_candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                  if (std::fabs(lhs.priority - rhs.priority) > 0.0001) {
                      return lhs.priority > rhs.priority;
                  }
                  return DenseIndexFromEntityId(lhs.entity.entity_id) <
                         DenseIndexFromEntityId(rhs.entity.entity_id);
              });

    std::vector<xdpg::EntitySnapshot> selected;
    const std::size_t reserve_count =
        unlimited_budget || entities.size() < static_cast<std::size_t>(budget)
            ? entities.size()
            : static_cast<std::size_t>(budget);
    selected.reserve(reserve_count);
    for (const auto& player : selected_players) {
        selected.push_back(player);
    }

    const std::size_t remaining_budget =
        unlimited_budget || selected.size() >= budget
            ? (unlimited_budget ? small_candidates.size() : 0)
            : static_cast<std::size_t>(budget) - selected.size();
    for (std::size_t i = 0; i < small_candidates.size() && i < remaining_budget; ++i) {
        selected.push_back(small_candidates[i].entity);
    }

    // 协议的 delta-offset 编码要求实体按 dense index 单调递增；优先级只影响“选谁”，
    // 真正写包前仍要回到稳定顺序。
    std::sort(selected.begin(), selected.end(), [](const auto& lhs, const auto& rhs) {
        return DenseIndexFromEntityId(lhs.entity_id) < DenseIndexFromEntityId(rhs.entity_id);
    });
    return selected;
}

std::vector<xdpg::EntitySnapshot> DsServer::CollectChangedEntities(
    const ClientRecord& client,
    const std::vector<xdpg::EntitySnapshot>& current) const {
    if (!client.has_previous_snapshot ||
        !SameEntitySet(client.previous_snapshot_entities, current)) {
        return current;
    }

    std::vector<xdpg::EntitySnapshot> changed;
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (SnapshotEntityChanged(client.previous_snapshot_entities[i], current[i])) {
            changed.push_back(current[i]);
        }
    }
    return changed;
}

xdpg::SnapshotEncodingMode DsServer::ChooseSnapshotEncoding(
    const ClientRecord& client,
    const std::vector<xdpg::EntitySnapshot>& current,
    const std::vector<xdpg::EntitySnapshot>& changed) const {
    // 每秒强制发送一次 full keyframe，用来让新 viewer 建立基线，也让 UDP 丢包造成的
    // delta 状态漂移可以自动恢复。第一版不做可靠重传，先用周期性 full 保守兜底。
    if (!client.has_previous_snapshot ||
        client.snapshots_since_full >= config_.snapshot_rate ||
        !SameEntitySet(client.previous_snapshot_entities, current)) {
        return xdpg::SnapshotEncodingMode::Full;
    }

    xdpg::SnapshotEncodingMode best_mode = xdpg::SnapshotEncodingMode::Full;
    std::size_t best_cost = ChunkedSnapshotCost(best_mode, current.size());

    if (CanUseOffsetEncoding(changed, xdpg::SnapshotEncodingMode::DeltaOffsetU8)) {
        const std::size_t cost =
            ChunkedSnapshotCost(xdpg::SnapshotEncodingMode::DeltaOffsetU8, changed.size());
        if (cost < best_cost) {
            best_cost = cost;
            best_mode = xdpg::SnapshotEncodingMode::DeltaOffsetU8;
        }
    }

    if (CanUseOffsetEncoding(changed, xdpg::SnapshotEncodingMode::DeltaOffsetU16)) {
        const std::size_t cost =
            ChunkedSnapshotCost(xdpg::SnapshotEncodingMode::DeltaOffsetU16, changed.size());
        if (cost < best_cost) {
            best_mode = xdpg::SnapshotEncodingMode::DeltaOffsetU16;
        }
    }

    return best_mode;
}

}  // namespace xdpg::server
