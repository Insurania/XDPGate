#include "xdpg/physics/ode_world.h"

#include <ode/ode.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace xdpg::physics {
namespace {

constexpr std::uint32_t kPlayerEntityId = 1;
constexpr std::uint32_t kSmallCubeEntityIdBase = 1000;
constexpr double kGravity = -9.81;
constexpr double kPlayerCubeSize = 1.0;
constexpr double kSmallCubeSize = 0.32;
constexpr double kPlayerMass = 3.0;
constexpr double kSmallCubeMass = 0.28;
constexpr double kMoveForce = 95.0;
constexpr double kBoostImpulse = 1.2;
constexpr std::uint32_t kBoostButtonMask = 1u << 0u;
constexpr int kMaxContactsPerPair = 8;
constexpr double kPlayerLinearDamping = 0.018;
constexpr double kPlayerAngularDamping = 0.002;
constexpr double kSmallCubeLinearDamping = 0.02;
constexpr double kSmallCubeAngularDamping = 0.025;
constexpr double kMaxPlayerHorizontalSpeed = 5.0;
constexpr double kAttractionRadius = 2.2;
constexpr double kAttractionForce = 9.0;
constexpr double kStoppedLinearSpeed = 0.12;
constexpr double kStoppedAngularSpeed = 0.18;
constexpr std::uint32_t kStoppedTicksToDeactivate = 45;

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
    if (config_.small_cube_count > 256) {
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
    ApplyAttractionForces();
    dSpaceCollide(space_, this, &OdeWorld::NearCallback);

    // QuickStep 比完整 Step 更适合实时服务器：速度更快，稳定性足够支撑 toy DS。
    dWorldQuickStep(world_, config_.fixed_dt);
    ClampPlayerVelocity();
    UpdateInteractionStates();
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
        state.is_interacting = entity.is_interacting;
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
    dWorldSetQuickStepNumIterations(world_, 32);
    dWorldSetLinearDampingThreshold(world_, 0.0);
    dWorldSetAngularDampingThreshold(world_, 0.0);
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
    struct CandidatePosition {
        double x = 0.0;
        double z = 0.0;
        double distance2 = 0.0;
    };

    std::vector<CandidatePosition> candidates;

    // 小 cube 以玩家为中心向外铺开，并按距离排序取前 N 个。
    // 这样增多数量时仍然是“围绕玩家都有”，而不是因为遍历顺序只出现在某个方向。
    constexpr int kGridRadius = 12;
    constexpr double kSpacing = 0.48;
    constexpr double kCenterGap = 1.25;
    for (int z = -kGridRadius; z <= kGridRadius; ++z) {
        for (int x = -kGridRadius; x <= kGridRadius; ++x) {
            const double world_x = static_cast<double>(x) * kSpacing;
            const double world_z = static_cast<double>(z) * kSpacing;
            if (std::abs(world_x) < kCenterGap && std::abs(world_z) < kCenterGap) {
                continue;
            }
            candidates.push_back(CandidatePosition{
                world_x,
                world_z,
                world_x * world_x + world_z * world_z,
            });
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const CandidatePosition& left, const CandidatePosition& right) {
                  return left.distance2 < right.distance2;
              });

    const std::uint32_t count =
        std::min<std::uint32_t>(config_.small_cube_count, static_cast<std::uint32_t>(candidates.size()));
    for (std::uint32_t created = 0; created < count; ++created) {
        const auto& candidate = candidates[created];

        const double height_jitter = static_cast<double>((created % 3u)) * 0.01;
        const Vec3 position{
            candidate.x,
            kSmallCubeSize * 0.5 + height_jitter,
            candidate.z,
        };
        entities_.push_back(CreateCube(kSmallCubeEntityIdBase + created, EntityKind::SmallCube,
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
    dBodySetFiniteRotationMode(body, 1);
    dBodySetMaxAngularSpeed(body, kind == EntityKind::PlayerCube ? 18.0 : 12.0);
    if (kind == EntityKind::PlayerCube) {
        // player cube 的移动力会比较强，靠摩擦和接触点把线性运动转成翻滚。
        // 线速度由 ClampPlayerVelocity 限制，角速度则尽量保留，方便观察 tumbling。
        dBodySetDamping(body, kPlayerLinearDamping, kPlayerAngularDamping);
    } else {
        dBodySetDamping(body, kSmallCubeLinearDamping, kSmallCubeAngularDamping);
    }
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
        // 较高摩擦让地面接触点更容易产生反向切向力矩。
        // 这样即使 input force 仍作用在质心，也更容易从平移转成滚动/翻倒。
        contact.surface.mode = dContactBounce | dContactSoftCFM | dContactApprox1;
        contact.surface.mu = 4.0;
        contact.surface.bounce = 0.03;
        contact.surface.bounce_vel = 0.05;
        contact.surface.soft_cfm = 5e-6;
    }

    const int count = dCollide(geom_a, geom_b, kMaxContactsPerPair, &contacts[0].geom,
                               sizeof(dContact));
    for (int i = 0; i < count; ++i) {
        dJointID joint = dJointCreateContact(world_, contact_group_, &contacts[i]);
        dJointAttach(joint, body_a, body_b);
    }

    if (IsPlayerBody(body_a)) {
        MarkSmallCubeInteracted(body_b);
    } else if (IsPlayerBody(body_b)) {
        MarkSmallCubeInteracted(body_a);
    }
}

void OdeWorld::ApplyAttractionForces() {
    if (entities_.empty()) {
        return;
    }

    const dReal* player_position = dBodyGetPosition(entities_.front().body);
    for (DynamicEntity& entity : entities_) {
        if (entity.kind != EntityKind::SmallCube) {
            continue;
        }

        const dReal* position = dBodyGetPosition(entity.body);
        const double dx = static_cast<double>(player_position[0] - position[0]);
        const double dy = static_cast<double>(player_position[1] - position[1]);
        const double dz = static_cast<double>(player_position[2] - position[2]);
        const double distance2 = dx * dx + dy * dy + dz * dz;
        if (distance2 > kAttractionRadius * kAttractionRadius || distance2 <= 0.0001) {
            continue;
        }

        const double distance = std::sqrt(distance2);
        const double strength = kAttractionForce * (1.0 - distance / kAttractionRadius);
        dBodyAddForce(entity.body, dx / distance * strength, dy / distance * strength,
                      dz / distance * strength);

        // 进入“块魂球”影响范围后只施加朝向玩家中心的力，不把刚体粘到玩家身上。
        // 它仍然会继续参与 ODE 碰撞、摩擦、滚动和翻倒。
        entity.is_interacting = true;
        entity.stopped_ticks = 0;
    }
}

void OdeWorld::UpdateInteractionStates() {
    for (DynamicEntity& entity : entities_) {
        if (entity.kind != EntityKind::SmallCube || !entity.is_interacting) {
            continue;
        }

        const dReal* linear_velocity = dBodyGetLinearVel(entity.body);
        const dReal* angular_velocity = dBodyGetAngularVel(entity.body);
        const double linear_speed = std::sqrt(linear_velocity[0] * linear_velocity[0] +
                                              linear_velocity[1] * linear_velocity[1] +
                                              linear_velocity[2] * linear_velocity[2]);
        const double angular_speed = std::sqrt(angular_velocity[0] * angular_velocity[0] +
                                               angular_velocity[1] * angular_velocity[1] +
                                               angular_velocity[2] * angular_velocity[2]);
        if (linear_speed < kStoppedLinearSpeed && angular_speed < kStoppedAngularSpeed) {
            ++entity.stopped_ticks;
            if (entity.stopped_ticks >= kStoppedTicksToDeactivate) {
                entity.is_interacting = false;
                entity.stopped_ticks = 0;
            }
        } else {
            entity.stopped_ticks = 0;
        }
    }
}

void OdeWorld::ClampPlayerVelocity() {
    if (entities_.empty()) {
        return;
    }

    DynamicEntity& player = entities_.front();
    const dReal* velocity = dBodyGetLinearVel(player.body);
    const double horizontal_speed =
        std::sqrt(velocity[0] * velocity[0] + velocity[2] * velocity[2]);
    if (horizontal_speed <= kMaxPlayerHorizontalSpeed || horizontal_speed <= 0.0001) {
        return;
    }

    const double scale = kMaxPlayerHorizontalSpeed / horizontal_speed;
    dBodySetLinearVel(player.body, velocity[0] * scale, velocity[1], velocity[2] * scale);
}

OdeWorld::DynamicEntity* OdeWorld::FindEntityByBody(dxBody* body) {
    if (body == nullptr) {
        return nullptr;
    }

    for (DynamicEntity& entity : entities_) {
        if (entity.body == body) {
            return &entity;
        }
    }
    return nullptr;
}

const OdeWorld::DynamicEntity* OdeWorld::FindEntityByBody(dxBody* body) const {
    if (body == nullptr) {
        return nullptr;
    }

    for (const DynamicEntity& entity : entities_) {
        if (entity.body == body) {
            return &entity;
        }
    }
    return nullptr;
}

bool OdeWorld::IsPlayerBody(dxBody* body) const {
    const DynamicEntity* entity = FindEntityByBody(body);
    return entity != nullptr && entity->kind == EntityKind::PlayerCube;
}

void OdeWorld::MarkSmallCubeInteracted(dxBody* body) {
    DynamicEntity* entity = FindEntityByBody(body);
    if (entity == nullptr || entity->kind != EntityKind::SmallCube) {
        return;
    }

    entity->is_interacting = true;
    entity->stopped_ticks = 0;
}

}  // namespace xdpg::physics
