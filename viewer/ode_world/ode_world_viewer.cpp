#include "xdpg/physics/ode_world.h"

#include <drawstuff/drawstuff.h>
#include <ode/ode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>

namespace {

std::unique_ptr<xdpg::physics::OdeWorld> g_world;
std::uint32_t g_input_sequence = 0;
bool g_move_forward = false;
bool g_move_backward = false;
bool g_move_left = false;
bool g_move_right = false;
bool g_boost = false;

xdpg::physics::InputCommand BuildInputCommand() {
    xdpg::physics::InputCommand input;
    input.input_sequence = ++g_input_sequence;

    // 项目物理世界使用游戏常见坐标：Y 轴向上，X/Z 为水平面。
    // drawstuff 自身文档按 Z 轴向上理解，所以渲染时会做坐标转换。
    input.move_x = (g_move_right ? 1.0 : 0.0) - (g_move_left ? 1.0 : 0.0);
    input.move_z = (g_move_forward ? 1.0 : 0.0) - (g_move_backward ? 1.0 : 0.0);
    input.buttons = g_boost ? 1u : 0u;
    return input;
}

void OdeToDrawPosition(const xdpg::physics::Vec3& in, float out[3]) {
    out[0] = static_cast<float>(in.x);
    out[1] = static_cast<float>(in.z);
    out[2] = static_cast<float>(in.y);
}

std::array<double, 9> QuaternionToMatrix(const xdpg::physics::Quat& q) {
    const double xx = q.x * q.x;
    const double yy = q.y * q.y;
    const double zz = q.z * q.z;
    const double xy = q.x * q.y;
    const double xz = q.x * q.z;
    const double yz = q.y * q.z;
    const double wx = q.w * q.x;
    const double wy = q.w * q.y;
    const double wz = q.w * q.z;

    return {
        1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz),       2.0 * (xz + wy),
        2.0 * (xy + wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx),
        2.0 * (xz - wy),       2.0 * (yz + wx),       1.0 - 2.0 * (xx + yy),
    };
}

void OdeToDrawRotation(const xdpg::physics::Quat& rotation, float out[12]) {
    const auto r = QuaternionToMatrix(rotation);

    // 坐标转换 P: ODE(x,y,z) -> drawstuff(x,z,y)。
    // 对方向矩阵使用 P * R * P，可以保持 cube 的局部轴和世界轴都落到 drawstuff 坐标系。
    const int p[3] = {0, 2, 1};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[row * 4 + col] = static_cast<float>(r[p[row] * 3 + p[col]]);
        }
        out[row * 4 + 3] = 0.0f;
    }
}

void DrawGround() {
    float pos[3]{0.0f, 0.0f, -0.02f};
    float rot[12]{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
    };
    float sides[3]{18.0f, 18.0f, 0.04f};
    dsSetColor(0.25f, 0.28f, 0.30f);
    dsDrawBox(pos, rot, sides);
}

void DrawEntity(const xdpg::physics::EntityState& entity) {
    float pos[3];
    float rot[12];
    float sides[3]{
        static_cast<float>(entity.cube_size),
        static_cast<float>(entity.cube_size),
        static_cast<float>(entity.cube_size),
    };

    OdeToDrawPosition(entity.position, pos);
    OdeToDrawRotation(entity.rotation, rot);

    if (entity.kind == xdpg::physics::EntityKind::PlayerCube) {
        dsSetColor(0.1f, 0.45f, 1.0f);
    } else {
        dsSetColor(0.95f, 0.65f, 0.18f);
    }

    dsDrawBox(pos, rot, sides);
}

void Start() {
    static float xyz[3] = {6.0f, -9.0f, 6.0f};
    static float hpr[3] = {130.0f, -24.0f, 0.0f};
    dsSetViewpoint(xyz, hpr);
    std::cout << "XDPGate ODE world viewer\n"
              << "按 W/A/S/D 控制蓝色 player cube，按 Space 触发 boost，按 Q 或 Esc 退出。\n";
}

void Step(int pause) {
    if (g_world == nullptr) {
        return;
    }

    if (pause == 0) {
        const auto input = BuildInputCommand();
        g_world->ApplyInput(input);
        g_world->Step();
    }

    DrawGround();
    for (const auto& entity : g_world->CollectEntityStates()) {
        DrawEntity(entity);
    }
}

void Command(int cmd) {
    const char key = static_cast<char>(std::tolower(cmd));
    if (key == 'w') {
        g_move_forward = !g_move_forward;
    } else if (key == 's') {
        g_move_backward = !g_move_backward;
    } else if (key == 'a') {
        g_move_left = !g_move_left;
    } else if (key == 'd') {
        g_move_right = !g_move_right;
    } else if (cmd == ' ') {
        g_boost = !g_boost;
    } else if (key == 'q' || cmd == 27) {
        std::exit(0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        xdpg::physics::OdeWorldConfig config;
        config.small_cube_count = 12;
        config.fixed_dt = 1.0 / 60.0;
        g_world = std::make_unique<xdpg::physics::OdeWorld>(config);

        dsFunctions functions{};
        functions.version = DS_VERSION;
        functions.start = &Start;
        functions.step = &Step;
        functions.command = &Command;
        functions.stop = nullptr;
        functions.path_to_textures = DRAWSTUFF_TEXTURE_PATH;

        dsSimulationLoop(argc, argv, 1280, 720, &functions);
    } catch (const std::exception& e) {
        std::cerr << "启动 ODE world viewer 失败: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
