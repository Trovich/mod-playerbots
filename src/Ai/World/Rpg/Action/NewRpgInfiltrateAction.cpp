/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgInfiltrateAction.h"

#include "Creature.h"
#include "Event.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"
#include <cmath>

namespace
{
    using Phase = NewRpgInfo::Infiltrate::Phase;

    constexpr float SNEAK_START_DIS = 400.0f;    // switch to dismounted + stealthed once this close to the target
    constexpr float CITY_RADIUS = 160.0f;        // this close to the target the bot is "in the city": fights may begin
    constexpr float STRIKE_PLAYER_DIS = 30.0f;   // an enemy player this close is worth opening on
    constexpr float STRIKE_GUARD_DIS = 20.0f;    // and a guard this close
    constexpr float PATROL_ARRIVE_DIS = 6.0f;    // a patrol spot counts as reached this close
    constexpr uint32 PATROL_PAUSE_MIN_MS = 3000;  // how long it stands, stealthed, at each spot
    constexpr uint32 PATROL_PAUSE_MAX_MS = 9000;
    constexpr uint32 PATROL_STUCK_MS = 60000;    // a spot it has not reached in this long is dropped for another
    constexpr uint8 PATROL_MAX_FAILS = 4;        // spots in a row with no way there before the raid is called off
    constexpr float RAID_MIN_HEALTH_PCT = 60.0f;  // below this the raider eats before it goes on
    constexpr float RECOVER_HEALTH_PCT = 90.0f;  // eaten/drunk up to this much (health, and mana where there is any)
    constexpr uint32 RECOVER_MAX_MS = 120000;    // and never sat around for longer than this
}

bool NewRpgInfiltrateAction::isUseful()
{
    FollowTravelState const& travel = AI_VALUE(FollowTravelState&, "follow travel state");

    // Our own taxi leg / ferry crossing is running: always useful, so the stalled-flight watchdog in
    // StepTravel keeps ticking even if something else changes botAI->rpgInfo out from under us.
    if (bot->IsInFlight() && travel.phase == FollowTravelPhase::InFlight)
        return true;

    if (bot->GetTransport() && (travel.phase == FollowTravelPhase::Aboard || travel.phase == FollowTravelPhase::ToDock))
        return true;

    return std::get_if<NewRpgInfo::Infiltrate>(&botAI->rpgInfo.data) != nullptr;
}

bool NewRpgInfiltrateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::Infiltrate>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    FollowTravelState& travel = AI_VALUE(FollowTravelState&, "follow travel state");

    // Riding a ferry we boarded ourselves, or a taxi: let the shared stepper finish the leg. Same guard order
    // as FollowTravelAction/LfgTravelToDungeonAction so a mid-crossing bot is never abandoned.
    if (bot->GetTransport() &&
        (travel.phase == FollowTravelPhase::Aboard || travel.phase == FollowTravelPhase::ToDock))
    {
        travel.phase = FollowTravelPhase::Aboard;
        return StepTravel(travel, travel.goal);
    }

    if (bot->IsInFlight())
    {
        if (travel.phase != FollowTravelPhase::InFlight)
        {
            travel.phase = FollowTravelPhase::InFlight;
            travel.flightStallSinceMs = 0;
            travel.flightProbeMs = 0;
        }
        return StepTravel(travel, travel.goal);
    }

    // dead: the dead engine has the bot, and NewRpgInfo::OnInfiltrateDeath has already booked the death
    if (!bot->IsAlive())
        return false;

    switch (data.phase)
    {
        case Phase::Recover:
            return Recover(data);
        case Phase::Raid:
            return Raid(data);
        case Phase::Travel:
        default:
            return Travel(data);
    }
}

bool NewRpgInfiltrateAction::Travel(NewRpgInfo::Infiltrate& data)
{
    FollowTravelState& travel = AI_VALUE(FollowTravelState&, "follow travel state");

    float const dist = bot->GetMapId() == data.target.GetMapId()
                            ? bot->GetExactDist2d(data.target.GetPositionX(), data.target.GetPositionY())
                            : FLT_MAX;

    // Close enough to go the rest of the way on foot, unseen.
    if (dist < SNEAK_START_DIS)
    {
        travel.Clear();
        data.phase = Phase::Raid;
        data.patrol = WorldPosition();
        return Raid(data);
    }

    if (!StepTravel(travel, data.target))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} gave up travelling to infiltrate zone {}", bot->GetName(), data.zoneId);
        travel.Clear();  // the abandoned trip's phase would otherwise keep the bot "travelling" (no grinding, no flee)
        botAI->rpgInfo.ChangeToIdle();
        return false;
    }

    // Keep the bot mounted for the road - the normal mount check only mirrors a nearby master, which this
    // masterless trip has none of.
    if (!bot->IsMounted() && !bot->IsInFlight() && !bot->IsInCombat() && !bot->GetTransport() &&
        travel.phase != FollowTravelPhase::InFlight && travel.phase != FollowTravelPhase::Aboard)
        botAI->DoSpecificAction("check mount state", Event(), true);

    return true;
}

