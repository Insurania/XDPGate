#include "xdpg/physics/ode_world.h"

#include <ode/ode.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xdpg::physics {
namespace {

constexpr std::uint32_t kPlayerEntityId = 1;
constexpr std::uint32_t kSmallCubeEntityIdBase = 1000;
constexpr double kGravity = -9.81;
constexpr double kPlayerCubeSize = 1.0;
constexpr double kSmallCubeSize = 0.55;
constexpr double kPlayerMass = 5.0;
constexpr double kSmallCubeMass = 1.0;
constexpr double kMoveForce = 85.0;
constexpr double kBoostImpulse = 1.8;
constexpr std::uint32_t kBoostButtonMask = 1u << 0u;
constexpr int kMaxContactsPerPair = 8;

double Clamp(double value, double min_value, double max_value) {
    return std::max(min_value, std::min(max_value, value));
}

Vec3 ReadVec3(const dReal* values) {
    return Vec3{
        static_cast<double>(values[0]),
        static_cast<double>(values[1]),
        static_cast<double>(values[2]),
    };
}

Quat ReadQuat(const dReal* values) {
    // ODE 的 dQuaternion 存储顺序是 w, x, y, z；协议和渲染层更常用 x, y, z, w。
    return Quat{
        static_cast<double>(values[1]),
        static_cast<double>(values[2]),
        static_cast<double>(values[3]),
        static_cast<double>(values[0]),
    };
}

}  // namespace

OdeWorld::OdeWorld(const OdeWorldConfig& config) : config_(config) {
    if (config_.small_cube_count > 64) {
        throw std::invalid_argument("small_cube_count too large for phase 1 world");
    }
    if (config_.fixed_dt <= 0.0) {
        throw std::invalid_argument("fixed_dt must be positive");
    }

    dInitODE();
    CreateWorld();
    CreateGround();
    CreatePlayerCube();
    CreateSmallCubes();
}

OdeWorld::~OdeWorld() {
    if (contact_group_ != nullptr) {
        dJointGroupDestroy(contact_group_);
        contact_group_ = nullptr;
    }
    if (space_ != nullptr) {
        dSpaceDestroy(space_);
        space_ = nullptr;
    }
    if (world_ != nullptr) {
        dWorldDestroy(world_);
        world_ = nullptr;
    }
    dCloseODE();
}

void OdeWorld::ApplyInput(const InputCommand& input) {
    if (entities_.empty()) {
        return;
    }

    DynamicEntity& player = entities_.front();
    const double move_x = Clamp(input.move_x, -1.0, 1.0);
    const double move_z = Clamp(input.move_z, -1.0, 1.0);
    const double length = std::sqrt(move_x * move_x + move_z * move_z);
    if (length > 0.0001) {
        // 玩家输入只转换成作用在刚体质心附近的力，不直接改位置。
        // 这样 player cube 推动 small cubes 时，结果仍然由 ODE 碰撞求解决定。
        const double normalized_x = move_x / std::max(1.0, length);
        const double normalized_z = move_z / std::max(1.0, length);
        dBodyAddForce(player.body, normalized_x * kMoveForce, 0.0, normalized_z * kMoveForce);

        if ((input.buttons & kBoostButtonMask) != 0u) {
            // ODE C API 没有直接的 AddImpulse 接口。这里用“一次性增加线速度”
            // 近似水平冲量，后续如果需要更严格的动量控制，可以按质量换算成力并
            // 只持续一个 fixed tick。
            const dReal* velocity = dBodyGetLinearVel(player.body);
            dBodySetLinearVel(player.body, velocity[0] + normalized_x * kBoostImpulse,
                              velocity[1], velocity[2] + normalized_z * kBoostImpulse);
        }
    }

    last_processed_input_sequence_ = input.input_sequence;
}

void OdeWorld::Step() {
    dSpaceCollide(space_, this, &OdeWorld::NearCallback);

    // QuickStep 比完整 Step 更适合实时服务器：速度更快，稳定性足够支撑 toy DS。
    dWorldQuickStep(world_, config_.fixed_dt);
    dJointGroupEmpty(contact_group_);
    ++tick_;
}

