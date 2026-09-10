/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FollowTravelAction.h"

#include <algorithm>
#include <cmath>

#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "Group.h"
#include "LastMovementValue.h"
#include "LfgTravelToDungeonAction.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectDefines.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Position.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelMgr.h"

namespace
{
    constexpr uint32 GIVE_UP_MS = 300 * 1000;      // per-plan safety budget, refreshed on each (re)plan
    constexpr uint32 LEG_STUCK_MS = 25 * 1000;     // no progress on a single leg for this long => abort
    constexpr uint32 TAXI_FAIL_COOLDOWN_MS = 60 * 1000;  // pause smart travel after repeated taxi failures
    constexpr uint32 MAX_SWIM_MS = 20 * 1000;  // longer than any legitimate crossing on a travel leg
    constexpr float DOCK_ARRIVE_DIS = 25.0f;   // "standing at the dock", and "close enough to step off"
    constexpr float DOCK_BOARD_DIS = 80.0f;    // bot-to-hull distance that counts as "berthed here"
    constexpr float DOCK_LEAVE_DIS = 120.0f;   // bot-to-dock distance that counts as "we have arrived"
    constexpr uint32 FLIGHT_PROBE_MS = 3000;       // how often to sample in-flight progress
    constexpr uint32 FLIGHT_STALL_MS = 9000;       // no in-flight progress this long => dead taxi
    constexpr float FLIGHT_PROGRESS_DIS = 5.0f;    // yards a live taxi easily covers per probe
    constexpr float PATHFINDER_DIS = 70.0f;        // switch to direct MoveTo within this range (as NewRpg)
    constexpr float ARRIVE_DIS = 60.0f;            // "close enough" to the goal; hand back to plain follow
    constexpr float PORTAL_STEP_DIS = 25.0f;       // how close to the hub centre before we jump the portal
    constexpr float DUNGEON_STEP_DIS = 20.0f;      // how close to the entrance before we step inside
    constexpr float DUNGEON_TAKEOVER_DIS = 150.0f;  // take over from plain follow beyond this to the entrance

    bool MapIsInstance(Map const* map) { return map && map->Instanceable(); }

    bool Travelable(Player* p)
    {
        return p && p->IsInWorld() && !p->IsBeingTeleported() && !MapIsInstance(p->GetMap());
    }

    bool IsHuman(Player* p) { return p && (IsRealPlayer(p) || IsSelfBot(p)); }

    Player* TravelAnchor(PlayerbotAI* botAI, Player* bot)
    {
        Player* master = botAI->GetMaster();
        if (IsHuman(master) && master != bot && Travelable(master))
            return master;

        Player* leader = botAI->GetGroupLeader();
        if (IsHuman(leader) && leader != bot && Travelable(leader))
            return leader;

        // Master not reconciled yet and the leader is a bot: follow any real player /
        // self-bot in the group.
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                if (m && m != bot && IsHuman(m) && Travelable(m))
                    return m;
            }

