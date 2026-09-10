/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InvalidTargetValue.h"
#include "AttackersValue.h"
#include "Playerbots.h"
#include "Unit.h"

bool InvalidTargetValue::Calculate()
{
    Unit* target = AI_VALUE(Unit*, qualifier);
    Unit* enemy = AI_VALUE(Unit*, "enemy player target");
    if (target && enemy && target == enemy && target->IsAlive())
        return false;

    if (target && qualifier == "current target")
    {
        // IsCharmed() is deliberately not checked here. Unit::SetCharmedBy() gives the
        // victim the charmer's faction, so a charmed unit that is still friendly is
        // already covered by IsFriendlyTo(bot) below. Dropping every charmed target
        // instead made bots stand idle during encounters whose whole mechanic is
        // damaging the mind-controlled member to break the control - Baroness Anastari
        // in Stratholme being the clearest case.
        return target->GetMapId() != bot->GetMapId() || target->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ||
               target->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) || target->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE_2) ||
               !target->IsVisible() || !target->IsAlive() || target->IsPolymorphed() ||
               target->HasFearAura() || target->HasUnitState(UNIT_STATE_ISOLATED) || target->IsFriendlyTo(bot) ||
               !AttackersValue::IsValidTarget(target, bot);
    }

    return !target;
}
