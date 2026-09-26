/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LASTMOVEMENTVALUE_H
#define PLAYERBOTS_LASTMOVEMENTVALUE_H

#include "ObjectGuid.h"
#include "TravelNode.h"
#include "Value.h"

class PlayerbotAI;
class Unit;

// A flee state whose action has not run for this long belongs to a fight that is already over
constexpr uint32 FLEE_STATE_STALE_MS = 3000;

// High priority movement can override the previous low priority one
enum class MovementPriority
{
    MOVEMENT_IDLE,
    MOVEMENT_WANDER,
    MOVEMENT_NORMAL,
    MOVEMENT_COMBAT,
    MOVEMENT_FORCED
};

class LastMovement
{
public:
    LastMovement();
    LastMovement(LastMovement& other);

    LastMovement& operator=(LastMovement const& other)
    {
        taxiNodes = other.taxiNodes;
        taxiMaster = other.taxiMaster;
        lastFollow = other.lastFollow;
        lastAreaTrigger = other.lastAreaTrigger;
        lastMoveShort = other.lastMoveShort;
        lastPath = other.lastPath;
        nextTeleport = other.nextTeleport;
        priority = other.priority;
        return *this;
    };

    void clear();

    void EndFlee()
    {
        fleeActive = false;
        fleeStartMs = 0;
        fleeLastMs = 0;
        fleeFromGuid.Clear();
    }

    void Set(Unit* follow);
    void Set(uint32 mapId, float x, float y, float z, float ori, float delayTime, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    void setShort(WorldPosition point);
    void setPath(TravelPath path);

    std::vector<uint32> taxiNodes;
    ObjectGuid taxiMaster;
    Unit* lastFollow;
    uint32 lastAreaTrigger;
    time_t lastFlee;
    // Sticky flee state (FleeAction): once a flee starts it holds until the pursuit is over, instead of
    // being re-decided every tick. Default member initialisers so the copy constructor stays valid.
    bool fleeActive = false;
    uint32 fleeStartMs = 0;
    uint32 fleeLastMs = 0;  // last tick the flee action ran; a long gap means the fight ended and the state is stale
    ObjectGuid fleeFromGuid;
    uint32 lastMoveToMapId;
    float lastMoveToX;
    float lastMoveToY;
    float lastMoveToZ;
    float lastMoveToOri;
    float lastdelayTime;
    WorldPosition lastMoveShort;
    uint32 msTime;
    MovementPriority priority;
    TravelPath lastPath;
    time_t nextTeleport;
    std::future<TravelPath> future;
};

class LastMovementValue : public ManualSetValue<LastMovement&>
{
public:
    LastMovementValue(PlayerbotAI* botAI) : ManualSetValue<LastMovement&>(botAI, data) {}

private:
    LastMovement data = LastMovement();
};

class StayTimeValue : public ManualSetValue<time_t>
{
public:
    StayTimeValue(PlayerbotAI* botAI) : ManualSetValue<time_t>(botAI, 0) {}
};

#endif