        return nullptr;
    }

    // Pick where this bot should travel: the LFG dungeon entrance if the group is walking to
    // one (`toDungeon` true, `dungeonInside` = instance-side start), otherwise the travel
    // anchor's position. Returns false when there is nothing valid to travel to.
    //
    // Requires a human anchor either way. Fully bot-led LFG groups are driven by
    // LfgTravelToDungeonAction instead - that trigger bails when an anchor exists, so the two
    // never fight over the shared FollowTravelState.
    bool ResolveTravelGoal(PlayerbotAI* botAI, Player* bot, WorldPosition& goal, WorldPosition& dungeonInside,
                           bool& toDungeon)
    {
        Player* anchor = TravelAnchor(botAI, bot);
        if (!anchor)
            return false;

        WorldPosition outside;
        if (LfgWalkToDungeonTarget(bot, outside, dungeonInside))
        {
            goal = outside;
            toDungeon = true;
            return true;
        }

        goal = WorldPosition(anchor);
        toDungeon = false;
        return true;
    }

    // Total taxi fare for the planned node chain (consecutive directed TaxiPath costs).
    uint32 TaxiRouteCost(std::vector<uint32> const& nodes)
    {
        uint32 total = 0;
        for (size_t i = 1; i < nodes.size(); ++i)
        {
            uint32 path = 0;
            uint32 cost = 0;
            sObjectMgr->GetTaxiPath(nodes[i - 1], nodes[i], path, cost);
            total += cost;
        }
        return total;
    }

    // FlightPathMovementGenerator::LoadPath silently fails on a hop whose directed TaxiPath is
    // missing or has no usable TaxiPathNode geometry, which leaves the bot flagged in-flight
    // with a dead spline (it "hangs on the taxi" then drops to the ground). Reject such chains
    // before activating so the planner falls back to a portal hop / walk / teleport instead.
    bool TaxiChainFlyable(std::vector<uint32> const& nodes)
    {
        if (nodes.size() < 2)
            return false;

        for (size_t i = 1; i < nodes.size(); ++i)
        {
            uint32 pathId = 0;
            uint32 cost = 0;
            sObjectMgr->GetTaxiPath(nodes[i - 1], nodes[i], pathId, cost);
            if (!pathId || pathId >= sTaxiPathNodesByPath.size())
                return false;

            if (sTaxiPathNodesByPath[pathId].size() < 2)
                return false;
        }
        return true;
    }

    // The transport we mean to ride, if it is currently on the bot's map. A cross-continent
    // ferry lives in exactly one map's container at a time, so a null result usually just means
    // it is still on the far side of its loop.
    Transport* FindTransport(Player* bot, uint32 entry)
    {
        Map* map = bot->GetMap();
        if (!map || !entry)
            return nullptr;

        for (Transport* t : map->GetAllTransports())
            if (t && t->GetEntry() == entry)
                return t;

        return nullptr;
    }

    // True only if `dest` (assumed on the bot's current map) is reachable over the navmesh
    // rather than via a straight-line shortcut through terrain / open water. Long partial
    // paths (PATHFIND_INCOMPLETE) still count - TravelFarTo chains those.
    bool SameMapReachable(Player* bot, WorldPosition const& dest)
    {
        PathGenerator probe(bot);
        probe.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());

        // NOPATH / SHORTCUT / NOT_USING_PATH == "no navmesh route, straight line only" - that is
        // the under-terrain beeline we must not do. A far-from-poly endpoint on an otherwise
        // real path is fine (MoveTo snaps it).
        uint32 const t = probe.GetPathType();
        if (t & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
            return false;

        return (t & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) != 0;
    }

    constexpr float AVOID_BAND = 16.0f;       // hostile this close to the path line is "in the way"
    constexpr float AVOID_LOOKAHEAD = 45.0f;  // ...and this far ahead of the bot
    constexpr float AVOID_SHIFT = 20.0f;      // sidestep this far to the clear side
    constexpr float AVOID_STEP = 24.0f;       // shortened forward step while dodging

    // If an idle hostile NPC sits right on the bot -> (tx,ty) line just ahead, return a
    // waypoint sidestepped to the clearer side (PathGenerator-validated), so the bot rounds
    // the pack instead of running through it. Returns the input unchanged when nothing blocks
    // or no safe detour exists.
    WorldPosition AvoidHostiles(PlayerbotAI* botAI, Player* bot, float tx, float ty, float tz)
    {
        WorldPosition original(bot->GetMapId(), tx, ty, tz);
        if (!sPlayerbotAIConfig.smartTravelAvoidEnemies)
            return original;

        GuidVector hostiles = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest hostile npcs")->Get();
        if (hostiles.empty())
            return original;

        float const bx = bot->GetPositionX();
        float const by = bot->GetPositionY();
        float dx = tx - bx;
        float dy = ty - by;
        float const segLen = std::sqrt(dx * dx + dy * dy);
        if (segLen < 8.0f)
            return original;

        float const ux = dx / segLen;
        float const uy = dy / segLen;
        float const px = -uy;  // left-hand normal
        float const py = ux;

        float sideBias = 0.0f;  // >0 => hostiles mostly on the left, push right
        for (ObjectGuid const& guid : hostiles)
        {
            Unit* h = botAI->GetUnit(guid);
            if (!h || !h->IsAlive() || h->IsInCombat())
                continue;

            float const hx = h->GetPositionX() - bx;
            float const hy = h->GetPositionY() - by;
            float const along = hx * ux + hy * uy;
            if (along < 2.0f || along > std::min(segLen, AVOID_LOOKAHEAD))
                continue;

            float const lateral = hx * px + hy * py;
            if (std::fabs(lateral) < AVOID_BAND)
                sideBias += (lateral >= 0.0f) ? 1.0f : -1.0f;
        }

        if (sideBias == 0.0f)
            return original;

        float const sign = sideBias > 0.0f ? -1.0f : 1.0f;  // steer away from the crowded side
        float const step = std::min(segLen, AVOID_STEP);
        float const nx = bx + ux * step + px * sign * AVOID_SHIFT;
        float const ny = by + uy * step + py * sign * AVOID_SHIFT;

        PathGenerator gen(bot);
        gen.CalculatePath(nx, ny, tz);
        if (gen.GetPathType() & ~(PATHFIND_NORMAL | PATHFIND_INCOMPLETE))
            return original;  // detour not walkable - keep the straight line

        G3D::Vector3 const& end = gen.GetActualEndPosition();
        return WorldPosition(bot->GetMapId(), end.x, end.y, end.z);
    }
}

Player* FollowTravelAnchor(PlayerbotAI* botAI, Player* bot) { return TravelAnchor(botAI, bot); }

// While a trip is in progress, decide whether the bot should keep running through combat it
// picked up in transit (trash leashes once outrun) rather than stop and fight.
bool TravelRunsThroughCombat(PlayerbotAI* botAI, Player* bot)
{
    if (!sPlayerbotAIConfig.smartTravelRunPastEnemies)
        return false;

    if (bot->GetHealthPct() < static_cast<float>(sPlayerbotAIConfig.smartTravelCombatMinHealth))
        return false;

    // Can't move anyway -> let the combat AI handle it.
    if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING | UNIT_STATE_ROOT) ||
        bot->isFrozen())
        return false;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* attacker = botAI->GetUnit(guid);
        if (!attacker || !attacker->IsAlive())
            continue;

        if (attacker->IsPlayer())
            return false;  // PvP - fight or flee properly

        if (Creature* c = attacker->ToCreature())
            if (c->isElite() || c->isWorldBoss())
                return false;  // don't try to outrun an elite / boss
    }

    return true;
}

