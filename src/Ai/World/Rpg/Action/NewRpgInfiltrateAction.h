/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINFILTRATEACTION_H
#define PLAYERBOTS_NEWRPGINFILTRATEACTION_H

#include "FollowTravelAction.h"
#include "NewRpgInfo.h"

class PlayerbotAI;

// A high-level rogue or druid raids a foreign capital, in three phases (NewRpgInfo::Infiltrate::Phase):
//
//   Travel  - to the capital, using the same flight/portal/ferry machinery as grouped-follower travel. Close to
//             the city it dismounts and stealths (Stealth / Prowl) for the final approach, and the raid begins.
//   Raid    - in the city, on foot and stealthed: walks from banker to banker, and whenever an enemy player or a
//             guard is within reach it opens on it and hands the fight to the normal combat engine. When the fight
//             is over the bot stealths again and carries on. The raid goes on until the bot has died as many times
//             as AiPlayerbot.InfiltrateMaxDeaths allows (counted in PlayerbotAI, which calls
//             NewRpgInfo::OnInfiltrateDeath); after the last death the bot goes idle.
//   Recover - after a death: home (its homebind, when it was killed in or near the city), eat and drink until
//             it is well again, then back to Travel for another go.
class NewRpgInfiltrateAction : public FollowTravelAction
{
public:
    NewRpgInfiltrateAction(PlayerbotAI* botAI) : FollowTravelAction(botAI, "new rpg infiltrate") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool Travel(NewRpgInfo::Infiltrate& data);
    bool Raid(NewRpgInfo::Infiltrate& data);
    bool Recover(NewRpgInfo::Infiltrate& data);

    // Casts the class-appropriate stealth (Rogue Stealth; Druid Cat Form then Prowl). Returns true once the bot
    // is actually stealthed; false while a cast/shapeshift is still pending (caller waits a tick).
    bool EnsureStealthed() const;

    // Is the bot in or close to the city it is raiding
    bool InCity(NewRpgInfo::Infiltrate const& data) const;

    // The enemy to open on: a player within reach first, else a guard (else any hostile creature). Null if
    // there is nobody to fight.
    Unit* FindStrikeTarget() const;

    // Opens on `victim` and hands the fight to the combat engine
    void Strike(NewRpgInfo::Infiltrate const& data, Unit* victim);

    // Walks from one banker's stand to the next, pausing at each. Returns false if it cannot find a way at all.
    bool Patrol(NewRpgInfo::Infiltrate& data);
};

#endif
