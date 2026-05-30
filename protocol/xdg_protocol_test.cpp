#include "protocol/xdg_protocol.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void RequireNear(float actual, float expected, float epsilon, const std::string& message) {
    if (std::fabs(actual - expected) > epsilon) {
        throw std::runtime_error(message);
    }
}

void TestInputRoundTrip() {
    xdpg::InputPayload input;
    input.client_id = 42;
    input.input_sequence = 1001;
    input.move_x = 0.5f;
    input.move_z = -1.0f;
    input.buttons = 1;
    input.client_timestamp_usec = 123456789;

    const auto packet = xdpg::EncodeInput(7, input);
    Require(packet.size() == 48, "INPUT packet size should be 48 bytes");

    const auto decoded = xdpg::DecodeInput(packet.data(), packet.size());
    Require(decoded.error == xdpg::DecodeError::None, "INPUT decode should succeed");
    Require(decoded.header.packet_type == xdpg::PacketType::Input, "packet type should be INPUT");
    Require(decoded.header.sequence == 7, "header sequence mismatch");
    Require(decoded.payload.client_id == input.client_id, "client_id mismatch");
    Require(decoded.payload.input_sequence == input.input_sequence, "input_sequence mismatch");
    RequireNear(decoded.payload.move_x, input.move_x, 0.0001f, "move_x mismatch");
    RequireNear(decoded.payload.move_z, input.move_z, 0.0001f, "move_z mismatch");
    Require(decoded.payload.buttons == input.buttons, "buttons mismatch");
    Require(decoded.payload.client_timestamp_usec == input.client_timestamp_usec,
            "client timestamp mismatch");
}

void TestSnapshotRoundTrip() {
    xdpg::SnapshotPayload snapshot;
    snapshot.server_tick = 60;
    snapshot.last_processed_input_sequence = 1001;
    snapshot.chunk_index = 0;
    snapshot.chunk_count = 1;
    snapshot.total_entity_count = 2;

    xdpg::EntitySnapshot player;
    player.entity_id = 1;
    player.entity_type = xdpg::EntityType::PlayerCube;
    player.position[0] = 1.0f;
    player.position[1] = 2.0f;
    player.position[2] = 3.0f;
    player.rotation[1] = 0.38268343f;
    player.rotation[3] = 0.9238795f;
    player.linear_velocity[0] = 4.0f;
    player.angular_velocity[2] = 5.0f;

    xdpg::EntitySnapshot cube;
    cube.entity_id = 1000;
    cube.entity_type = xdpg::EntityType::SmallCube;
    cube.position[0] = -1.0f;
    cube.position[1] = 0.25f;
    cube.position[2] = 9.5f;

    snapshot.entities.push_back(player);
    snapshot.entities.push_back(cube);

    const auto packet = xdpg::EncodeSnapshot(8, snapshot);
    Require(packet.size() == 82, "SNAPSHOT packet size should be 82 bytes");

    const auto decoded = xdpg::DecodeSnapshot(packet.data(), packet.size());
    Require(decoded.error == xdpg::DecodeError::None, "SNAPSHOT decode should succeed");
    Require(decoded.header.packet_type == xdpg::PacketType::Snapshot,
            "packet type should be SNAPSHOT");
    Require(decoded.header.sequence == 8, "snapshot header sequence mismatch");
    Require(decoded.payload.server_tick == snapshot.server_tick, "server_tick mismatch");
    Require(decoded.payload.last_processed_input_sequence ==
                snapshot.last_processed_input_sequence,
            "last_processed_input_sequence mismatch");
    Require(decoded.payload.chunk_index == 0, "snapshot chunk_index mismatch");
    Require(decoded.payload.chunk_count == 1, "snapshot chunk_count mismatch");
    Require(decoded.payload.total_entity_count == 2, "snapshot total_entity_count mismatch");
    Require(decoded.payload.entities.size() == 2, "entity count mismatch");
    Require(decoded.payload.entities[0].entity_id == 1, "player entity_id mismatch");
    Require(decoded.payload.entities[0].entity_type == xdpg::EntityType::PlayerCube,
            "player entity_type mismatch");
    RequireNear(decoded.payload.entities[0].position[1], 2.0f, 0.001f, "player y mismatch");
    RequireNear(decoded.payload.entities[0].rotation[1], player.rotation[1], 0.0001f,
                "player rotation y mismatch");
    RequireNear(decoded.payload.entities[0].rotation[3], player.rotation[3], 0.0001f,
                "player rotation w mismatch");
    RequireNear(decoded.payload.entities[0].linear_velocity[0], 0.0f, 0.0001f,
                "render snapshot should not carry linear velocity");
    Require(decoded.payload.entities[1].entity_id == 1000, "small cube entity_id mismatch");
    RequireNear(decoded.payload.entities[1].position[2], 9.5f, 0.01f, "small cube z mismatch");
}

void TestHeaderValidation() {
    const auto packet = xdpg::EncodePing(9, xdpg::PingPayload{123});
    auto bad_magic = packet;
    bad_magic[0] = 'B';
    Require(xdpg::DecodeHeaderOnly(bad_magic.data(), bad_magic.size()).error ==
                xdpg::DecodeError::InvalidMagic,
            "bad magic should fail");

    auto bad_version = packet;
    bad_version[4] = 99;
    Require(xdpg::DecodeHeaderOnly(bad_version.data(), bad_version.size()).error ==
                xdpg::DecodeError::InvalidVersion,
            "bad version should fail");

    auto bad_payload_size = packet;
    bad_payload_size[12] = 0xff;
    Require(xdpg::DecodeHeaderOnly(bad_payload_size.data(), bad_payload_size.size()).error ==
                xdpg::DecodeError::InvalidPayloadSize,
            "bad payload size should fail");
}

void TestPingPongRoundTrip() {
    const auto ping_packet = xdpg::EncodePing(10, xdpg::PingPayload{123456});
    const auto decoded_ping = xdpg::DecodePing(ping_packet.data(), ping_packet.size());
    Require(decoded_ping.error == xdpg::DecodeError::None, "PING decode should succeed");
    Require(decoded_ping.header.sequence == 10, "PING sequence mismatch");
    Require(decoded_ping.payload.timestamp_usec == 123456, "PING timestamp mismatch");

    const auto pong_packet = xdpg::EncodePong(11, xdpg::PongPayload{123456, 222222});
    const auto decoded_pong = xdpg::DecodePong(pong_packet.data(), pong_packet.size());
    Require(decoded_pong.error == xdpg::DecodeError::None, "PONG decode should succeed");
    Require(decoded_pong.header.sequence == 11, "PONG sequence mismatch");
    Require(decoded_pong.payload.timestamp_usec == 123456, "PONG timestamp mismatch");
    Require(decoded_pong.payload.server_timestamp_usec == 222222,
            "PONG server timestamp mismatch");
}

}  // namespace

int main() {
    try {
        TestInputRoundTrip();
        TestSnapshotRoundTrip();
        TestHeaderValidation();
        TestPingPongRoundTrip();
    } catch (const std::exception& e) {
        std::cerr << "协议测试失败: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "协议测试通过\n";
    return EXIT_SUCCESS;
}