bool FollowTravelTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.groupSmartTravel)
        return false;

    if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
        return false;

    // Respect an explicit "stay" - the bot was told to hold position, not chase the master.
    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    bool const inCombat = botAI->GetState() == BOT_STATE_COMBAT || bot->IsInCombat();

    if (!bot->IsAlive() || bot->IsBeingTeleported() || bot->GetVehicle())
        return false;

    if (bot->InBattleground() || MapIsInstance(bot->GetMap()))
        return false;

    FollowTravelState& st = AI_VALUE(FollowTravelState&, "follow travel state");

    // Riding a taxi: stay selected only for our own flight leg, so StepTravel's InFlight
    // watchdog can force-land a flight whose spline died. Bailing out here unconditionally is
    // what made that watchdog dead code - a stalled bot keeps IsInFlight() forever, so the
    // action was never selected again and nothing could rescue it.
    if (bot->IsInFlight())
        return st.phase == FollowTravelPhase::InFlight;

    // Same for a ferry we boarded on purpose: stay selected so the Aboard phase can watch for
    // the destination dock. Any other transport (riding along with a player, an elevator) is
    // not ours to drive, so yield.
    if (bot->GetTransport())
        return st.phase == FollowTravelPhase::Aboard || st.phase == FollowTravelPhase::ToDock;

    uint32 const now = getMSTime();
    if (st.cooldownUntilMs && now < st.cooldownUntilMs)
        return false;

    // In combat only keep going if it's trash we can outrun; otherwise stop and let the
    // combat AI deal with it.
    if (inCombat && !TravelRunsThroughCombat(botAI, bot))
        return false;

    if (st.phase != FollowTravelPhase::None)
        return true;  // a trip is already in progress - keep this action selected

    WorldPosition goal;
    WorldPosition dungeonInside;
    bool toDungeon = false;
    if (!ResolveTravelGoal(botAI, bot, goal, dungeonInside, toDungeon))
        return false;

    if (toDungeon)
    {
        // Stay active the whole way in: the goal only resolves while the bot is still outside
        // the instance (ResolveLfgDungeon bails once it is on the dungeon map), and the action
        // itself decides between travelling, walking the last stretch, and stepping through the
        // portal - a bot cannot fire the entrance areatrigger on its own. Gating on distance
        // here would strand it standing on the portal.
        return true;
    }

    bool const sameMap = bot->GetMapId() == goal.GetMapId();
    float const dist2d = sameMap ? bot->GetExactDist2d(goal.GetPositionX(), goal.GetPositionY()) : FLT_MAX;

    bool const farEnough = !sameMap || dist2d > sPlayerbotAIConfig.groupSmartTravelMinDist;
    if (!farEnough)
        return false;

    if (!sTravelMgr.GetFlightPathToward(bot, goal).empty())
        return true;

    TravelMgr::PortalHop hop;
    if (sTravelMgr.FindPortalHop(bot, goal, hop, false))
        return true;

    if (!sameMap)
    {
        WorldPosition hub;
        return sTravelMgr.GetPortalHubStaging(bot, goal, hub) &&
               !sTravelMgr.GetFlightPathToward(bot, hub).empty();
    }

    // Same map, no flight route, but past the min distance: plain follow would beeline this in
    // a straight line through terrain. Take over and walk it on the navmesh instead.
    return true;
}

bool FollowTravelAction::isUseful()
{
    if (!sPlayerbotAIConfig.groupSmartTravel)
        return false;

    FollowTravelState& st = AI_VALUE(FollowTravelState&, "follow travel state");

    // Our own taxi leg is running: always useful, so the stalled-flight watchdog keeps ticking
    // even if the goal stops resolving (e.g. the master zoned into the instance meanwhile).
    if (bot->IsInFlight() && st.phase == FollowTravelPhase::InFlight)
        return true;

    if (bot->GetTransport() &&
        (st.phase == FollowTravelPhase::Aboard || st.phase == FollowTravelPhase::ToDock))
        return true;

    if (st.cooldownUntilMs && getMSTime() < st.cooldownUntilMs)
        return false;

    WorldPosition goal;
    WorldPosition dungeonInside;
    bool toDungeon = false;
    return ResolveTravelGoal(botAI, bot, goal, dungeonInside, toDungeon);
}

