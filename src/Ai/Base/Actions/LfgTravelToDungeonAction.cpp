/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgTravelToDungeonAction.h"

#include "Event.h"
#include "Group.h"
#include "LFGMgr.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Transport.h"
#include "World.h"

namespace
{
    // Resolves the LFG dungeon this bot's group is heading to. `outside` is the staging point
    // just outside the instance portal (where a normal walk-in run exits to); `inside` is the
    // instance-side start position. Returns false when the bot is not in an eligible LFG group
    // or is already on the dungeon map.
    bool ResolveLfgDungeon(Player* bot, WorldPosition& outside, WorldPosition& inside)
    {
        if (!bot)
            return false;

        Group* group = bot->GetGroup();
        if (!group || !group->isLFGGroup())
            return false;

        ObjectGuid const gguid = group->GetGUID();
        if (sLFGMgr->GetState(gguid) != lfg::LFG_STATE_DUNGEON)
            return false;

        uint32 const mapId = sLFGMgr->GetDungeonMapId(gguid);
        if (!mapId || bot->GetMapId() == mapId)
            return false;

        // Staging point just outside the instance portal (where a normal walk-in run exits to).
        AreaTriggerTeleport const* back = sObjectMgr->GetGoBackTrigger(mapId);
        // Instance-side spawn point (where the entrance areatrigger drops players).
        AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(mapId);
        if (!back || !entrance)
            return false;

        outside =
            WorldPosition(back->target_mapId, back->target_X, back->target_Y, back->target_Z, back->target_Orientation);
        inside = WorldPosition(entrance->target_mapId, entrance->target_X, entrance->target_Y, entrance->target_Z,
                               entrance->target_Orientation);
        return true;
    }
}

bool LfgWalkToDungeonTarget(Player* bot, WorldPosition& outside, WorldPosition& inside)
{
    if (!sPlayerbotAIConfig.lfgWalkToDungeon || !sWorld->getBoolConfig(CONFIG_LFG_SKIP_TELEPORT))
        return false;

    return ResolveLfgDungeon(bot, outside, inside);
}

bool LfgWalkToDungeonApplicable(Player* bot)
{
    WorldPosition outside;
    WorldPosition inside;
    return LfgWalkToDungeonTarget(bot, outside, inside);
}

bool LfgTravelToDungeonTrigger::IsActive()
{
    if (!bot->IsAlive())
        return false;

    if ((botAI->GetState() == BOT_STATE_COMBAT || bot->IsInCombat()) && !TravelRunsThroughCombat(botAI, bot))
        return false;

    if (bot->IsBeingTeleported() || bot->GetVehicle())
        return false;

    // Bots whose long trips "follow travel" already drives are handled there - it aims at the dungeon entrance
    // whether or not there is a human to follow. This trigger only covers bots outside its reach (no follow
    // strategy, or told to stay put), so the two never fight over the shared travel state.
    if (FollowTravelCovers(botAI))
        return false;

    // Told to hold position: the same respect "follow travel" pays to "stay".
    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    FollowTravelState& st = AI_VALUE(FollowTravelState&, "follow travel state");

    // Riding a taxi: stay selected only for our own flight leg so StepTravel's InFlight
    // watchdog can force-land a flight whose spline died. Bailing out unconditionally here
    // would leave a stalled bot (IsInFlight() stuck on) with nothing able to rescue it.
    if (bot->IsInFlight())
        return st.phase == FollowTravelPhase::InFlight;

    if (bot->GetTransport())
        return st.phase == FollowTravelPhase::Aboard || st.phase == FollowTravelPhase::ToDock;

    if (st.cooldownUntilMs && getMSTime() < st.cooldownUntilMs)
        return false;

    return LfgWalkToDungeonApplicable(bot);
}

bool LfgTravelToDungeonAction::isUseful()
{
    // Our own taxi leg is running: always useful, so the stalled-flight watchdog keeps ticking.
    FollowTravelState const& st = AI_VALUE(FollowTravelState&, "follow travel state");

    if (bot->IsInFlight() && st.phase == FollowTravelPhase::InFlight)
        return true;

    if (bot->GetTransport() && (st.phase == FollowTravelPhase::Aboard || st.phase == FollowTravelPhase::ToDock))
        return true;

    return !FollowTravelCovers(botAI) && LfgWalkToDungeonApplicable(bot);
}

bool LfgTravelToDungeonAction::Execute(Event /*event*/)
{
    FollowTravelState& st = AI_VALUE(FollowTravelState&, "follow travel state");

    // Riding a ferry we boarded ourselves: wait for the destination dock. Deliberately before
    // the cooldown and goal checks so the Aboard phase can never be lost mid-crossing.
    if (bot->GetTransport() &&
        (st.phase == FollowTravelPhase::Aboard || st.phase == FollowTravelPhase::ToDock))
    {
        st.phase = FollowTravelPhase::Aboard;
        return StepTravel(st, st.goal);
    }

    // Riding a taxi: wait it out and let StepTravel's InFlight watchdog rescue a dead flight.
    if (bot->IsInFlight())
    {
        if (st.phase != FollowTravelPhase::InFlight)
        {
            st.phase = FollowTravelPhase::InFlight;
            st.flightStallSinceMs = 0;
            st.flightProbeMs = 0;
        }
        return StepTravel(st, st.goal);
    }

    if (st.cooldownUntilMs && getMSTime() < st.cooldownUntilMs)
        return false;

    WorldPosition outside;
    WorldPosition inside;
    if (!ResolveLfgDungeon(bot, outside, inside))
    {
        st.Clear();
        return false;
    }

    float const nearDist = bot->GetMapId() == outside.GetMapId()
                               ? bot->GetExactDist2d(outside.GetPositionX(), outside.GetPositionY())
                               : 100000.0f;

    if (nearDist < 60.0f)
    {
        if (nearDist < 20.0f)
        {
            // At the portal - step inside, the same short hop clicking the portal performs.
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            bot->TeleportTo(inside.GetMapId(), inside.GetPositionX(), inside.GetPositionY(), inside.GetPositionZ(),
                            inside.GetOrientation());
            st.Clear();
            return true;
        }

        // Stateless short walk-in, re-selected by the trigger every tick: drop any leftover
        // trip state so a stuck bot is not left with a non-None phase forever.
        st.Clear();
        return MoveTo(outside.GetMapId(), outside.GetPositionX(), outside.GetPositionY(), outside.GetPositionZ(), false,
                      false, false, true);
    }

    st.goalIsDungeon = true;
    st.dungeonInside = inside;

    if (!StepTravel(st, outside))
    {
        bool const arrived = bot->GetMapId() == outside.GetMapId() &&
                             bot->GetExactDist2d(outside.GetPositionX(), outside.GetPositionY()) < 150.0f;
        if (!arrived)
            st.cooldownUntilMs = getMSTime() + 60 * 1000;

        st.Clear();
        return false;
    }

    return true;
}
