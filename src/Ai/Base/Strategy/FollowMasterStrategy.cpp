/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FollowMasterStrategy.h"

std::vector<NextAction> FollowMasterStrategy::getDefaultActions()
{
    return {
        NextAction("follow", 1.0f)
    };
}

void FollowMasterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Route a far-away follower to the master via flight masters / portal hubs instead of
    // beelining. Higher relevance than the default "follow" action above; when no smart route
    // exists the trigger stays inactive and plain follow is used.
    triggers.push_back(new TriggerNode("follow travel needed", { NextAction("follow travel", 2.0f) }));
}
