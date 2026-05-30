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

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::unique_ptr<xdpg::physics::OdeWorld> g_world;
std::uint32_t g_input_sequence = 0;
bool g_previous_boost_down = false;
float g_camera_yaw_degrees = 130.0f;

bool IsKeyDown(int key) {
#if defined(_WIN32)
    return (GetAsyncKeyState(key) & 0x8000) != 0;
#else
    (void)key;
    return false;
#endif
}

xdpg::physics::InputCommand BuildInputCommand() {
    xdpg::physics::InputCommand input;
    input.input_sequence = ++g_input_sequence;

    // 项目物理世界使用游戏常见坐标：Y 轴向上，X/Z 为水平面。
    // drawstuff 自身文档按 Z 轴向上理解，所以渲染时会做坐标转换。
    // drawstuff 的 command 回调没有 key-up 事件，所以 Windows 本地 viewer
    // 每帧轮询真实键盘状态：按住才移动，松开就停止继续施力。
    input.move_x = (IsKeyDown('D') ? 1.0 : 0.0) - (IsKeyDown('A') ? 1.0 : 0.0);
    input.move_z = (IsKeyDown('W') ? 1.0 : 0.0) - (IsKeyDown('S') ? 1.0 : 0.0);

#if defined(_WIN32)
    const bool boost_down = IsKeyDown(VK_SPACE);
#else
    const bool boost_down = false;
#endif
    input.buttons = (boost_down && !g_previous_boost_down) ? 1u : 0u;
    g_previous_boost_down = boost_down;
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
    float sides[3]{160.0f, 160.0f, 0.04f};
    dsSetColor(1.0f, 1.0f, 1.0f);
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
        dsSetColor(1.0f, 0.05f, 0.04f);
    } else if (entity.is_interacting) {
        dsSetColor(1.0f, 0.05f, 0.04f);
    } else {
        dsSetColor(0.55f, 0.55f, 0.55f);
    }

    dsDrawBox(pos, rot, sides);
}

void UpdateFollowCamera(const xdpg::physics::EntityState& player) {
    float player_pos[3];
    OdeToDrawPosition(player.position, player_pos);

    // drawstuff 的相机用 xyz + hpr 表示。这里不跟随 cube 自身旋转，
    // 而是固定在玩家斜后上方，避免 player 翻滚时镜头也跟着翻到天旋地转。
    constexpr float kFollowDistance = 8.0f;
    constexpr float kFollowHeight = 6.5f;
    constexpr float kViewYawDegrees = 130.0f;
    constexpr float kPitchDegrees = -38.0f;
    constexpr float kRadiansPerDegree = 3.1415926535f / 180.0f;

    g_camera_yaw_degrees = kViewYawDegrees;
    const float yaw_radians = g_camera_yaw_degrees * kRadiansPerDegree;
    float xyz[3]{
        player_pos[0] - std::cos(yaw_radians) * kFollowDistance,
        player_pos[1] - std::sin(yaw_radians) * kFollowDistance,
        player_pos[2] + kFollowHeight,
    };
    float hpr[3]{
        g_camera_yaw_degrees,
        kPitchDegrees,
        0.0f,
    };

    dsSetViewpoint(xyz, hpr);
}

void Start() {
    static float xyz[3] = {6.0f, -6.0f, 6.5f};
    static float hpr[3] = {130.0f, -38.0f, 0.0f};
    dsSetViewpoint(xyz, hpr);
    std::cout << "XDPGate ODE world viewer\n"
              << "Hold W/A/S/D to move, tap Space to boost, press Q or Esc to quit.\n";
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

    const auto entities = g_world->CollectEntityStates();
    for (const auto& entity : entities) {
        if (entity.kind == xdpg::physics::EntityKind::PlayerCube) {
            UpdateFollowCamera(entity);
            break;
        }
    }

    DrawGround();
    for (const auto& entity : entities) {
        DrawEntity(entity);
    }
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
        xdpg::physics::OdeWorldConfig config;
        config.small_cube_count = 180;
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