std::vector<EntityState> OdeWorld::CollectEntityStates() const {
    std::vector<EntityState> states;
    states.reserve(entities_.size());

    for (const DynamicEntity& entity : entities_) {
        EntityState state;
        state.entity_id = entity.entity_id;
        state.kind = entity.kind;
        state.cube_size = entity.half_extent * 2.0;
        state.position = ReadVec3(dBodyGetPosition(entity.body));
        state.rotation = ReadQuat(dBodyGetQuaternion(entity.body));
        state.linear_velocity = ReadVec3(dBodyGetLinearVel(entity.body));
        state.angular_velocity = ReadVec3(dBodyGetAngularVel(entity.body));
        states.push_back(state);
    }

    return states;
}

void OdeWorld::NearCallback(void* data, dxGeom* geom_a, dxGeom* geom_b) {
    auto* self = static_cast<OdeWorld*>(data);
    self->HandleCollision(geom_a, geom_b);
}

void OdeWorld::CreateWorld() {
    world_ = dWorldCreate();
    space_ = dHashSpaceCreate(nullptr);
    contact_group_ = dJointGroupCreate(0);

    if (world_ == nullptr || space_ == nullptr || contact_group_ == nullptr) {
        throw std::runtime_error("failed to create ODE world");
    }

    dWorldSetGravity(world_, 0.0, kGravity, 0.0);
    dWorldSetCFM(world_, 1e-5);
    dWorldSetERP(world_, 0.2);
    dWorldSetQuickStepNumIterations(world_, 24);
}

void OdeWorld::CreateGround() {
    // 无限平面只参与碰撞；第一阶段 snapshot 不发送 ground，viewer 本地画地面即可。
    ground_geom_ = dCreatePlane(space_, 0.0, 1.0, 0.0, 0.0);
    if (ground_geom_ == nullptr) {
        throw std::runtime_error("failed to create ground plane");
    }
}

void OdeWorld::CreatePlayerCube() {
    entities_.push_back(CreateCube(kPlayerEntityId, EntityKind::PlayerCube, kPlayerCubeSize,
                                   kPlayerMass, Vec3{0.0, 1.5, 0.0}));
}

void OdeWorld::CreateSmallCubes() {
    for (std::uint32_t i = 0; i < config_.small_cube_count; ++i) {
        const double row = static_cast<double>(i / 4u);
        const double col = static_cast<double>(i % 4u);
        const Vec3 position{
            -1.8 + col * 1.2,
            0.8 + row * 0.05,
            4.0 + row * 1.2,
        };
        entities_.push_back(CreateCube(kSmallCubeEntityIdBase + i, EntityKind::SmallCube,
                                       kSmallCubeSize, kSmallCubeMass, position));
    }
}

OdeWorld::DynamicEntity OdeWorld::CreateCube(std::uint32_t entity_id, EntityKind kind, double size,
                                             double mass, const Vec3& position) {
    dBodyID body = dBodyCreate(world_);
    dGeomID geom = dCreateBox(space_, size, size, size);
    if (body == nullptr || geom == nullptr) {
        throw std::runtime_error("failed to create cube body or geom");
    }

    dMass ode_mass;
    dMassSetZero(&ode_mass);
    dMassSetBoxTotal(&ode_mass, mass, size, size, size);
    dBodySetMass(body, &ode_mass);
    dBodySetPosition(body, position.x, position.y, position.z);
    dGeomSetBody(geom, body);

    return DynamicEntity{
        entity_id,
        kind,
        body,
        geom,
        size * 0.5,
    };
}

void OdeWorld::HandleCollision(dxGeom* geom_a, dxGeom* geom_b) {
    dBodyID body_a = dGeomGetBody(geom_a);
    dBodyID body_b = dGeomGetBody(geom_b);
    if (body_a != nullptr && body_b != nullptr && dAreConnectedExcluding(body_a, body_b, dJointTypeContact)) {
        return;
    }

    dContact contacts[kMaxContactsPerPair];
    for (dContact& contact : contacts) {
        contact.surface.mode = dContactBounce | dContactSoftCFM;
        contact.surface.mu = 0.9;
        contact.surface.bounce = 0.08;
        contact.surface.bounce_vel = 0.1;
        contact.surface.soft_cfm = 1e-5;
    }

    const int count = dCollide(geom_a, geom_b, kMaxContactsPerPair, &contacts[0].geom,
                               sizeof(dContact));
    for (int i = 0; i < count; ++i) {
        dJointID joint = dJointCreateContact(world_, contact_group_, &contacts[i]);
        dJointAttach(joint, body_a, body_b);
    }
}

}  // namespace xdpg::physics
