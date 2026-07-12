#pragma once

#include <cmath>

// Area-trigger box containment, in SERVER/wire frame, replicating AzerothCore/TrinityCore
// `Position::IsWithinBox` exactly. The client must agree with the server about whether the
// player is inside a box trigger, or it fires CMSG_AREATRIGGER from a spot the server rejects
// as "too far" (see araxia/docs/improvements/007-instance-exit-blocked.md — Story 007).
//
// IMPORTANT: this works in SERVER frame (server.X = canonical.Y, server.Y = canonical.X). The
// bug it fixes was doing the test in canonical frame after swapping the trigger's X/Y but NOT
// its box length/width — transposing the box. Callers must convert the player position to
// server frame first and pass the box's raw DBC length/width/yaw (which are already server-frame).

namespace wowee {
namespace game {

// Half-extents: hx = length/2 (along server-X), hy = width/2 (along server-Y), hz = height/2.
// yaw is the box orientation in server/wire radians. All positions/center in server frame.
// Matches AzerothCore Position::IsWithinBox: rotate the point about the center by (2π - yaw),
// then compare axis-aligned half-extents. Inside is inclusive (server rejects on strict >).
[[nodiscard]] inline bool isWithinServerBox(
    float px, float py, float pz,
    float cx, float cy, float cz,
    float yaw, float hx, float hy, float hz) {
    const double rotation = 2.0 * M_PI - static_cast<double>(yaw);
    const double s = std::sin(rotation);
    const double c = std::cos(rotation);

    const float bdx = px - cx;
    const float bdy = py - cy;

    // Rotate the point around the box center (server rotates the point, not the box).
    const float rotX = cx + static_cast<float>(bdx * c - bdy * s);
    const float rotY = cy + static_cast<float>(bdy * c + bdx * s);

    const float dx = rotX - cx;
    const float dy = rotY - cy;
    const float dz = pz - cz;

    return std::fabs(dx) <= hx && std::fabs(dy) <= hy && std::fabs(dz) <= hz;
}

}  // namespace game
}  // namespace wowee
