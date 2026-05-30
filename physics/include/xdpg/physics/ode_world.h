#pragma once

#include "xdpg/physics/entity_state.h"
#include "xdpg/physics/input_command.h"

#include <cstdint>
#include <vector>

struct dxBody;
struct dxGeom;
struct dxJointGroup;
struct dxSpace;
struct dxWorld;

namespace xdpg::physics {

struct OdeWorldConfig {
    std::uint32_t small_cube_count = 48;
    double fixed_dt = 1.0 / 60.0;
};

class OdeWorld {
public:
    explicit OdeWorld(const OdeWorldConfig& config = {});
    ~OdeWorld();

    OdeWorld(const OdeWorld&) = delete;
    OdeWorld& operator=(const OdeWorld&) = delete;

    void ApplyInput(const InputCommand& input);
    void Step();

    std::uint64_t tick() const { return tick_; }
    std::uint32_t last_processed_input_sequence() const {
        return last_processed_input_sequence_;
    }

    std::vector<EntityState> CollectEntityStates() const;

private:
    struct DynamicEntity {
        std::uint32_t entity_id = 0;
        EntityKind kind = EntityKind::SmallCube;
        dxBody* body = nullptr;
        dxGeom* geom = nullptr;
        double half_extent = 0.5;
    };

    static void NearCallback(void* data, dxGeom* geom_a, dxGeom* geom_b);
    void CreateWorld();
    void CreateGround();
    void CreatePlayerCube();
    void CreateSmallCubes();
    DynamicEntity CreateCube(std::uint32_t entity_id, EntityKind kind, double size, double mass,
                             const Vec3& position);
    void HandleCollision(dxGeom* geom_a, dxGeom* geom_b);
    void ClampPlayerVelocity();

    OdeWorldConfig config_;
    dxWorld* world_ = nullptr;
    dxSpace* space_ = nullptr;
    dxJointGroup* contact_group_ = nullptr;
    dxGeom* ground_geom_ = nullptr;
    std::vector<DynamicEntity> entities_;
    std::uint64_t tick_ = 0;
    std::uint32_t last_processed_input_sequence_ = 0;
};

}  // namespace xdpg::physics