bool NewRpgInfiltrateAction::EnsureStealthed() const
{
    if (bot->getClass() == CLASS_DRUID)
    {
        // Prowl requires Cat Form.
        if (!botAI->HasAura("cat form", bot))
        {
            botAI->DoSpecificAction("cat form", Event(), true);
            return false;
        }
        if (!botAI->HasAura("prowl", bot))
        {
            botAI->DoSpecificAction("prowl", Event(), true);
            return false;
        }
        return true;
    }

    if (bot->getClass() == CLASS_ROGUE)
    {
        if (!botAI->HasAura("stealth", bot))
        {
            botAI->DoSpecificAction("stealth", Event(), true);
            return false;
        }
        return true;
    }

    return true;  // should never happen - CheckRpgStatusAvailable already gates the class
}

bool NewRpgInfiltrateAction::InCity(NewRpgInfo::Infiltrate const& data) const
{
    return bot->GetMapId() == data.target.GetMapId() &&
           bot->GetExactDist2d(data.target.GetPositionX(), data.target.GetPositionY()) < CITY_RADIUS;
}

bool NewRpgInfiltrateAction::Raid(NewRpgInfo::Infiltrate& data)
{
    if (bot->IsInCombat())
    {
        // Spotted, or already struck - not ours to manage, the combat engine (and the bot's own flee-at-low-health
        // strategy) has it. The raid picks up again when the fight is over, or ends with the bot's death.
        return false;
    }

    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);

    if (!EnsureStealthed())
        return true;  // waiting on a cast / GCD / shapeshift

    // Hurt from the last fight: it does not go looking for the next one until it has eaten, unseen where it stands.
    // Once it has sat down to eat it keeps at it up to the recovery mark instead of getting up at the first bite.
    bool sittingToEat = (bot->getStandState() == UNIT_STAND_STATE_SIT);
    if (bot->GetHealthPct() < (sittingToEat ? RECOVER_HEALTH_PCT : RAID_MIN_HEALTH_PCT))
    {
        botAI->DoSpecificAction("food", Event(), true);
        return true;
    }

    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // Nobody is fought outside the city: on the way in, the guards of the outskirts and whoever passes are left be.
    if (InCity(data))
    {
        if (Unit* victim = FindStrikeTarget())
        {
            Strike(data, victim);
            return true;
        }
    }

    return Patrol(data);
}

Unit* NewRpgInfiltrateAction::FindStrikeTarget() const
{
    // Enemy players first: the nearest one that can be attacked and seen
    Player* playerVictim = nullptr;
    float best = STRIKE_PLAYER_DIS;
    for (auto& ref : bot->GetMap()->GetPlayers())
    {
        Player* player = ref.GetSource();
        if (!player || player == bot || !player->IsInWorld() || !player->IsAlive() || player->IsGameMaster() ||
            !bot->IsValidAttackTarget(player) || !bot->IsWithinLOSInMap(player))
            continue;

        float const dist = bot->GetExactDist(player);
        if (dist < best)
        {
            best = dist;
            playerVictim = player;
        }
    }

    if (playerVictim)
        return playerVictim;

    // then the guards, and failing that anything hostile
    Unit* guard = nullptr;
    Unit* other = nullptr;
    float bestGuard = STRIKE_GUARD_DIS;
    float bestOther = STRIKE_GUARD_DIS;
    GuidVector const hostiles = AI_VALUE(GuidVector, "nearest hostile npcs");
    for (ObjectGuid const& guid : hostiles)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !bot->IsValidAttackTarget(unit) || !bot->IsWithinLOSInMap(unit))
            continue;

        float const dist = bot->GetExactDist2d(unit);
        Creature* creature = unit->ToCreature();
        if (creature && creature->IsGuard())
        {
            if (dist < bestGuard)
            {
                bestGuard = dist;
                guard = unit;
            }
        }
        else if (dist < bestOther)
        {
            bestOther = dist;
            other = unit;
        }
    }

    return guard ? guard : other;
}

void NewRpgInfiltrateAction::Strike(NewRpgInfo::Infiltrate const& data, Unit* victim)
{
    LOG_INFO("playerbots", "[New RPG] {} strikes {} in zone {} (death {} of {})", bot->GetName(), victim->GetName(),
             data.zoneId, uint32(data.deaths), sPlayerbotAIConfig.infiltrateMaxDeaths);

    // Same handoff AttackAction::Attack() does for every other opener: hand the target to the shared value so
    // every combat strategy orients on it, then land the opening hit. The combat engine owns the fight from here.
    context->GetValue<Unit*>("current target")->Set(victim);
    bot->SetSelection(victim->GetGUID());
    bot->Attack(victim, bot->IsWithinMeleeRange(victim) || PlayerbotAI::IsMelee(bot));
}

