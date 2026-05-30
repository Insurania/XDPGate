#pragma once

#include <cstdint>

namespace xdpg::physics {

struct InputCommand {
    std::uint32_t input_sequence = 0;
    double move_x = 0.0;
    double move_z = 0.0;
    std::uint32_t buttons = 0;
};

}  // namespace xdpg::physics

