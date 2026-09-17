/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FollowTravelStateValue.h"

void FollowTravelState::Clear()
{
    // NB: cooldownUntilMs is deliberately preserved - it outlives a single trip.
    phase = FollowTravelPhase::None;
    goal = WorldPosition();
    planGoal = WorldPosition();
    goalIsDungeon = false;
    dungeonInside = WorldPosition();
    flightMasterEntry = 0;
    flightMasterPos = WorldPosition();
    taxiNodes.clear();
    taxiArrival = WorldPosition();
    taxiTakeoffAtMs = 0;
    flightFails = 0;
    flightStallSinceMs = 0;
    flightProbePos = WorldPosition();
    flightProbeMs = 0;
    transportEntry = 0;
    dockPos = WorldPosition();
    landPos = WorldPosition();
    dockWaitSinceMs = 0;
    deckPos = WorldPosition();
    waitPos = WorldPosition();
    waitPosTried = false;
    portalStaging = WorldPosition();
    portalDestMap = 0;
    portalDestPos = WorldPosition();
    swimSinceMs = 0;
    approachPos = WorldPosition();
    approachTries = 0;
    moveFarPos = WorldPosition();
    nearestMoveFarDis = FLT_MAX;
    stuckSinceMs = 0;
    stuckAttempts = 0;
    giveUpAtMs = 0;
    legFails = 0;
    onRoute = false;
    nextStepSearchMs = 0;
    pendingSinceMs = 0;
    nextMountPokeMs = 0;
}

void FollowTravelState::ResetLeg(WorldPosition const& legTarget)
{
    moveFarPos = legTarget;
    nearestMoveFarDis = FLT_MAX;
    swimSinceMs = 0;
    stuckSinceMs = 0;
    stuckAttempts = 0;
    onRoute = false;
    nextStepSearchMs = 0;
}
