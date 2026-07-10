// Speed-based walk/run inference — Story 002 (creatures always run on WotLK/AzerothCore).
//
// WotLK carries no walkmode spline flag (AzerothCore's MoveSplineFlag has none, and ground
// moves serialize flags=0 — verified against server source). Walk vs run is expressed only by
// the spline's speed: base WoW walk ~2.5 yd/s, run ~7.0 yd/s. isWalkingSpeed() classifies a
// move by pathLength/duration against a midpoint threshold. These tests pin the walk/run
// boundary and the degenerate (snap/zero) cases. If the classifier regresses to "always run"
// (the old hardcoded behavior), the walk cases fail.
#include <catch_amalgamated.hpp>
#include "game/spline_packet.hpp"

using wowee::game::isWalkingSpeed;
using wowee::game::WALK_RUN_SPEED_THRESHOLD;

TEST_CASE("walk speed: a move at ~walk speed (2.5 yd/s) is a walk", "[locomotion][walkspeed]") {
    CHECK(isWalkingSpeed(25.0f, 10000) == true);   // 25 yd / 10 s = 2.5 yd/s
    CHECK(isWalkingSpeed(2.5f, 1000) == true);      // 2.5 yd/s
    // A long cyclic patrol still walks: 30 yd over 12 s = 2.5 yd/s.
    CHECK(isWalkingSpeed(30.0f, 12000) == true);
}

TEST_CASE("walk speed: a move at ~run speed (7+ yd/s) is a run", "[locomotion][walkspeed]") {
    CHECK(isWalkingSpeed(70.0f, 10000) == false);   // 7.0 yd/s
    CHECK(isWalkingSpeed(7.0f, 1000) == false);
    CHECK(isWalkingSpeed(50.0f, 5000) == false);    // 10 yd/s (fleeing/chasing)
}

TEST_CASE("walk speed: the boundary sits at the threshold and is inclusive", "[locomotion][walkspeed]") {
    REQUIRE(WALK_RUN_SPEED_THRESHOLD == Catch::Approx(4.0f));
    CHECK(isWalkingSpeed(4.0f, 1000) == true);       // exactly threshold → walk
    CHECK(isWalkingSpeed(4.01f, 1000) == false);     // just over → run
}

TEST_CASE("walk speed: degenerate moves are not walks", "[locomotion][walkspeed]") {
    CHECK(isWalkingSpeed(10.0f, 0) == false);        // instantaneous snap (duration 0)
    CHECK(isWalkingSpeed(0.0f, 1000) == false);      // zero-length move
}