bool FollowTravelAction::Execute(Event /*event*/)
{
    FollowTravelState& st = AI_VALUE(FollowTravelState&, "follow travel state");

    // Riding a taxi: wait it out and let StepTravel's InFlight watchdog rescue a dead flight.
    // Deliberately before goal resolution - the goal can stop resolving mid-flight (the master
    // zones into the instance) and the watchdog must not be lost with it - and before the
    // short-range dungeon handling, which would issue a MoveTo while the bot is airborne.
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

    // Riding a ferry we boarded ourselves: same reasoning as the taxi branch above - the goal
    // may stop resolving mid-crossing, and the Aboard phase must survive that to step off.
    if (bot->GetTransport() &&
        (st.phase == FollowTravelPhase::Aboard || st.phase == FollowTravelPhase::ToDock))
    {
        st.phase = FollowTravelPhase::Aboard;
        return StepTravel(st, st.goal);
    }

    WorldPosition goal;
    WorldPosition dungeonInside;
    bool toDungeon = false;
    if (!ResolveTravelGoal(botAI, bot, goal, dungeonInside, toDungeon))
    {
        st.Clear();
        return false;
    }

    if (toDungeon)
    {
        bool const sameMap = bot->GetMapId() == goal.GetMapId();
        float const d = sameMap ? bot->GetExactDist2d(goal.GetPositionX(), goal.GetPositionY()) : FLT_MAX;

        if (sameMap && d < DUNGEON_STEP_DIS)
        {
            // On the portal - step inside, the same short hop clicking it performs.
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            bot->TeleportTo(dungeonInside.GetMapId(), dungeonInside.GetPositionX(), dungeonInside.GetPositionY(),
                            dungeonInside.GetPositionZ(), dungeonInside.GetOrientation());
            st.Clear();
            return true;
        }

        if (sameMap && d < DUNGEON_TAKEOVER_DIS)
        {
            // Final approach on the same map - just walk in on the navmesh. Drop any leftover
            // trip state first: this branch is stateless and re-selected by the trigger every
            // tick, and a non-None phase would keep plain "follow" disabled if we got stuck.
            st.Clear();
            return MoveTo(goal.GetMapId(), goal.GetPositionX(), goal.GetPositionY(), goal.GetPositionZ(), false, false,
                          false, true);
        }
    }

    st.goalIsDungeon = toDungeon;
    st.dungeonInside = dungeonInside;

    if (!StepTravel(st, goal))
    {
        // If we bailed while still far from the goal it was a give-up, not an arrival: pause
        // briefly so the trigger doesn't immediately re-select us into a tight replan loop.
        bool const arrived = bot->GetMapId() == goal.GetMapId() &&
                             bot->GetExactDist2d(goal.GetPositionX(), goal.GetPositionY()) < DUNGEON_TAKEOVER_DIS;
        if (!arrived)
            st.cooldownUntilMs = getMSTime() + TAXI_FAIL_COOLDOWN_MS;

        st.Clear();
        return false;  // hand control back to plain "follow"
    }

    // Keep the bot mounted for the road legs. The normal mount check only mirrors a nearby
    // master; CheckMountStateAction's separated-from-master path lets it self-mount here.
    uint32 const nowMs = getMSTime();
    if (!bot->IsMounted() && !bot->IsInFlight() && !bot->IsInCombat() && !bot->GetTransport() &&
        st.phase != FollowTravelPhase::InFlight && st.phase != FollowTravelPhase::Aboard &&
        nowMs >= st.nextMountPokeMs)
    {
        bool const atFlightMaster =
            st.phase == FollowTravelPhase::ToFlightMaster &&
            bot->GetExactDist2d(st.flightMasterPos.GetPositionX(), st.flightMasterPos.GetPositionY()) < 80.0f;
        if (!atFlightMaster)
        {
            botAI->DoSpecificAction("check mount state", Event(), true);
            st.nextMountPokeMs = nowMs + 3000;
        }
    }

    return true;
}

