// Area-trigger box containment (server frame) — Story 007 (cannot exit instances).
//
// The bug: the client tested box triggers in canonical frame after swapping the trigger's
// X/Y but NOT its box length/width — transposing the box. On elongated boxes (e.g. the
// Ragefire Chasm exit, 30.69 x 12.19) this made the client fire CMSG_AREATRIGGER from a spot
// the server's Position::IsWithinBox rejects as "too far", so the exit teleport never fired.
//
// These cases use the REAL RFC exit trigger (AreaTrigger.dbc / areatrigger id 2226, server
// frame) and the player's actual logged fire position, so they pin the exact failure.
#include <catch_amalgamated.hpp>
#include "game/area_trigger_box.hpp"

using wowee::game::isWithinServerBox;

namespace {
// RFC exit trigger 2226 in SERVER frame (from AzerothCore areatrigger table):
//   center (x,y,z) = (2.58019, -0.013587, -13.3668), length 30.69, width 12.19, height 25.56, yaw 0.
constexpr float CX = 2.58019f, CY = -0.013587f, CZ = -13.3668f;
constexpr float YAW = 0.0f;
constexpr float HX = 30.69f * 0.5f;  // 15.345  (server-X half-extent)
constexpr float HY = 12.19f * 0.5f;  // 6.095   (server-Y half-extent)
constexpr float HZ = 25.56f * 0.5f;  // 12.78

// Convert a client canonical (x,y,z) to server frame (swap X/Y) for the call site.
constexpr float sx(float cx, float cy) { (void)cx; return cy; }  // server.X = canonical.Y
constexpr float sy(float cx, float cy) { (void)cy; return cx; }  // server.Y = canonical.X
}  // namespace

TEST_CASE("area-trigger box: the premature fire position is OUTSIDE the server box", "[areatrigger][box]") {
    // Player canonical (-14.3922, 1.62767, -17.4379) at the logged fire → server (1.62767, -14.3922, ...).
    // It is only 0.95u off the long axis but ~14.38u off the SHORT axis (width/2 = 6.095) → outside.
    // The old transposed code checked 14.38 against length/2 (15.345) and wrongly fired here.
    const float px = sx(-14.3922f, 1.62767f);
    const float py = sy(-14.3922f, 1.62767f);
    CHECK(isWithinServerBox(px, py, -17.4379f, CX, CY, CZ, YAW, HX, HY, HZ) == false);
}

TEST_CASE("area-trigger box: standing on the trigger is INSIDE the server box", "[areatrigger][box]") {
    // Player canonical (0.65, 3.14, -13.16) near the trigger → server (3.14, 0.65, -13.16):
    // 0.56u off long axis, 0.66u off short axis, 0.21u in Z → inside on all three.
    const float px = sx(0.65f, 3.14f);
    const float py = sy(0.65f, 3.14f);
    CHECK(isWithinServerBox(px, py, -13.16f, CX, CY, CZ, YAW, HX, HY, HZ) == true);
}

TEST_CASE("area-trigger box: the long axis genuinely extends to ~length/2", "[areatrigger][box]") {
    // A point 14u along the SERVER-X (long) axis but centered on Y is inside (14 < 15.345)…
    CHECK(isWithinServerBox(CX + 14.0f, CY, CZ, CX, CY, CZ, YAW, HX, HY, HZ) == true);
    // …and the same 14u along SERVER-Y (short) axis is outside (14 > 6.095). This asymmetry is
    // exactly what the transposition got backwards.
    CHECK(isWithinServerBox(CX, CY + 14.0f, CZ, CX, CY, CZ, YAW, HX, HY, HZ) == false);
}

TEST_CASE("area-trigger box: rotated box respects yaw", "[areatrigger][box]") {
    // A 20x4 box rotated 90° (π/2): its long axis now runs along server-Y. A point 8u along
    // server-Y must be inside (long axis), and 8u along server-X outside (short axis).
    const float hx = 10.0f, hy = 2.0f, hz = 5.0f, yaw = 1.57079632679f;
    CHECK(isWithinServerBox(0.0f, 8.0f, 0.0f, 0.0f, 0.0f, 0.0f, yaw, hx, hy, hz) == true);
    CHECK(isWithinServerBox(8.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, yaw, hx, hy, hz) == false);
}
