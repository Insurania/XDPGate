#include "xdpg/physics/ode_world.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestWorldStepsAndExportsStates() {
    xdpg::physics::OdeWorldConfig config;
    config.small_cube_count = 4;
    config.fixed_dt = 1.0 / 60.0;

    xdpg::physics::OdeWorld world(config);
    Require(world.tick() == 0, "初始 tick 应为 0");
    Require(world.AddPlayer(1, xdpg::physics::Vec3{0.0, 1.5, 0.0}),
            "应该能添加第一个 player cube");

    for (std::uint32_t i = 0; i < 90; ++i) {
        xdpg::physics::InputCommand input;
        input.player_entity_id = 1;
        input.input_sequence = i + 1;
        input.move_x = 1.0;
        input.move_z = 0.0;
        world.ApplyInput(input);
        world.Step();
    }

    const auto states = world.CollectEntityStates();
    Require(world.tick() == 90, "tick 应该随 Step 增加");
    Require(world.last_processed_input_sequence() == 90, "last input sequence 不正确");
    Require(states.size() == 5, "entity 数量应该是 1 个 player + 4 个 small cube");
    const auto player = std::find_if(states.begin(), states.end(), [](const auto& state) {
        return state.entity_id == 1;
    });
    Require(player != states.end(), "应该导出 player cube");
    Require(player->position.x > 0.05, "player cube 应该沿 X 方向移动");
    Require(player->position.y > 0.2, "player cube 不应该掉穿地面");
}

}  // namespace

int main() {
    try {
        TestWorldStepsAndExportsStates();
    } catch (const std::exception& e) {
        std::cerr << "ODE world 测试失败: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "ODE world 测试通过\n";
    return EXIT_SUCCESS;
}