bool FollowTravelAction::StepTravel(FollowTravelState& st, WorldPosition const& goal)
{
    uint32 const now = getMSTime();
    st.goal = goal;

    if (st.phase == FollowTravelPhase::None)
    {
        st.Clear();
        st.goal = goal;
        st.giveUpAtMs = now + GIVE_UP_MS;

        if (bot->IsInFlight())
        {
            st.phase = FollowTravelPhase::InFlight;
            return true;
        }

        if (bot->GetMapId() == goal.GetMapId() &&
            bot->GetExactDist2d(goal.GetPositionX(), goal.GetPositionY()) < ARRIVE_DIS)
            return false;

        // Standing at a portal that gets us closer to the goal: jump it. Also covers same-map
        // links with no walkable route, such as Darnassus <-> Rut'theran Village.
        TravelMgr::PortalHop hop;
        if (sTravelMgr.FindPortalHop(bot, goal, hop))
        {
            st.portalStaging = hop.staging;
            st.portalDestMap = hop.destMap;
            st.portalDestPos = hop.destPos;
            st.phase = FollowTravelPhase::ToPortal;
            st.ResetLeg(st.portalStaging);
            return true;
        }

        // Otherwise fly toward the goal if there is a taxi route for it; if the goal is on
        // another continent with no direct taxi route, fly to a portal hub (Dalaran) first -
        // FindPortalHop then takes over once we land.
        WorldPosition arrival;
        std::vector<uint32> nodes = sTravelMgr.GetFlightPathToward(bot, goal, &arrival);
        if (nodes.size() < 2)
        {
            WorldPosition hub;
            if (sTravelMgr.GetPortalHubStaging(bot, goal, hub))
                nodes = sTravelMgr.GetFlightPathToward(bot, hub, &arrival);
        }
        // A ferry that bridges the two maps: usable either as a flight destination (fly to the
        // harbour first) or, once we are near enough, as a leg of its own. Resolved before the
        // flyable check below so a harbour-bound taxi chain is validated like any other.
        TravelMgr::TransportLeg ferry;
        bool const hasFerry = sPlayerbotAIConfig.smartTravelUseTransports && bot->GetMapId() != goal.GetMapId() &&
                              sTravelMgr.FindTransportLeg(bot, goal, ferry);

        if (nodes.size() < 2 && hasFerry)
            nodes = sTravelMgr.GetFlightPathToward(bot, ferry.board.pos, &arrival);

        // Only commit to a flight if the chain is short enough and every hop has usable taxi
        // geometry - a long or gappy BFS relay strands the bot mid-flight (see TaxiChainFlyable).
        bool const flyable = nodes.size() >= 2 &&
                             (nodes.size() - 1) <= sPlayerbotAIConfig.smartTravelMaxTaxiHops &&
                             TaxiChainFlyable(nodes);

        if (nodes.size() >= 2 && !flyable)
            LOG_DEBUG("playerbots", "[FollowTravel] {}: rejecting {}-hop taxi chain (too long or gappy)",
                      bot->GetName(), nodes.size());

        if (flyable)
        {
            if (TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot))
            {
                st.flightMasterEntry = fm->templateEntry;
                st.flightMasterPos = fm->pos;
                st.taxiNodes = std::move(nodes);
                st.taxiArrival = arrival;
                st.taxiTakeoffAtMs = 0;
                st.flightStallSinceMs = 0;
                st.phase = FollowTravelPhase::ToFlightMaster;
                st.ResetLeg(st.flightMasterPos);
                LOG_DEBUG("playerbots", "[FollowTravel] {}: fly ({} hops) toward map {} ({:.0f},{:.0f})",
                          bot->GetName(), st.taxiNodes.size(), goal.GetMapId(), goal.GetPositionX(),
                          goal.GetPositionY());
                return true;
            }
        }

        // No flight route. If a portal on this map leads closer to the goal and we can walk to
        // it, go there on foot: that is the only way out of places like Darnassus, whose sole
        // exit is the Rut'theran portal with the flight master on the far side of it.
        {
            TravelMgr::PortalHop walkHop;
            if (sTravelMgr.FindPortalHop(bot, goal, walkHop, false) && SameMapReachable(bot, walkHop.staging))
            {
                st.portalStaging = walkHop.staging;
                st.portalDestMap = walkHop.destMap;
                st.portalDestPos = walkHop.destPos;
                st.phase = FollowTravelPhase::ToPortal;
                st.ResetLeg(st.portalStaging);
                LOG_DEBUG("playerbots", "[FollowTravel] {}: walking to portal on map {} ({:.0f},{:.0f})",
                          bot->GetName(), walkHop.staging.GetMapId(), walkHop.staging.GetPositionX(),
                          walkHop.staging.GetPositionY());
                return true;
            }
        }

        // Still here: no flight and no portal to the goal's continent. If a ferry bridges the
        // two maps and its harbour is walkable from here, go wait for it.
        if (hasFerry && SameMapReachable(bot, ferry.board.pos))
        {
            st.transportEntry = ferry.entry;
            st.dockPos = ferry.board.pos;
            st.landPos = ferry.land.pos;
            st.dockWaitSinceMs = 0;
            st.phase = FollowTravelPhase::ToDock;
            st.ResetLeg(st.dockPos);
            LOG_DEBUG("playerbots", "[FollowTravel] {}: ferry {} from map {} to map {}", bot->GetName(),
                      ferry.entry, ferry.board.mapId, ferry.land.mapId);
            return true;
        }

        // No flight and no portal. If we are on the goal's map, close enough, and the navmesh
        // actually connects there, walk it on real paths. Beyond the walk cap or across an
        // unlinked landmass (Bloodmyst Isle -> Hellfire, same map 530) fall through instead of
        // beelining across terrain / the ocean floor.
        if (bot->GetMapId() == goal.GetMapId())
        {
            float const gd = bot->GetExactDist2d(goal.GetPositionX(), goal.GetPositionY());
            if (gd <= sPlayerbotAIConfig.smartTravelMaxWalkDist && SameMapReachable(bot, goal))
            {
                st.phase = FollowTravelPhase::FinalApproach;
                st.ResetLeg(goal);
                LOG_DEBUG("playerbots", "[FollowTravel] {}: walk {:.0f}y toward goal on map {} (no flight route)",
                          bot->GetName(), gd, goal.GetMapId());
                return true;
            }

            LOG_DEBUG("playerbots", "[FollowTravel] {}: goal on map {} is {:.0f}y away and not walkable - fallback",
                      bot->GetName(), goal.GetMapId(), gd);
        }

        // No road route to the goal (another map with no taxi/portal link, or a same-map spot
        // the navmesh cannot reach). Last resort so a grouped bot is not stranded forever:
        // teleport it to the goal. Guarded by config, off = the bot just gives up.
        if (sPlayerbotAIConfig.smartTravelTeleportFallback && !bot->IsInCombat())
        {
            LOG_DEBUG("playerbots", "[FollowTravel] {}: no route to map {} - teleport fallback", bot->GetName(),
                      goal.GetMapId());
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            bot->TeleportTo(goal.GetMapId(), goal.GetPositionX(), goal.GetPositionY(), goal.GetPositionZ(),
                            bot->GetOrientation());
        }
        else
        {
            LOG_DEBUG("playerbots", "[FollowTravel] {}: no route to map {} and teleport fallback off - giving up",
                      bot->GetName(), goal.GetMapId());
        }

        return false;
    }

    // InFlight and the ferry phases run on their own budgets: a taxi ride or a wait at a dock
    // is legitimately minutes long and must not trip the per-plan give-up timer.
    if (st.phase != FollowTravelPhase::InFlight && st.phase != FollowTravelPhase::ToDock &&
        st.phase != FollowTravelPhase::Aboard && now > st.giveUpAtMs)
        return false;

    switch (st.phase)
    {
        case FollowTravelPhase::ToFlightMaster:
        {
            if (bot->IsInFlight())
            {
                st.phase = FollowTravelPhase::InFlight;
                return true;
            }

            float const distToFm = bot->GetExactDist(st.flightMasterPos.GetPositionX(),
                                                     st.flightMasterPos.GetPositionY(),
                                                     st.flightMasterPos.GetPositionZ());
            if (distToFm > INTERACTION_DISTANCE)
                return TravelFarTo(st, st.flightMasterPos);

            Creature* flightMaster = bot->FindNearestCreature(st.flightMasterEntry, INTERACTION_DISTANCE * 3.0f);
            if (!flightMaster || !flightMaster->IsAlive())
                return false;

            if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
                return TravelFarTo(st, WorldPosition(flightMaster));

            // The flight can't be started in combat - wait it out rather than burning the
            // retry budget.
            if (bot->IsInCombat())
                return true;

            // Staggered take-off so a whole group does not click the flight master at once.
            if (st.taxiTakeoffAtMs == 0)
            {
                int32 slotIndex = botAI->GetGroupSlotIndex(bot);
                uint32 const slot = slotIndex > 0 ? static_cast<uint32>(slotIndex) : 0u;
                uint32 delay = sPlayerbotAIConfig.botTaxiDelayMin + slot * sPlayerbotAIConfig.botTaxiGapMs +
                               urand(0, sPlayerbotAIConfig.botTaxiGapJitterMs);
                delay = std::min(delay, sPlayerbotAIConfig.botTaxiDelayMax);
                st.taxiTakeoffAtMs = now + delay;
                return true;
            }
            if (now < st.taxiTakeoffAtMs)
                return true;

            // The taxi-master path rejects a mounted/shapeshifted player outright.
            botAI->RemoveShapeshift();
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (bot->IsMounted())
                bot->Dismount();

            bot->GetSession()->SendLearnNewTaxiNode(flightMaster);
            for (uint32 node : st.taxiNodes)
                bot->m_taxi.SetTaximaskNode(node);

            // Bots are not part of the economy - cover the fare so a low-gold bot is not
            // stranded at the flight master.
            if (sPlayerbotAIConfig.smartTravelFreeFare && !bot->isTaxiCheater())
            {
                uint32 const fare = TaxiRouteCost(st.taxiNodes);
                if (fare && bot->GetMoney() < fare)
                    bot->ModifyMoney(static_cast<int32>(fare - bot->GetMoney()));
            }

            if (!bot->ActivateTaxiPathTo(st.taxiNodes, flightMaster, 0))
            {
                // Instant-taxi servers teleport the bot and still return false - detect that by
                // the bot no longer being at the flight master, and just re-plan from there.
                if (bot->IsInFlight() ||
                    bot->GetExactDist2d(st.flightMasterPos.GetPositionX(), st.flightMasterPos.GetPositionY()) > 100.0f)
                {
                    st.phase = FollowTravelPhase::None;
                    return true;
                }

                // Something genuinely refuses the flight (DBC path gap, still unaffordable, ...):
                // pause smart travel for this bot so it doesn't loop back here forever.
                if (++st.flightFails >= 2)
                {
                    st.cooldownUntilMs = now + TAXI_FAIL_COOLDOWN_MS;
                    return false;
                }
                return true;  // retry once next tick
            }

            st.phase = FollowTravelPhase::InFlight;
            return true;
        }

        case FollowTravelPhase::InFlight:
        {
            if (!bot->IsInFlight())
            {
                // Landed - re-plan from here (this is what chains "fly to Dalaran" then
                // "portal out" over successive ticks).
                st.flightStallSinceMs = 0;
                st.flightProbeMs = 0;
                st.phase = FollowTravelPhase::None;
                return true;
            }

            // Stalled-flight watchdog. A taxi moves ~32 yd/s, so sampling the bot's position
            // every few seconds is an unambiguous liveness test: no movement means the flight
            // spline died (degenerate DBC path geometry / the "velocity > 0.01f" spline error)
            // and the bot is frozen in taxi pose, usually having dropped to the ground, with
            // UNIT_STATE_IN_FLIGHT stuck on forever.
            if (st.flightProbeMs == 0)
            {
                st.flightProbeMs = now;
                st.flightProbePos = WorldPosition(bot);
                return true;
            }

            if (now - st.flightProbeMs >= FLIGHT_PROBE_MS)
            {
                float const moved = bot->GetExactDist(st.flightProbePos.GetPositionX(),
                                                      st.flightProbePos.GetPositionY(),
                                                      st.flightProbePos.GetPositionZ());
                st.flightProbeMs = now;
                st.flightProbePos = WorldPosition(bot);

                if (moved >= FLIGHT_PROGRESS_DIS)
                    st.flightStallSinceMs = 0;
                else if (st.flightStallSinceMs == 0)
                    st.flightStallSinceMs = now;
            }

            // The spline reporting itself done is the same failure seen a few seconds earlier.
            if (st.flightStallSinceMs == 0 && bot->movespline && bot->movespline->Finalized())
                st.flightStallSinceMs = now;

            if (st.flightStallSinceMs == 0 || now - st.flightStallSinceMs < FLIGHT_STALL_MS)
                return true;

            // Land at the flight's intended endpoint. With nothing sensible recorded (a flight
            // we did not plan), teleport in place instead: that still runs the core's in-flight
            // cleanup and frees the bot, rather than flinging it at map 0 (0,0,0).
            WorldPosition to = st.taxiArrival ? st.taxiArrival : st.goal;
            if (!to)
                to = WorldPosition(bot);

            LOG_INFO("playerbots", "[FollowTravel] {}: taxi stalled in flight - force-landing at map {} ({:.0f},{:.0f})",
                     bot->GetName(), to.GetMapId(), to.GetPositionX(), to.GetPositionY());
            bot->TeleportTo(to.GetMapId(), to.GetPositionX(), to.GetPositionY(), to.GetPositionZ(),
                            bot->GetOrientation());
            st.flightStallSinceMs = 0;
            st.flightProbeMs = 0;
            st.phase = FollowTravelPhase::None;
            return true;
        }

        case FollowTravelPhase::ToDock:
        {
            // Already aboard (we may have walked on while it sat at the pier).
            if (bot->GetTransport())
            {
                st.phase = FollowTravelPhase::Aboard;
                st.dockWaitSinceMs = now;
                return true;
            }

            if (bot->GetMapId() != st.dockPos.GetMapId())
            {
                st.phase = FollowTravelPhase::None;
                return true;
            }

            if (bot->GetExactDist2d(st.dockPos.GetPositionX(), st.dockPos.GetPositionY()) > DOCK_ARRIVE_DIS)
                return TravelFarTo(st, st.dockPos);

            // Standing on the pier. Waiting for a ferry is minutes long by design, so this runs
            // on its own budget instead of the per-leg stuck detection.
            if (st.dockWaitSinceMs == 0)
                st.dockWaitSinceMs = now;
            else if (now - st.dockWaitSinceMs > sPlayerbotAIConfig.smartTravelDockWaitMs)
            {
                LOG_DEBUG("playerbots", "[FollowTravel] {}: ferry {} never arrived - abandoning dock",
                          bot->GetName(), st.transportEntry);
                return false;
            }

            Transport* transport = FindTransport(bot, st.transportEntry);
            if (!transport)
                return true;  // still on the far side of its loop

            // Wait until it is actually parked; boarding a moving hull drops the bot in the sea.
            if (!TravelMgr::IsTransportParked(transport))
                return true;

            // And until it is *this* dock it is parked at - the bot is standing on the pier, so
            // its own distance to the hull is the reliable test (the model origin is nowhere
            // near the route's stop node while berthed).
            if (bot->GetExactDist2d(transport->GetPositionX(), transport->GetPositionY()) > DOCK_BOARD_DIS)
                return true;

            // Step aboard rather than walk aboard. The navmesh knows nothing about a moving
            // object and the model origin usually sits inside the hull, so walking at it means
            // clipping through the boat, falling through the deck, or pacing the gangplank.
            WorldPosition deck;
            if (!sTravelMgr.FindDeckSpot(transport, bot, deck))
                return true;  // no usable spot this tick - try again while it is still berthed

            bot->NearTeleportTo(deck.GetPositionX(), deck.GetPositionY(), deck.GetPositionZ(),
                                bot->GetOrientation());
            transport->AddPassenger(bot, true);
            bot->StopMovingOnCurrentPos();
            st.phase = FollowTravelPhase::Aboard;
            st.dockWaitSinceMs = now;
            LOG_DEBUG("playerbots", "[FollowTravel] {}: boarded ferry {} bound for map {}", bot->GetName(),
                      st.transportEntry, st.landPos.GetMapId());
            return true;
        }

        case FollowTravelPhase::Aboard:
        {
            Transport* transport = bot->GetTransport();
            if (!transport)
            {
                // Ride ended, or we slipped off - re-plan from wherever we are.
                st.dockWaitSinceMs = 0;
                st.phase = FollowTravelPhase::None;
                return true;
            }

            // Safety net: never ride forever if the destination dock is somehow never matched.
            if (st.dockWaitSinceMs && now - st.dockWaitSinceMs > sPlayerbotAIConfig.smartTravelDockWaitMs)
            {
                LOG_DEBUG("playerbots", "[FollowTravel] {}: aboard {} too long - stepping off",
                          bot->GetName(), st.transportEntry);
                transport->RemovePassenger(bot);
                st.dockWaitSinceMs = 0;
                st.phase = FollowTravelPhase::None;
                return true;
            }

            // The core carries passengers across the map boundary, so simply wait until we are
            // berthed on the goal's side, then step ashore. Measured from the bot, which rides
            // with the hull - the transport's own origin is offset from the route's stop node.
            if (bot->GetMapId() == st.landPos.GetMapId() && TravelMgr::IsTransportParked(transport) &&
                bot->GetExactDist2d(st.landPos.GetPositionX(), st.landPos.GetPositionY()) < DOCK_LEAVE_DIS)
            {
                transport->RemovePassenger(bot);
                // Put the bot on the pier itself. Walking off a deck has the same problem as
                // walking on: it ends up clipping, or still aboard when the ferry departs.
                bot->NearTeleportTo(st.landPos.GetPositionX(), st.landPos.GetPositionY(),
                                    st.landPos.GetPositionZ(), bot->GetOrientation());
                LOG_DEBUG("playerbots", "[FollowTravel] {}: left ferry {} on map {}", bot->GetName(),
                          st.transportEntry, bot->GetMapId());
                st.dockWaitSinceMs = 0;
                st.phase = FollowTravelPhase::None;
            }

            return true;
        }

        case FollowTravelPhase::ToPortal:
        {
            if (bot->GetMapId() != st.portalStaging.GetMapId())
            {
                st.phase = FollowTravelPhase::None;
                return true;
            }

            if (bot->GetExactDist2d(st.portalStaging.GetPositionX(), st.portalStaging.GetPositionY()) > PORTAL_STEP_DIS)
                return TravelFarTo(st, st.portalStaging);

            // Step through the portal - the same short hop a player's click performs.
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            bot->TeleportTo(st.portalDestMap, st.portalDestPos.GetPositionX(), st.portalDestPos.GetPositionY(),
                            st.portalDestPos.GetPositionZ(), st.portalDestPos.GetOrientation());
            st.phase = FollowTravelPhase::None;
            return true;
        }

        case FollowTravelPhase::FinalApproach:
        {
            if (bot->GetMapId() != st.goal.GetMapId())
            {
                st.phase = FollowTravelPhase::None;
                return true;
            }

            if (bot->GetExactDist2d(st.goal.GetPositionX(), st.goal.GetPositionY()) < ARRIVE_DIS)
                return false;  // plain follow closes the last few yards

            if (TravelFarTo(st, st.goal))
                return true;

            // The leg gave up: no progress for LEG_STUCK_MS while still further than ARRIVE_DIS out.
            // SameMapReachable() deliberately accepts PATHFIND_INCOMPLETE, because a long overland
            // trip is walked as a chain of partial paths - but a goal the navmesh genuinely cannot
            // reach reports the same INCOMPLETE, and the bot then parks on the nearest reachable
            // polygon and re-plans the identical leg forever. Hellfire Citadel is where this shows:
            // the Blood Furnace door sits ~33y up on the wall dividing the peninsula, so a bot bound
            // for it walks the passage it shares with Ramparts / Shattered Halls, stops at the
            // Shattered Halls door and mills there. Treat an aborted final leg like "no road route"
            // and hop the remainder rather than looping.
            if (sPlayerbotAIConfig.smartTravelTeleportFallback && !bot->IsInCombat())
            {
                LOG_DEBUG("playerbots", "[FollowTravel] {}: final approach stalled {:.0f}y short on map {} - teleport fallback",
                          bot->GetName(), bot->GetExactDist2d(st.goal.GetPositionX(), st.goal.GetPositionY()),
                          st.goal.GetMapId());
                bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                bot->TeleportTo(st.goal.GetMapId(), st.goal.GetPositionX(), st.goal.GetPositionY(),
                                st.goal.GetPositionZ(), bot->GetOrientation());
            }

            st.phase = FollowTravelPhase::None;
            return false;
        }

        default:
            return false;
    }
}

