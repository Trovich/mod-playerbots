/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGTRAVELTODUNGEONACTION_H
#define PLAYERBOTS_LFGTRAVELTODUNGEONACTION_H

#include "FollowTravelAction.h"
#include "TravelMgr.h"
#include "Trigger.h"

class Player;
class PlayerbotAI;

// When DungeonFinder.SkipTeleport + AiPlayerbot.LfgWalkToDungeon are on and this bot is in an
// LFG group that has a dungeon it has not entered yet: fills `outside` (staging just outside
// the instance portal) and `inside` (the instance-side start point) and returns true.
bool LfgWalkToDungeonTarget(Player* bot, WorldPosition& outside, WorldPosition& inside);

// Convenience predicate. Shared with FollowTravelAction so the two travellers do not fight
// over the shared travel state.
bool LfgWalkToDungeonApplicable(Player* bot);

class LfgTravelToDungeonTrigger : public Trigger
{
public:
    LfgTravelToDungeonTrigger(PlayerbotAI* botAI) : Trigger(botAI, "lfg travel to dungeon needed", 2) {}

    bool IsActive() override;
};

// Walks an LFG bot to the dungeon entrance (reusing FollowTravelAction's flight/portal/road
// stepping) and then through the portal, instead of being teleported in.
class LfgTravelToDungeonAction : public FollowTravelAction
{
public:
    LfgTravelToDungeonAction(PlayerbotAI* botAI) : FollowTravelAction(botAI, "lfg travel to dungeon") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