bool NewRpgInfiltrateAction::Patrol(NewRpgInfo::Infiltrate& data)
{
    FollowTravelState& travel = AI_VALUE(FollowTravelState&, "follow travel state");
    uint32 const now = getMSTime();

    // standing still, stealthed, at the last spot
    if (data.patrolPauseUntilMs && now < data.patrolPauseUntilMs)
        return true;

    data.patrolPauseUntilMs = 0;

    bool const hasSpot = data.patrol != WorldPosition();
    bool const reached = hasSpot && bot->GetMapId() == data.patrol.GetMapId() &&
                         bot->GetExactDist2d(data.patrol.GetPositionX(), data.patrol.GetPositionY()) < PATROL_ARRIVE_DIS;
    if (reached)
    {
        // now the next one, after a pause
        data.patrol = WorldPosition();
        data.patrolFails = 0;
        data.patrolPauseUntilMs = now + urand(PATROL_PAUSE_MIN_MS, PATROL_PAUSE_MAX_MS);
        travel.Clear();
        return true;
    }

    if (!hasSpot || now - data.patrolSinceMs > PATROL_STUCK_MS)
    {
        WorldPosition spot;
        // the first spot is the one it was sent to; after that any banker of the city
        if (!hasSpot && data.patrolSinceMs == 0)
            spot = data.target;
        else if (!sTravelMgr.GetCapitalPatrolPoint(data.zoneId, spot))
            spot = data.target;

        data.patrol = spot;
        data.patrolSinceMs = now;
        travel.Clear();
    }

    if (!TravelFarTo(travel, data.patrol))
    {
        if (++data.patrolFails >= PATROL_MAX_FAILS)
        {
            LOG_DEBUG("playerbots", "[New RPG] {} could not find a way around zone {} - the raid is off",
                      bot->GetName(), data.zoneId);
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }

        // try another spot next tick
        data.patrol = WorldPosition();
        data.patrolSinceMs = now;
        travel.Clear();
        return true;
    }

    return true;
}

bool NewRpgInfiltrateAction::Recover(NewRpgInfo::Infiltrate& data)
{
    FollowTravelState& travel = AI_VALUE(FollowTravelState&, "follow travel state");
    uint32 const now = getMSTime();

    if (!data.recoverSinceMs)
    {
        data.recoverSinceMs = now;
        travel.Clear();

        // killed in or near the enemy city: nowhere to eat there, so it goes home to do it (unless home is there too)
        bool const homeIsThere =
            bot->m_homebindMapId == data.target.GetMapId() &&
            std::hypot(bot->m_homebindX - data.target.GetPositionX(), bot->m_homebindY - data.target.GetPositionY()) <
                2.0f * CITY_RADIUS;
        if (InCity(data) && !homeIsThere)
        {
            LOG_DEBUG("playerbots", "[New RPG] {} died in zone {} - home to recover (death {})", bot->GetName(),
                      data.zoneId, uint32(data.deaths));
            bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ,
                            bot->GetOrientation());
            return true;
        }
    }

    if (bot->IsBeingTeleported())
        return true;

    if (bot->IsInCombat())
        return false;  // the combat engine has it

    bool const needFood = bot->GetHealthPct() < RECOVER_HEALTH_PCT;
    bool const needDrink = bot->GetMaxPower(POWER_MANA) > 0 &&
                           bot->GetPower(POWER_MANA) * 100.0f < bot->GetMaxPower(POWER_MANA) * RECOVER_HEALTH_PCT;
    if ((needFood || needDrink) && now - data.recoverSinceMs < RECOVER_MAX_MS)
    {
        // eat and drink what it has; with nothing to eat it just sits and lets time do it
        bool ate = false;
        if (needFood)
            ate = botAI->DoSpecificAction("food", Event(), true);
        if (needDrink)
            ate = botAI->DoSpecificAction("drink", Event(), true) || ate;

        if (!ate && bot->getStandState() == UNIT_STAND_STATE_STAND && !bot->isMoving())
            bot->SetStandState(UNIT_STAND_STATE_SIT);

        return true;
    }

    // well again: back to the city
    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    LOG_DEBUG("playerbots", "[New RPG] {} recovered - back to zone {} (death {})", bot->GetName(), data.zoneId,
              uint32(data.deaths));
    data.phase = Phase::Travel;
    data.recoverSinceMs = 0;
    travel.Clear();
    return true;
}
