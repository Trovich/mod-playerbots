/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ENEMYPLAYERVALUE_H
#define PLAYERBOTS_ENEMYPLAYERVALUE_H

#include "PlayerbotAIConfig.h"
#include "PossibleTargetsValue.h"
#include "TargetValue.h"

class Player;
class PlayerbotAI;
class Unit;

class NearestEnemyPlayersValue : public PossibleTargetsValue
{
public:
    NearestEnemyPlayersValue(PlayerbotAI* botAI, float range = sPlayerbotAIConfig.grindDistance)
        : PossibleTargetsValue(botAI, "nearest enemy players", range)
    {
    }

public:
    bool AcceptUnit(Unit* unit) override;
};

class EnemyPlayerValue : public UnitCalculatedValue
{
public:
    EnemyPlayerValue(PlayerbotAI* botAI, std::string const name = "enemy player")
        : UnitCalculatedValue(botAI, name, 1 * 1000)
    {
    }

    Unit* Calculate() override;

private:
    Unit* SelectTarget();
    // Team play: goes for whoever is healing the enemy being fought, or else the enemy casters, instead of
    // `fallback` (what SelectTarget() found)
    Unit* PreferSupportTarget(Unit* fallback);
    bool IsHealing(Player* healer, Unit* target);
    float GetMaxAttackDistance();

    // last support pick, held for a few seconds so that the bots do not flip between two targets
    ObjectGuid supportGuid;
    uint32 supportUntilMs = 0;
};

#endif