bool FollowTravelAction::TravelFarTo(FollowTravelState& st, WorldPosition const& dest)
{
    if (!dest)
        return false;

    if (dest != st.moveFarPos)
        st.ResetLeg(dest);

    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return true;

    // Let a committed spline on the same map finish before recomputing (anti-oscillation,
    // as in NewRpgBaseAction::MoveFarTo).
    {
        LastMovement& lm = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lm.lastMoveToMapId == bot->GetMapId())
        {
            float const remaining = bot->GetExactDist(lm.lastMoveToX, lm.lastMoveToY, lm.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    uint32 const now = getMSTime();

    // Swimming for a long stretch means the route is crossing open water the navmesh only
    // "solves" by letting the cone sampler hop from wave to wave - that is how a bot ends up
    // swimming a lake or an ocean under the terrain. Bail so the caller can re-plan (or fall
    // back to a teleport) instead of grinding across.
    if (bot->IsInWater())
    {
        if (st.swimSinceMs == 0)
            st.swimSinceMs = now;
        else if (now - st.swimSinceMs > MAX_SWIM_MS)
        {
            LOG_DEBUG("playerbots", "[FollowTravel] {}: swimming too long toward ({:.0f},{:.0f}) - aborting leg",
                      bot->GetName(), dest.GetPositionX(), dest.GetPositionY());
            return false;
        }
    }
    else
        st.swimSinceMs = 0;

    float const distToDest =
        bot->GetExactDist(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());

    if (distToDest + 5.0f < st.nearestMoveFarDis)
    {
        st.nearestMoveFarDis = distToDest;
        st.stuckSinceMs = now;
        st.stuckAttempts = 0;
    }
    else if (++st.stuckAttempts >= 5 && st.stuckSinceMs != 0 && GetMSTimeDiffToNow(st.stuckSinceMs) >= LEG_STUCK_MS)
    {
        return false;  // no meaningful progress - abort the trip (never teleport to the master)
    }

    if (distToDest < PATHFINDER_DIS)
    {
        WorldPosition wp = AvoidHostiles(botAI, bot, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        return MoveTo(wp.GetMapId(), wp.GetPositionX(), wp.GetPositionY(), wp.GetPositionZ(), false, false, false, true);
    }

    uint32 const typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    {
        PathGenerator path(bot);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        if (!(path.GetPathType() & ~typeOk))
        {
            G3D::Vector3 const& end = path.GetActualEndPosition();
            if (dest.GetExactDist(end.x, end.y, end.z) + 5.0f < distToDest)
            {
                WorldPosition wp = AvoidHostiles(botAI, bot, end.x, end.y, end.z);
                return MoveTo(bot->GetMapId(), wp.GetPositionX(), wp.GetPositionY(), wp.GetPositionZ(), false, false,
                              false, true);
            }
        }
    }

    // Sample the forward cone for a reachable stepping stone so the bot keeps moving.
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    float const baseAngle = bot->GetAngle(dest.GetPositionX(), dest.GetPositionY());
    float best = static_cast<float>(M_PI);
    float rx = 0.0f;
    float ry = 0.0f;
    float rz = 0.0f;
    bool found = false;
    for (int i = 0; i < 2; ++i)
    {
        float const delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);
        float const sampleDis = (0.5f + rand_norm() * 0.5f) * PATHFINDER_DIS;
        float const angle = baseAngle + delta;
        float const dx = x + std::cos(angle) * sampleDis;
        float const dy = y + std::sin(angle) * sampleDis;
        float const dz = z + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        if (!(path.GetPathType() & ~typeOk) && std::fabs(delta) <= best)
        {
            found = true;
            G3D::Vector3 const& end = path.GetActualEndPosition();
            rx = end.x;
            ry = end.y;
            rz = end.z;
            best = std::fabs(delta);
        }
    }

    if (found)
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);

    return true;  // nothing usable this tick, but not declared stuck yet
}
