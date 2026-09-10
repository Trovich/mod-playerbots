/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOLLOWTRAVELACTION_H
#define PLAYERBOTS_FOLLOWTRAVELACTION_H

#include "FollowTravelStateValue.h"
#include "MovementActions.h"
#include "Trigger.h"

class Player;
class PlayerbotAI;

// The human this bot should travel toward: its master, or the group leader (what plain
// "follow" uses) when no master is set. Only a real player or a self-bot counts; returns
// null for a bot-led group. Shared with LfgTravelToDungeonAction so the two travellers
// don't both drive the shared travel state.
Player* FollowTravelAnchor(PlayerbotAI* botAI, Player* bot);

// True if a mid-trip bot should keep running through the combat it just picked up (trash
// leashes once outrun) rather than stop and fight. Gated by AiPlayerbot.SmartTravelRunPastEnemies
// and a minimum health %, and always false against players / elites / bosses.
bool TravelRunsThroughCombat(PlayerbotAI* botAI, Player* bot);

// Fires while a follower is far enough from its master that a plain navmesh follow would
// beeline through the world. Handled by "follow travel".
class FollowTravelTrigger : public Trigger
{
public:
    FollowTravelTrigger(PlayerbotAI* botAI) : Trigger(botAI, "follow travel needed", 2) {}

    bool IsActive() override;
};

// Routes a far-away follower to its master the sensible way: walk to the nearest flight
// master, fly toward the master, optionally jump an inter-city portal hub, then close the
// last stretch on real paths. Shared by LfgTravelToDungeonAction through TravelFarTo /
// StepTravel, which target an arbitrary WorldPosition instead of the master.
class FollowTravelAction : public MovementAction
{
public:
    FollowTravelAction(PlayerbotAI* botAI, std::string const name = "follow travel") : MovementAction(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;

protected:
    // Drive one leg of a trip toward `goal`. Returns true while still travelling, false once
    // the caller should hand control back to plain follow (arrived, or gave up). Populates and
    // consumes `st` (a FollowTravelState value).
    bool StepTravel(FollowTravelState& st, WorldPosition const& goal);

    // Long-distance "walk toward" that follows real paths (PathGenerator endpoint + forward
    // cone sampling) instead of a straight spline, with stuck detection. Adapted from
    // NewRpgBaseAction::MoveFarTo, minus its teleport recovery. Returns false when genuinely
    // stuck so the caller can abort the trip.
    bool TravelFarTo(FollowTravelState& st, WorldPosition const& dest);
};

#endif
