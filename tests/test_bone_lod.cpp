// Bone-update LOD throttle tests — Story 003 (choppy animation).
//
// The bug: animated models' bone matrices were recomputed only every Nth frame
// starting at just 10 world-units from the camera, while the 3rd-person camera sits
// 10-22 units from the player (up to 50 zoomed out — camera_controller.hpp). So the
// player's OWN model updated every 2nd-4th frame and animations looked like sprite-
// swapping. Bones must update every frame throughout normal view distance.
#include <catch_amalgamated.hpp>
#include "rendering/render_constants.hpp"

using wowee::rendering::boneUpdateIntervalForDistanceSq;

namespace {
// squared distance in world units, for readable call sites
constexpr float sq(float d) { return d * d; }
}  // namespace

TEST_CASE("bone LOD: point-blank updates every frame", "[animation][lod]") {
    CHECK(boneUpdateIntervalForDistanceSq(sq(0.0f)) == 1u);
    CHECK(boneUpdateIntervalForDistanceSq(sq(5.0f)) == 1u);
}

TEST_CASE("bone LOD: player at any normal 3rd-person zoom updates every frame", "[animation][lod]") {
    // The player's own model sits at the camera distance: default 10u, max-normal
    // 22u, max-extended 50u (camera_controller.hpp MAX_DISTANCE_*). ALL of these must
    // be interval 1 — otherwise the player's own animation is choppy (the reported bug).
    CHECK(boneUpdateIntervalForDistanceSq(sq(10.0f)) == 1u);  // default zoom
    CHECK(boneUpdateIntervalForDistanceSq(sq(15.0f)) == 1u);
    CHECK(boneUpdateIntervalForDistanceSq(sq(22.0f)) == 1u);  // max normal zoom
    CHECK(boneUpdateIntervalForDistanceSq(sq(30.0f)) == 1u);
    CHECK(boneUpdateIntervalForDistanceSq(sq(50.0f)) == 1u);  // max extended zoom
}

TEST_CASE("bone LOD: distant units throttle for performance", "[animation][lod]") {
    // Beyond the max view distance, small on-screen units may recompute less often.
    CHECK(boneUpdateIntervalForDistanceSq(sq(60.0f)) == 2u);   // >50u tier
    CHECK(boneUpdateIntervalForDistanceSq(sq(120.0f)) == 4u);  // >100u tier
}

TEST_CASE("bone LOD: interval never decreases as distance grows", "[animation][lod]") {
    // Monotonicity: a farther unit must never recompute MORE often than a nearer one.
    uint32_t prev = 1u;
    for (float d = 0.0f; d <= 200.0f; d += 2.0f) {
        uint32_t iv = boneUpdateIntervalForDistanceSq(sq(d));
        CHECK(iv >= prev);
        prev = iv;
    }
}
