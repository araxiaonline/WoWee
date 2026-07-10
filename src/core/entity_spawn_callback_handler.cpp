#include "core/entity_spawn_callback_handler.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/game_handler.hpp"

namespace wowee { namespace core {

EntitySpawnCallbackHandler::EntitySpawnCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer& renderer,
    game::GameHandler& gameHandler,
    std::function<bool(uint64_t)> isLocalPlayerGuid)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , gameHandler_(gameHandler)
    , isLocalPlayerGuid_(std::move(isLocalPlayerGuid))
{
}

void EntitySpawnCallbackHandler::setupCallbacks() {
    // Creature spawn callback (online mode) - spawn creature models
    gameHandler_.setCreatureSpawnCallback([this](uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
        // Queue spawns to avoid hanging when many creatures appear at once.
        // Deduplicate so repeated updates don't flood pending queue.
        if (entitySpawner_.isCreatureSpawned(guid)) return;
        if (entitySpawner_.isCreaturePending(guid)) return;
        entitySpawner_.queueCreatureSpawn(guid, displayId, x, y, z, orientation, scale);
    });

    // Player spawn callback (online mode) - spawn player models with correct textures
    gameHandler_.setPlayerSpawnCallback([this](uint64_t guid,
                                              uint32_t /*displayId*/,
                                              uint8_t raceId,
                                              uint8_t genderId,
                                              uint32_t appearanceBytes,
                                              uint8_t facialFeatures,
                                              float x, float y, float z, float orientation) {
        LOG_DEBUG("playerSpawnCallback: guid=0x", std::hex, guid, std::dec,
                    " race=", static_cast<int>(raceId), " gender=", static_cast<int>(genderId),
                    " pos=(", x, ",", y, ",", z, ")");
        // Skip local player — already spawned as the main character
        if (isLocalPlayerGuid_(guid)) return;
        if (entitySpawner_.isPlayerSpawned(guid)) return;
        if (entitySpawner_.isPlayerPending(guid)) return;
        entitySpawner_.queuePlayerSpawn(guid, raceId, genderId, appearanceBytes, facialFeatures, x, y, z, orientation);
    });

    // Online player equipment callback - apply armor geosets/skin overlays per player instance.
    gameHandler_.setPlayerEquipmentCallback([this](uint64_t guid,
                                                  const std::array<uint32_t, 19>& displayInfoIds,
                                                  const std::array<uint8_t, 19>& inventoryTypes) {
        // Queue equipment compositing instead of doing it immediately —
        // compositeWithRegions is expensive (file I/O + CPU blit + GPU upload)
        // and causes frame stutters if multiple players update at once.
        entitySpawner_.queuePlayerEquipment(guid, displayInfoIds, inventoryTypes);
    });

    // Creature despawn callback (online mode) - remove creature models
    gameHandler_.setCreatureDespawnCallback([this](uint64_t guid) {
        entitySpawner_.despawnCreature(guid);
    });

    gameHandler_.setPlayerDespawnCallback([this](uint64_t guid) {
        entitySpawner_.despawnPlayer(guid);
    });

    // GameObject spawn callback (online mode) - spawn static models (mailboxes, etc.)
    gameHandler_.setGameObjectSpawnCallback([this](uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
        entitySpawner_.queueGameObjectSpawn(guid, entry, displayId, x, y, z, orientation, scale);
    });

    // GameObject despawn callback (online mode) - remove static models
    gameHandler_.setGameObjectDespawnCallback([this](uint64_t guid) {
        entitySpawner_.despawnGameObject(guid);
    });

    // GameObject custom animation callback (e.g. chest opening)
    gameHandler_.setGameObjectCustomAnimCallback([this](uint64_t guid, uint32_t animId) {
        auto& goInstances = entitySpawner_.getGameObjectInstances();
        auto it = goInstances.find(guid);
        if (it == goInstances.end()) return;
        auto& info = it->second;
        if (!info.isWmo) {
            if (auto* m2r = renderer_.getM2Renderer()) {
                // Play the custom animation as a one-shot if model supports it
                if (m2r->hasAnimation(info.instanceId, animId))
                    m2r->setInstanceAnimation(info.instanceId, animId, false);
                else
                    m2r->setInstanceAnimationFrozen(info.instanceId, false);
            }
        }
    });

    // GameObject state change callback — animate doors/chests opening/closing/destroying
    gameHandler_.setGameObjectStateCallback([this](uint64_t guid, uint8_t goState) {
        auto& goInstances = entitySpawner_.getGameObjectInstances();
        auto it = goInstances.find(guid);
        if (it == goInstances.end()) return;
        auto& info = it->second;
        if (info.isWmo) return; // WMOs don't have M2 animation sequences
        auto* m2r = renderer_.getM2Renderer();
        if (!m2r) return;
        uint32_t instId = info.instanceId;
        // GO states: 0=READY(closed), 1=OPEN, 2=DESTROYED/ACTIVE
        if (goState == 1) {
            // Opening: play OPEN(148) one-shot, fall back to unfreezing
            if (m2r->hasAnimation(instId, 148))
                m2r->setInstanceAnimation(instId, 148, false);
            else
                m2r->setInstanceAnimationFrozen(instId, false);
        } else if (goState == 2) {
            // Destroyed: play DESTROY(149) one-shot
            if (m2r->hasAnimation(instId, 149))
                m2r->setInstanceAnimation(instId, 149, false);
        } else {
            // Closed: play CLOSE(146) one-shot, else freeze
            if (m2r->hasAnimation(instId, 146))
                m2r->setInstanceAnimation(instId, 146, false);
            else
                m2r->setInstanceAnimationFrozen(instId, true);
        }
    });

    // Creature move callback (online mode) - update creature positions
    gameHandler_.setCreatureMoveCallback([this](uint64_t guid, float x, float y, float z, uint32_t durationMs, bool walk) {
        if (!renderer_.getCharacterRenderer()) return;
        uint32_t instanceId = 0;
        bool isPlayer = false;
        instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId != 0) { isPlayer = true; }
        else {
            instanceId = entitySpawner_.getCreatureInstanceId(guid);
        }
        if (instanceId != 0) {
            glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
            float durationSec = static_cast<float>(durationMs) / 1000.0f;
            renderer_.getCharacterRenderer()->moveInstanceTo(instanceId, renderPos, durationSec);
            // Locomotion animation for the duration of the spline move. Walk vs Run is inferred
            // from the move's speed (WotLK has no walkmode flag — see spline_packet.hpp). WoW M2
            // animation IDs: 4=Walk, 5=Run. Don't override Death (1); the per-frame sync loop
            // returns the unit to Stand when movement stops.
            if (durationMs > 0) {
                // Player animation is managed by the local renderer state machine —
                // don't reset it here or every server movement packet restarts the
                // cycle from frame 0, causing visible stutter.
                if (!isPlayer) {
                    auto* cr = renderer_.getCharacterRenderer();
                    // Some creature models have NO Walk (4) sequence. playAnimation() silently
                    // falls back to sequence[0] for a missing anim, so currentAnimationId never
                    // becomes WALK — the "already playing?" guard below can't suppress it and WALK
                    // gets replayed every move+frame, resetting animationTime to 0 (frozen model).
                    // So only treat the move as a walk if the model actually has a walk sequence.
                    const bool effectiveWalk = walk && cr->hasAnimation(instanceId, rendering::anim::WALK);

                    // Reconcile the two locomotion drivers: application.cpp re-selects Walk/Run
                    // EVERY frame from getCreatureWalkingState() and defaults to Run when the guid
                    // is absent — overriding a one-shot playAnimation here. Spline NPCs never send
                    // the MSG_MOVE WALKING flag that normally feeds that map, so patrols the server
                    // walks were stuck running. Feed the effective walk state into the SAME map
                    // (same set/erase contract as setUnitMoveFlagsCallback) so the per-frame
                    // selector agrees — and never asks a walk-less model for WALK. (Story 002.)
                    auto& walkState = entitySpawner_.getCreatureWalkingState();
                    if (effectiveWalk) walkState[guid] = true;
                    else               walkState.erase(guid);

                    const uint32_t moveAnim = effectiveWalk ? rendering::anim::WALK : rendering::anim::RUN;
                    uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                    bool gotState = cr->getAnimationState(instanceId, curAnimId, curT, curDur);
                    // Start the selected locomotion anim unless it is already playing (avoids
                    // frame-0 restart stutter) or the unit is dead. Comparing against moveAnim
                    // (not a hardcoded RUN) means a walk↔run transition correctly restarts.
                    if (!gotState || (curAnimId != rendering::anim::DEATH && curAnimId != moveAnim)) {
                        cr->playAnimation(instanceId, moveAnim, /*loop=*/true);
                    }
                    entitySpawner_.getCreatureWasMoving()[guid] = true;
                }
            }
        }
    });

    gameHandler_.setGameObjectMoveCallback([this](uint64_t guid, float x, float y, float z, float orientation) {
        auto& goInstMap = entitySpawner_.getGameObjectInstances();
        auto it = goInstMap.find(guid);
        if (it == goInstMap.end()) {
            return;
        }
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        auto& info = it->second;
        if (info.isWmo) {
            if (auto* wr = renderer_.getWMORenderer()) {
                glm::mat4 transform(1.0f);
                transform = glm::translate(transform, renderPos);
                transform = glm::rotate(transform, orientation, glm::vec3(0, 0, 1));
                wr->setInstanceTransform(info.instanceId, transform);
            }
        } else {
            if (auto* mr = renderer_.getM2Renderer()) {
                glm::mat4 transform(1.0f);
                transform = glm::translate(transform, renderPos);
                mr->setInstanceTransform(info.instanceId, transform);
            }
        }
    });
}

}} // namespace wowee::core
