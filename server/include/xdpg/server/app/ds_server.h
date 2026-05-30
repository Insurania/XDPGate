#pragma once

#include "xdpg/physics/input_command.h"
#include "xdpg/physics/ode_world.h"
#include "xdpg/server/net/udp_socket.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace xdpg::server {

struct DsServerConfig {
    std::uint16_t port = 40000;
    std::uint32_t tick_rate = 60;
    std::uint32_t snapshot_rate = 20;
    std::uint32_t small_cube_count = 15;
};

class DsServer {
public:
    explicit DsServer(DsServerConfig config);

    int Run(const std::atomic_bool& stop_requested);

private:
    struct Counters {
        std::uint64_t received_packets = 0;
        std::uint64_t valid_inputs = 0;
        std::uint64_t invalid_packets = 0;
        std::uint64_t sent_snapshots = 0;
        std::uint64_t sent_pongs = 0;
    };

    void DrainSocket();
    void HandlePacket(const std::uint8_t* data, std::size_t size, const Endpoint& from);
    void HandleInput(const std::uint8_t* data, std::size_t size, const Endpoint& from);
    void HandlePing(const std::uint8_t* data, std::size_t size, const Endpoint& from);
    void StepSimulation();
    void SendSnapshot();
    void LogStatsIfDue();

    DsServerConfig config_;
    UdpSocket socket_;
    physics::OdeWorld world_;
    physics::InputCommand latest_input_;
    Endpoint latest_client_;
    bool has_client_ = false;
    std::uint32_t outbound_sequence_ = 1;
    Counters counters_;
    Counters last_logged_counters_;
    std::chrono::steady_clock::time_point next_stats_log_;
};

}  // namespace xdpg::server

