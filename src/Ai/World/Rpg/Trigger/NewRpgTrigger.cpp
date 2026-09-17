/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgTriggers.h"
#include "PlayerbotAI.h"

// The free-roaming brain has no business steering a bot that plays alongside a real player: its
// stale goal (a quest spot, a grind camp) would pull the bot away - or, after a summon, teleport it
// straight back there once the old stuck timer fires.
bool NewRpgStatusTrigger::IsActive()
{
    return status == botAI->rpgInfo.GetStatus() && !botAI->HasRealPlayerInGroup();
}
