/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOLLOWTRAVELSTATEVALUE_H
#define PLAYERBOTS_FOLLOWTRAVELSTATEVALUE_H

#include <cfloat>
#include <cstdint>
#include <vector>

#include "TravelMgr.h"
#include "Value.h"

class PlayerbotAI;

enum class FollowTravelPhase : uint8_t
{
    None = 0,        // no trip in progress; the action will (re)plan on the next tick
    ToFlightMaster,  // walking to the flight master that starts the taxi leg
    InFlight,        // riding the taxi; the action just waits it out
    ToPortal,        // walking to an inter-city portal hub, then jumping through it
    FinalApproach,   // last leg on the destination continent, navmesh only
    ToDock,          // walking to a ferry dock, then waiting there for the boat
    Aboard,          // riding a boat / zeppelin; the core carries us across the map boundary
};

// Per-bot scratch state for FollowTravelAction / LfgTravelToDungeonAction. Set manually by
// the action; mirrors how LastMovement is carried as a ManualSetValue.
class FollowTravelState
{
public:
    void Clear();

    // Reset the stuck tracking used by FollowTravelAction::TravelFarTo when a new leg target
    // is committed (adapted from NewRpgBaseAction::SetMoveFarTo).
    void ResetLeg(WorldPosition const& legTarget);

    FollowTravelPhase phase = FollowTravelPhase::None;

    // Snapshot of what we are travelling toward (master position, or the dungeon entrance).
    WorldPosition goal;

    // Where the goal was when the current plan was made: a master who has since moved to another region, or far
    // enough away, invalidates the legs we are walking.
    WorldPosition planGoal;

    // Set when `goal` is an LFG dungeon entrance; `dungeonInside` is the instance-side start.
    bool goalIsDungeon = false;
    WorldPosition dungeonInside;

    // Flight leg.
    uint32 flightMasterEntry = 0;
    WorldPosition flightMasterPos;
    std::vector<uint32> taxiNodes;
    WorldPosition taxiArrival;    // intended endpoint of the taxi leg (for stuck-flight recovery)
    uint32 taxiTakeoffAtMs = 0;  // staggered take-off time; 0 = not scheduled yet
    uint32 flightFails = 0;      // consecutive ActivateTaxiPathTo failures this trip
    uint32 flightStallSinceMs = 0;  // in-flight but making no progress since this time; 0 = healthy
    WorldPosition flightProbePos;   // bot position at the last in-flight progress probe
    uint32 flightProbeMs = 0;       // when that probe was taken; 0 = not probing yet

    // Set when a trip is abandoned (e.g. taxi kept failing); the trigger stays quiet until
    // then so the bot doesn't loop back to the flight master forever. Survives Clear().
    uint32 cooldownUntilMs = 0;

    // Throttle for the in-travel "get mounted" nudge.
    uint32 nextMountPokeMs = 0;

    // Ferry leg (boat / zeppelin).
    uint32 transportEntry = 0;   // gameobject entry of the transport we intend to ride
    WorldPosition dockPos;       // where it docks on our map (board here)
    WorldPosition landPos;       // where it docks on the goal's map (get off here)
    uint32 dockWaitSinceMs = 0;  // when we started waiting at the dock; 0 = not waiting yet
    WorldPosition deckPos;       // deck spot we hopped to; attach as passenger once the hop lands
    WorldPosition waitPos;       // this bot's own spot on the pier, so waiting bots do not stack
    bool waitPosTried = false;   // looked for one already (the search is a few hundred height probes)

    // Portal leg.
    WorldPosition portalStaging;
    uint32 portalDestMap = 0;
    WorldPosition portalDestPos;

    // Continuous-swim tracking: a stepping stone across water looks walkable to the navmesh, which is how a bot
    // ends up crossing a whole lake - or an ocean - under the terrain.
    uint32 swimSinceMs = 0;

    // Final-approach dead end: the closest point we found that is actually walkable, and how many
    // times we re-aimed at it. Teleporting the remainder is only allowed from there.
    WorldPosition approachPos;
    uint8 approachTries = 0;

    // Stuck / give-up tracking.
    WorldPosition moveFarPos;
    float nearestMoveFarDis = FLT_MAX;
    uint32 stuckSinceMs = 0;
    uint32 stuckAttempts = 0;
    uint32 giveUpAtMs = 0;

    // Walking legs of this plan that stalled; the next stall hops the bot to the leg's travel point.
    uint8 legFails = 0;

    // TravelFarTo is following a complete navmesh route (which may lead away for a while) rather than a guess.
    bool onRoute = false;

    // No stepping stone found last tick: the next search (a couple of dozen path queries) waits until then.
    uint32 nextStepSearchMs = 0;

    // The human we travel toward is loading into a new place since then; 0 = not waiting.
    uint32 pendingSinceMs = 0;
};

class FollowTravelStateValue : public ManualSetValue<FollowTravelState&>
{
public:
    FollowTravelStateValue(PlayerbotAI* botAI) : ManualSetValue<FollowTravelState&>(botAI, data) {}

private:
    FollowTravelState data;
};

#endif
