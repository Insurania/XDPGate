#pragma once

#include <cstdint>

namespace xdpg::physics {

enum class EntityKind : std::uint16_t {
    PlayerCube = 1,
    SmallCube = 2,
    StaticGround = 3,
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Quat {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
};

struct EntityState {
    std::uint32_t entity_id = 0;
    EntityKind kind = EntityKind::SmallCube;
    Vec3 position;
    Quat rotation;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
};

}  // namespace xdpg::physics

