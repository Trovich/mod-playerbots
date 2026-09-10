/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatStrategy.h"
#include "Strategy.h"

void CombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "enemy out of spell",
            {
                NextAction("reach spell", ACTION_HIGH)
            }
        )
    );
    // drop target relevance 99 (lower than Worldpacket triggers)
    triggers.push_back(
        new TriggerNode(
            "invalid target",
            {
                NextAction("drop target", 99)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "mounted",
            {
                NextAction("check mount state", 54)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "combat stuck",
            {
                NextAction("reset", 1.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "not facing target",
            {
                NextAction("set facing", ACTION_MOVE + 7)
            }
        )
    );
    // The pet-attack trigger is commented out because it was forcing the bot's pet to attack, overriding stay and follow commands.
    // Pets will automatically attack the bot's enemy if they are in "defensive" or "aggressive"
    // stance, or if the master issues an attack command.
}

AvoidAoeStrategy::AvoidAoeStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> AvoidAoeStrategy::getDefaultActions()
{
    return {
        NextAction("avoid aoe", ACTION_EMERGENCY)
    };
}

void AvoidAoeStrategy::InitTriggers(std::vector<TriggerNode*>& /*triggers*/)
{
}

void AvoidAoeStrategy::InitMultipliers(std::vector<Multiplier*>& /*multipliers*/)
{
}

TankFaceStrategy::TankFaceStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> TankFaceStrategy::getDefaultActions()
{
    return {
        NextAction("tank face", ACTION_MOVE)
    };
}

void TankFaceStrategy::InitTriggers(std::vector<TriggerNode*>& /*triggers*/)
{
}

std::vector<NextAction> CombatFormationStrategy::getDefaultActions()
{
    return {
        NextAction("combat formation move", ACTION_NORMAL)
    };
}

void CombatFormationStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Combat-engine home for the smart-travel triggers: keep a bot that is mid-trip moving
    // toward its goal through trash it aggroed on the way (the trash leashes once outrun)
    // instead of stopping to fight. This strategy is added exactly once per combat engine
    // (AiFactory::AddDefaultCombatStrategies), unlike CombatStrategy::InitTriggers which every
    // class strategy chains into - registering there would evaluate these triggers 2-4x a tick.
    triggers.push_back(
        new TriggerNode("follow travel needed", { NextAction("follow travel", ACTION_MOVE) }));
    triggers.push_back(
        new TriggerNode("lfg travel to dungeon needed", { NextAction("lfg travel to dungeon", ACTION_MOVE) }));
}
