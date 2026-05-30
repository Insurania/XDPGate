#pragma once

#include <cstdint>

namespace xdpg::physics {

struct InputCommand {
    // server session 分配的玩家实体。客户端不能直接声明自己控制哪个 entity；
    // server 在收到 INPUT 后把 endpoint 映射到 player_entity_id，再写入这里。
    std::uint32_t player_entity_id = 0;
    std::uint32_t input_sequence = 0;
    double move_x = 0.0;
    double move_z = 0.0;
    std::uint32_t buttons = 0;
};

}  // namespace xdpg::physics
