/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ServerFacade.h"
#include "Player.h"
#include "Playerbots.h"
#include "TargetedMovementGenerator.h"
#include "Timer.h"
#include <cmath>

float ServerFacade::GetDistance2d(Unit* unit, WorldObject* wo)
{
    ASSERT_NOTNULL(unit);
    ASSERT_NOTNULL(wo);

    float dist = unit->GetDistance2d(wo);
    return std::round(dist * 10.0f) / 10.0f;
}

float ServerFacade::GetDistance2d(Unit* unit, float x, float y)
{
    float dist = unit->GetDistance2d(x, y);
    return std::round(dist * 10.0f) / 10.0f;
}

bool ServerFacade::IsDistanceLessThan(float dist1, float dist2)
{
    // return dist1 - dist2 < sPlayerbotAIConfig.targetPosRecalcDistance;
    return dist1 < dist2;
}

bool ServerFacade::IsDistanceGreaterThan(float dist1, float dist2)
{
    // return dist1 - dist2 > sPlayerbotAIConfig.targetPosRecalcDistance;
    return dist1 > dist2;
}

bool ServerFacade::IsDistanceGreaterOrEqualThan(float dist1, float dist2) { return !IsDistanceLessThan(dist1, dist2); }

bool ServerFacade::IsDistanceLessOrEqualThan(float dist1, float dist2) { return !IsDistanceGreaterThan(dist1, dist2); }

void ServerFacade::SetFacingTo(Player* bot, WorldObject* wo, bool force)
{
    if (!bot || !wo)
        return;

    float angle = bot->GetAngle(wo);

    // PvP turn-rate simulation: real players cannot pivot instantly, so when the bot is turning
    // towards a hostile player, rotate at a capped angular speed instead of snapping. This lets
    // melee (rogues especially) actually work an opponent's back. Disabled (instant, legacy
    // behaviour) when PvpTurnSpeed <= 0 or when the caller forces the facing.
    if (!force && sPlayerbotAIConfig.pvpTurnSpeed > 0.0f)
    {
        Unit* target = wo->ToUnit();
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI && target && target->IsPlayer() && bot->IsValidAttackTarget(target))
        {
            uint32 dtMs = botAI->lastPvpFacingMs ? GetMSTimeDiffToNow(botAI->lastPvpFacingMs) : 200;
            botAI->lastPvpFacingMs = getMSTime();

            // Clamp so a long gap between updates cannot buy a near-instant spin
            if (dtMs > 500)
                dtMs = 500;

            float maxStep = sPlayerbotAIConfig.pvpTurnSpeed * (static_cast<float>(dtMs) / 1000.0f);
            float diff = Position::NormalizeOrientation(angle - bot->GetOrientation());
            if (diff > static_cast<float>(M_PI))
                diff -= 2.0f * static_cast<float>(M_PI);

            if (std::fabs(diff) > maxStep)
                angle = Position::NormalizeOrientation(bot->GetOrientation() + (diff > 0.0f ? maxStep : -maxStep));
        }
    }

    bot->SetOrientation(angle);

    if (!bot->IsRooted())
        // enforce (bool self) true otherwhise when using real-client with self-bot wont
        // recieve update; e.g. will not face the target when using (mostly ranged) attack
        bot->SendMovementFlagUpdate(true);
}

Unit* ServerFacade::GetChaseTarget(Unit* target)
{
    MovementGenerator* movementGen = target->GetMotionMaster()->top();
    if (movementGen && movementGen->GetMovementGeneratorType() == CHASE_MOTION_TYPE)
    {
        if (target->IsPlayer())
        {
            return static_cast<ChaseMovementGenerator<Player> const*>(movementGen)->GetTarget();
        }

        return static_cast<ChaseMovementGenerator<Creature> const*>(movementGen)->GetTarget();
    }

    return nullptr;
}

void ServerFacade::SendPacket(Player* player, WorldPacket* packet)
{
    player->GetSession()->SendPacket(packet);
}
