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
#include "GridDefines.h"
#include "GridTerrainData.h"
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
#include "SharedDefines.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelMgr.h"

namespace
{
    constexpr uint32 GIVE_UP_MS = 300 * 1000;      // no progress at all for this long => abandon the plan
    constexpr uint32 LEG_STUCK_MS = 25 * 1000;     // no progress on a single leg for this long => the leg stalled
    constexpr uint32 TAXI_FAIL_COOLDOWN_MS = 60 * 1000;  // pause smart travel after a give-up
    constexpr uint32 PENDING_MAX_MS = 60 * 1000;   // wait this long for the human we follow to finish loading
    constexpr uint32 MAX_SWIM_MS = 20 * 1000;  // longer than any legitimate crossing on a travel leg
    constexpr uint32 STEP_SEARCH_RETRY_MS = 2000;  // a failed stepping-stone search is not repeated sooner
    constexpr uint8 MAX_LEG_FAILS = 2;         // stalls on the way to a travel point before hopping to it
    constexpr float FLY_TO_LINK_DIS = 500.0f;  // farther than this from a portal / pier: consider flying there
    constexpr float TAXI_WORTH_DIS = 700.0f;   // same-region trips longer than this consider a flight
    constexpr float FLIGHT_MAX_WALK = 1500.0f;     // walk at most this far to a flight master
    constexpr float FLIGHT_OVERHEAD_DIS = 300.0f;  // take-off, landing and detours: a flight must beat walking by this
    constexpr float FLIGHT_ROUND_TRIP_DIS = 300.0f;  // a taxi landing this close to where we stand buys nothing
    constexpr float GRID_PRELOAD_DIS = 1600.0f;    // load the navmesh tiles of a walking leg up to this long
    constexpr float REPLAN_MOVE_DIS = 400.0f;      // the goal moved this far (or 30% of the way) => plan again
    constexpr float REPLAN_OTHER_MAP_DIS = 1500.0f;  // ...or this far while it is still on another map
    constexpr float DOCK_ARRIVE_DIS = 25.0f;   // "standing at the dock", and "close enough to step off"
    constexpr float DOCK_BOARD_DIS = 80.0f;    // bot-to-hull distance that counts as "berthed here"
    constexpr float DOCK_LAND_DIS = 120.0f;    // on the deck this close to the destination stop = arrived
    constexpr float DOCK_WAIT_PICK_DIS = 70.0f;  // pick a personal pier spot once this close to the dock
    constexpr float DOCK_WAIT_SPOT_DIS = 2.5f;   // standing on that spot
    constexpr uint32 FLIGHT_PROBE_MS = 3000;       // how often to sample in-flight progress
    constexpr uint32 FLIGHT_STALL_MS = 9000;       // no in-flight progress this long => dead taxi
    constexpr float FLIGHT_PROGRESS_DIS = 5.0f;    // yards a live taxi easily covers per probe
    constexpr float PATHFINDER_DIS = 70.0f;        // switch to direct MoveTo within this range (as NewRpg)
    constexpr float WALK_STEP_DIS = 90.0f;         // walk a route in steps this long; the core smooths each one
    constexpr float MIN_STEP_GAIN = 8.0f;          // a stepping stone must bring the bot at least this much closer
    constexpr float REGRESS_DIS = 250.0f;          // a guessed step leaving the bot this much farther => stalled
    constexpr float ARRIVE_DIS = 60.0f;            // "close enough" to the goal; hand back to plain follow
    constexpr float APPROACH_SEARCH_DIS = 250.0f;  // within this, look for the closest walkable point to the goal
    constexpr float APPROACH_AT_DIS = 8.0f;        // standing on that point
    constexpr float HOP_MIN_GAIN = 300.0f;         // a hop to a flight master must bring the bot this much closer
    constexpr float HOP_POINTLESS_DIS = 25.0f;     // a hop shorter than this changes nothing: give up instead
    constexpr uint8 MAX_APPROACH_TRIES = 2;        // re-aims before giving up on a dead end
    constexpr float PORTAL_STEP_DIS = 25.0f;       // how close to the hub centre before we jump the portal
    constexpr float DUNGEON_STEP_DIS = 20.0f;      // how close to the entrance before we step inside
    constexpr float DUNGEON_TAKEOVER_DIS = 150.0f;  // take over from plain follow beyond this to the entrance

    bool MapIsInstance(Map const* map) { return map && map->Instanceable(); }

    bool Travelable(Player* p)
    {
        return p && p->IsInWorld() && !p->IsBeingTeleported() && !MapIsInstance(p->GetMap());
    }

    bool IsHuman(Player* p) { return p && (IsRealPlayer(p) || IsSelfBot(p)); }

    uint32 RegionOf(Player* bot)
    {
        return TravelMgr::GetTravelRegion(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                          bot->GetPositionZ());
    }

    // Calls `fn` for the humans a bot travels toward, in order of preference: its master, the group leader
    // (what plain "follow" uses) and any other real player / self-bot in the group.
    template <typename Fn>
    Player* FirstHuman(PlayerbotAI* botAI, Player* bot, Fn fn)
    {
        Player* master = botAI->GetMaster();
        if (IsHuman(master) && master != bot && fn(master))
            return master;

        Player* leader = botAI->GetGroupLeader();
        if (IsHuman(leader) && leader != bot && fn(leader))
            return leader;

        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                if (m && m != bot && IsHuman(m) && fn(m))
                    return m;
            }

        return nullptr;
    }

    Player* TravelAnchor(PlayerbotAI* botAI, Player* bot)
    {
        return FirstHuman(botAI, bot, [](Player* p) { return Travelable(p); });
    }

    enum class GoalStatus : uint8
    {
        None,     // nothing to travel toward
        Pending,  // the human we follow is loading into a new place: keep the trip, re-aim once they land
        Ready
    };

    // Pick where this bot should travel. The group's LFG dungeon comes first and needs nobody to follow: a bot
    // that loses its master on the way (teleported off, zoned into some other instance) carries on to the dungeon
    // the group is queued for. Otherwise the travel anchor's position.
    GoalStatus ResolveTravelGoal(PlayerbotAI* botAI, Player* bot, WorldPosition& goal, WorldPosition& dungeonInside,
                                 bool& toDungeon)
    {
        WorldPosition outside;
        if (LfgWalkToDungeonTarget(bot, outside, dungeonInside))
        {
            goal = outside;
            toDungeon = true;
            return GoalStatus::Ready;
        }

        toDungeon = false;
        if (Player* anchor = TravelAnchor(botAI, bot))
        {
            goal = WorldPosition(anchor);
            return GoalStatus::Ready;
        }

        bool const loading =
            FirstHuman(botAI, bot, [](Player* p) { return p->IsBeingTeleported() || !p->IsInWorld(); });
        return loading ? GoalStatus::Pending : GoalStatus::None;
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

    // FlightPathMovementGenerator::LoadPath silently fails on a hop whose directed TaxiPath is missing or has no
    // usable TaxiPathNode geometry, which leaves the bot flagged in-flight with a dead spline. The faction taxi
    // graph never offers such a hop; this re-checks the exact path ids the core will look up.
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

    // The transport we mean to ride, if it is currently on the bot's map. A cross-continent ferry lives in exactly
    // one map's container at a time, so a null result usually just means it is still on the far side of its loop.
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

    // A path the bot can really walk: built on the navmesh, not a straight-line fallback (tile not loaded, end off
    // the mesh, or longer than the point buffer) and not an error.
    bool PathWalkable(PathGenerator const& path)
    {
        PathType const type = path.GetPathType();
        if (type == PATHFIND_BLANK || (type & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT |
                                               PATHFIND_SHORT)))
            return false;

        return path.GetPath().size() >= 2;
    }

    // Corner-to-corner paths: a route of many hundred yards fits the point buffer (a smoothed one is cut to a
    // straight line past ~600 yards), and it is only used to pick a waypoint - the core smooths the walk to it.
    void CalculateRoute(PathGenerator& path, float x, float y, float z)
    {
        path.SetUseStraightPath(true);
        path.CalculatePath(x, y, z);
    }

    float PathLength(Movement::PointsArray const& points)
    {
        float len = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
            len += (points[i] - points[i - 1]).length();
        return len;
    }

    // The first `dist` yards of the path (all of it when it is shorter). Its last point is where the bot walks to;
    // the points before it are what it passes on the way.
    Movement::PointsArray LegAlong(Movement::PointsArray const& points, float dist)
    {
        Movement::PointsArray leg;
        if (points.empty())
            return leg;

        leg.push_back(points.front());
        for (size_t i = 1; i < points.size(); ++i)
        {
            float const seg = (points[i] - points[i - 1]).length();
            if (seg >= dist && seg > 0.0f)
            {
                leg.push_back(points[i - 1] + (points[i] - points[i - 1]) * (dist / seg));
                return leg;
            }

            leg.push_back(points[i]);
            dist -= seg;
        }
        return leg;
    }

    // Detour can only route through map tiles that are loaded, and tiles load with the grids around players. A leg
    // of a few hundred yards therefore often has no route at all - the bot has to walk most of it blindly, which is
    // how bots crept along the Hellfire Citadel wall instead of running round it to the Blood Furnace door. Load the
    // handful of grids the leg crosses up front; they hold only what a player walking there would load anyway.
    void PreloadRouteGrids(Player* bot, WorldPosition const& dest)
    {
        Map* map = bot->GetMap();
        if (!map || bot->GetMapId() != dest.GetMapId())
            return;

        float const dx = dest.GetPositionX() - bot->GetPositionX();
        float const dy = dest.GetPositionY() - bot->GetPositionY();
        float const dist = std::sqrt(dx * dx + dy * dy);
        if (dist < 1.0f || dist > GRID_PRELOAD_DIS)
            return;

        uint32 const steps = static_cast<uint32>(dist / (SIZE_OF_GRIDS * 0.5f)) + 1;
        for (uint32 i = 0; i <= steps; ++i)
        {
            float const t = static_cast<float>(i) / static_cast<float>(steps);
            float const x = bot->GetPositionX() + dx * t;
            float const y = bot->GetPositionY() + dy * t;
            if (!Acore::IsValidMapCoord(x, y) || map->IsGridLoaded(x, y))
                continue;

            map->LoadGrid(x, y);
        }
    }

    // The next waypoint toward `dest`, at most WALK_STEP_DIS along a real navmesh route.
    //
    // 1. A route to the destination itself. A complete one is trusted even where it first leads away (round a wall,
    //    down to a gate); a partial one only when it ends nearer, or is a long prefix of a longer route.
    // 2. Otherwise - the destination's tile is not loaded yet, or it sits off the mesh - aim at points toward it
    //    inside the loaded area and take the route that ends nearest the destination, detours penalised. The old
    //    random cone sampler happily walked bots out of the back gate of a keep and away from the goal.
    //
    // `leg` is the stretch of that route up to the waypoint - what the bot is about to walk.
    bool NextRouteStep(Player* bot, WorldPosition const& dest, WorldPosition& out, bool& onRoute,
                       float& routeLength, Movement::PointsArray& leg)
    {
        float const distToDest = bot->GetExactDist(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());

        {
            PathGenerator path(bot);
            CalculateRoute(path, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
            if (PathWalkable(path))
            {
                Movement::PointsArray const& points = path.GetPath();
                G3D::Vector3 const& end = path.GetActualEndPosition();
                bool const complete = !(path.GetPathType() & PATHFIND_INCOMPLETE);
                float const endGap = dest.GetExactDist(end.x, end.y, end.z);
                float const length = PathLength(points);

                if (complete || endGap + MIN_STEP_GAIN < distToDest || length >= 2.0f * WALK_STEP_DIS)
                {
                    leg = LegAlong(points, WALK_STEP_DIS);
                    G3D::Vector3 const& step = leg.back();
                    out = WorldPosition(bot->GetMapId(), step.x, step.y, step.z);
                    onRoute = complete;
                    routeLength = length;  // only meaningful for a complete route: what is left of the way
                    return true;
                }
            }
        }

        Map* map = bot->GetMap();
        if (!map)
            return false;

        static float const RADII[] = { 120.0f, 70.0f, 35.0f };
        static float const OFFSETS[] = { 0.0f, 0.44f, -0.44f, 0.87f, -0.87f, 1.4f, -1.4f, 2.0f, -2.0f };  // radians

        float const bx = bot->GetPositionX();
        float const by = bot->GetPositionY();
        float const bz = bot->GetPositionZ();
        float const base = bot->GetAngle(dest.GetPositionX(), dest.GetPositionY());
        uint32 const phase = bot->GetPhaseMask();

        bool found = false;
        float bestScore = distToDest - MIN_STEP_GAIN;
        for (float const radius : RADII)
        {
            for (float const offset : OFFSETS)
            {
                float const cx = bx + std::cos(base + offset) * radius;
                float const cy = by + std::sin(base + offset) * radius;
                if (!Acore::IsValidMapCoord(cx, cy))
                    continue;

                float const cz = map->GetHeight(phase, cx, cy, bz + radius * 0.6f + 10.0f, true, radius * 1.2f + 30.0f);
                if (cz <= INVALID_HEIGHT)
                    continue;

                PathGenerator path(bot);
                CalculateRoute(path, cx, cy, cz);
                if (!PathWalkable(path))
                    continue;

                Movement::PointsArray const& points = path.GetPath();
                G3D::Vector3 const& end = path.GetActualEndPosition();
                float const straight = bot->GetExactDist(end.x, end.y, end.z);
                float const detour = std::max(0.0f, PathLength(points) - straight);
                float const score = dest.GetExactDist(end.x, end.y, end.z) + 0.25f * detour;
                if (score >= bestScore)
                    continue;

                leg = LegAlong(points, WALK_STEP_DIS);
                G3D::Vector3 const& step = leg.back();
                bestScore = score;
                found = true;
                out = WorldPosition(bot->GetMapId(), step.x, step.y, step.z);
            }

            // The widest ring that gains anything is the one to take; smaller rings only matter when it does not.
            if (found)
                break;
        }

        onRoute = false;
        routeLength = 0.0f;
        return found;
    }

    // The closest point to `goal` the bot can really walk to from where it stands: the end of the path toward the
    // goal itself, or toward ground samples around it (a door up a wall, a ledge off the navmesh). Straight-line
    // shortcuts and unloaded tiles do not count. False when nothing near the goal is reachable.
    bool FindApproachPoint(Player* bot, WorldPosition const& goal, WorldPosition& out)
    {
        Map* map = bot->GetMap();
        if (!map)
            return false;

        bool found = false;
        float best = FLT_MAX;
        auto consider = [&](float x, float y, float z)
        {
            PathGenerator path(bot);
            path.CalculatePath(x, y, z);
            if (!PathWalkable(path))
                return;

            G3D::Vector3 const& end = path.GetActualEndPosition();
            float const d = goal.GetExactDist(end.x, end.y, end.z);
            if (d >= best)
                return;

            best = d;
            found = true;
            out = WorldPosition(bot->GetMapId(), end.x, end.y, end.z, 0.0f);
        };

        consider(goal.GetPositionX(), goal.GetPositionY(), goal.GetPositionZ());

        uint32 const phase = bot->GetPhaseMask();
        for (float r = 10.0f; r <= 60.0f; r += 10.0f)
            for (uint8 a = 0; a < 8; ++a)
            {
                float const ang = static_cast<float>(a) * static_cast<float>(M_PI) / 4.0f;
                float const x = goal.GetPositionX() + std::cos(ang) * r;
                float const y = goal.GetPositionY() + std::sin(ang) * r;
                float const z = map->GetHeight(phase, x, y, goal.GetPositionZ() + 10.0f, true, 100.0f);
                if (z <= INVALID_HEIGHT)
                    continue;

                consider(x, y, z);
            }

        return found;
    }

    // Has the goal moved enough that the trip in progress no longer leads to it?
    bool GoalMoved(Player* bot, FollowTravelState const& st, WorldPosition const& goal, bool toDungeon)
    {
        if (!st.planGoal)
            return false;

        if (st.goalIsDungeon != toDungeon || st.planGoal.GetMapId() != goal.GetMapId() ||
            TravelMgr::GetTravelRegion(st.planGoal) != TravelMgr::GetTravelRegion(goal))
            return true;

        float const moved = st.planGoal.GetExactDist2d(goal.GetPositionX(), goal.GetPositionY());
        if (bot->GetMapId() != goal.GetMapId())
            return moved > REPLAN_OTHER_MAP_DIS;

        float const botToGoal = bot->GetExactDist2d(goal.GetPositionX(), goal.GetPositionY());
        return moved > std::max(REPLAN_MOVE_DIS, 0.3f * botToGoal);
    }

    constexpr float AVOID_MARGIN = 5.0f;         // pass this far outside a hostile's aggro radius
    constexpr float AVOID_MOVING_MARGIN = 4.0f;  // ...and this much more when it is walking about
    constexpr float AVOID_MAX_RADIUS = 45.0f;    // the core's own cap on an aggro radius
    constexpr float AVOID_HELD_RADIUS = 10.0f;   // a rooted one the bot left behind only reaches this far
    constexpr float AVOID_MIN_RADIUS = 3.0f;     // closer than this there is nothing left to avoid
    constexpr float AVOID_Z_RANGE = 12.0f;       // a hostile this far above / below the path does not see the bot
    constexpr float AVOID_LOOKAHEAD = 60.0f;     // how far along the path a hostile counts as "in the way"
    constexpr float AVOID_TANGENT_PAD = 3.0f;    // aim a detour this far outside the circle it rounds
    constexpr float AVOID_PACK_GAP = 6.0f;       // hostiles whose circles are this close cannot be passed between
    constexpr float AVOID_MIN_DETOUR = 8.0f;     // a detour point nearer than this is not worth a move
    constexpr float AVOID_MAX_DETOUR = 110.0f;   // ...and one farther than this is no detour
    constexpr float AVOID_MIN_GAIN = 4.0f;       // a detour that still clips a radius must clip it this much less
    constexpr float AVOID_SPOT_MISS = 4.0f;      // a path ending this far from the spot it was sent to did not reach it
    constexpr uint8 AVOID_MAX_PATHS = 8;         // path queries spent on one search for a way round
    constexpr uint8 AVOID_MAX_DETOURS = 5;       // detours in a row that bring the bot no nearer: then it pushes on
    constexpr uint32 AVOID_SCAN_MS = 400;        // how often a move under way is checked for hostiles on it
    constexpr uint32 AVOID_RETRY_MS = 3000;      // ...and how long a path known to clip one is left alone
    constexpr uint32 AVOID_DETOUR_MEMORY_MS = 20000;  // detours older than this no longer count as "in a row"

    // An idle hostile the bot would pull by passing within `radius` of it.
    struct Threat
    {
        float x;
        float y;
        float z;
        float radius;
    };

    // How deep a path runs into the hostiles' radii.
    struct PathRisk
    {
        float penetration = 0.0f;  // deepest it goes into any radius; 0 = it stays clear of all of them
        int32 blocker = -1;        // index of the hostile whose radius it enters first
    };

    struct DetourSpot
    {
        float x;
        float y;
        float cost;  // yards to the spot plus on to where the bot was heading
    };

    // The hostiles a travelling bot has to keep away from. The old sidestep looked at a 16 yard band along a straight
    // line - less than an aggro radius - and only once per 90 yard step, so most of what stood on the road was never
    // looked at; and with as many hostiles to the left as to the right it did nothing at all.
    //
    // `dest` is where the leg leads: a hostile it lies within reach of has to be passed anyway.
    void CollectThreats(PlayerbotAI* botAI, Player* bot, WorldPosition const& dest, std::vector<Threat>& out)
    {
        if (!sPlayerbotAIConfig.smartTravelAvoidEnemies)
            return;

        GuidVector const hostiles = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest hostile npcs")->Get();
        for (ObjectGuid const& guid : hostiles)
        {
            Unit* unit = botAI->GetUnit(guid);
            Creature* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->IsInEvadeMode())
                continue;

            // Nothing that would not come for the bot on its own.
            if (creature->IsCivilian() || creature->IsTrigger() || creature->IsCritter() || creature->IsTotem() ||
                creature->IsImmuneToPC() || creature->IsNeutralToAll() || !creature->HasReactState(REACT_AGGRESSIVE))
                continue;

            // An idle one is what the bot rounds to keep it idle. One already fighting is not in the way - it
            // leashes once outrun - unless it is held where it stands (the root the bot left behind): the route
            // runs right past it and it still swings at anything within reach.
            float radius = 0.0f;
            if (!creature->IsInCombat())
            {
                radius = creature->GetAggroRange(bot) + AVOID_MARGIN;
                if (creature->isMoving())
                    radius += AVOID_MOVING_MARGIN;
                radius = std::min(radius, AVOID_MAX_RADIUS);
            }
            else if (creature->HasRootAura())
                radius = AVOID_HELD_RADIUS;
            else
                continue;

            // Inside the radius already: what counts from here on is not getting any closer.
            radius = std::min(radius, bot->GetExactDist2d(creature) - 2.0f);
            if (radius < AVOID_MIN_RADIUS)
                continue;

            if (dest.GetExactDist2d(creature->GetPositionX(), creature->GetPositionY()) < radius)
                continue;

            Threat threat;
            threat.x = creature->GetPositionX();
            threat.y = creature->GetPositionY();
            threat.z = creature->GetPositionZ();
            threat.radius = radius;
            out.push_back(threat);
        }
    }

    // Checks the first `maxLen` yards of a path against the hostiles.
    PathRisk AssessPath(Movement::PointsArray const& points, float maxLen, std::vector<Threat> const& threats)
    {
        PathRisk risk;
        float firstEntry = FLT_MAX;
        for (size_t t = 0; t < threats.size(); ++t)
        {
            Threat const& threat = threats[t];
            float along = 0.0f;
            for (size_t i = 1; i < points.size() && along < maxLen; ++i)
            {
                G3D::Vector3 const& a = points[i - 1];
                G3D::Vector3 b = points[i];
                float seg = (b - a).length();
                if (seg <= 0.0f)
                    continue;

                if (along + seg > maxLen)
                {
                    b = a + (b - a) * ((maxLen - along) / seg);
                    seg = maxLen - along;
                }

                // Closest point of the segment to the hostile, seen from above.
                float const dx = b.x - a.x;
                float const dy = b.y - a.y;
                float const len2 = dx * dx + dy * dy;
                float u = len2 > 0.0f ? ((threat.x - a.x) * dx + (threat.y - a.y) * dy) / len2 : 0.0f;
                u = std::clamp(u, 0.0f, 1.0f);

                float const gapX = threat.x - (a.x + dx * u);
                float const gapY = threat.y - (a.y + dy * u);
                float const gap = std::sqrt(gapX * gapX + gapY * gapY);
                float const height = a.z + (b.z - a.z) * u;
                if (gap < threat.radius && std::fabs(threat.z - height) <= AVOID_Z_RANGE)
                {
                    risk.penetration = std::max(risk.penetration, threat.radius - gap);
                    float const entry = along + seg * u;
                    if (entry < firstEntry)
                    {
                        firstEntry = entry;
                        risk.blocker = static_cast<int32>(t);
                    }
                }

                along += seg;
            }
        }
        return risk;
    }

    // What is left of the move the bot is making right now, from where it stands.
    bool CommittedPath(Player* bot, Movement::PointsArray& out)
    {
        Movement::MoveSpline const* spline = bot->movespline;
        if (!spline || !spline->Initialized() || spline->Finalized() || spline->onTransport)
            return false;

        Movement::MoveSpline::MySpline const& curve = spline->_Spline();
        out.emplace_back(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        for (int32 i = spline->_currentSplineIdx() + 1; i <= curve.last() && i < curve.getPointCount(); ++i)
            out.push_back(curve.getPoint(i));

        return out.size() >= 2;
    }

    // Where the two tangents from the bot touch a circle, and two more spots further out along each tangent. The
    // straight line to a tangent point never comes inside the circle; the spots further out are the ones from which
    // the way on to `aim` clears it as well.
    void AddTangentSpots(Player* bot, float cx, float cy, float radius, WorldPosition const& aim,
                         std::vector<DetourSpot>& out)
    {
        static float const SIDES[] = { 1.0f, -1.0f };
        static float const EXTRA[] = { 0.0f, 15.0f, 30.0f };

        float const bx = bot->GetPositionX();
        float const by = bot->GetPositionY();
        float const dx = cx - bx;
        float const dy = cy - by;
        float const dist = std::sqrt(dx * dx + dy * dy);
        if (dist < 1.0f)
            return;

        // Standing inside the padded circle already: skirt it from where the bot is.
        float const r = std::min(radius + AVOID_TANGENT_PAD, dist - 0.5f);
        float const base = std::atan2(dy, dx);
        float const spread = std::asin(std::clamp(r / dist, 0.0f, 1.0f));
        float const reach = std::sqrt(std::max(0.0f, dist * dist - r * r));

        for (float const side : SIDES)
            for (float const extra : EXTRA)
            {
                float const len = reach + extra;
                if (len < AVOID_MIN_DETOUR || len > AVOID_MAX_DETOUR)
                    continue;

                DetourSpot spot;
                spot.x = bx + std::cos(base + side * spread) * len;
                spot.y = by + std::sin(base + side * spread) * len;
                spot.cost = len + aim.GetExactDist2d(spot.x, spot.y);
                out.push_back(spot);
            }
    }

    // The pack hostile `seed` stands in - everything chained to it by circles too close together to pass between -
    // as one circle that holds them all. False when it stands alone.
    bool PackCircle(std::vector<Threat> const& threats, size_t seed, float& cx, float& cy, float& radius)
    {
        std::vector<size_t> pack{ seed };
        std::vector<bool> taken(threats.size(), false);
        taken[seed] = true;
        for (size_t p = 0; p < pack.size(); ++p)
            for (size_t t = 0; t < threats.size(); ++t)
            {
                if (taken[t])
                    continue;

                Threat const& a = threats[pack[p]];
                Threat const& b = threats[t];
                float const dx = a.x - b.x;
                float const dy = a.y - b.y;
                if (std::sqrt(dx * dx + dy * dy) >= a.radius + b.radius + AVOID_PACK_GAP)
                    continue;

                taken[t] = true;
                pack.push_back(t);
            }

        if (pack.size() < 2)
            return false;

        cx = 0.0f;
        cy = 0.0f;
        for (size_t const t : pack)
        {
            cx += threats[t].x;
            cy += threats[t].y;
        }
        cx /= static_cast<float>(pack.size());
        cy /= static_cast<float>(pack.size());

        radius = 0.0f;
        for (size_t const t : pack)
        {
            float const dx = threats[t].x - cx;
            float const dy = threats[t].y - cy;
            radius = std::max(radius, std::sqrt(dx * dx + dy * dy) + threats[t].radius);
        }
        return true;
    }

    // A point to walk to first, so that the bot rounds the hostiles its way to `aim` runs into (`risk` is that way's
    // assessment). Round the one it would meet first, or round its whole pack; the cheapest spot the bot can really
    // walk to without entering any radius wins. With no such spot, the one that clips a radius least - when that is
    // really better than going straight on. `clear` tells which of the two it is.
    bool FindDetour(Player* bot, std::vector<Threat> const& threats, PathRisk const& risk, WorldPosition const& aim,
                    WorldPosition& out, bool& clear)
    {
        clear = false;
        Map* map = bot->GetMap();
        if (!map || risk.blocker < 0)
            return false;

        std::vector<DetourSpot> spots;
        Threat const& blocker = threats[risk.blocker];
        AddTangentSpots(bot, blocker.x, blocker.y, blocker.radius, aim, spots);

        float packX = 0.0f;
        float packY = 0.0f;
        float packRadius = 0.0f;
        if (PackCircle(threats, static_cast<size_t>(risk.blocker), packX, packY, packRadius))
            AddTangentSpots(bot, packX, packY, packRadius, aim, spots);

        std::sort(spots.begin(), spots.end(),
                  [](DetourSpot const& a, DetourSpot const& b) { return a.cost < b.cost; });

        uint32 const phase = bot->GetPhaseMask();
        G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        G3D::Vector3 const to(aim.GetPositionX(), aim.GetPositionY(), aim.GetPositionZ());

        bool haveClear = false;  // a spot reached without entering a radius (the way on from it may still clip one)
        bool haveLeast = false;  // a spot reached by clipping a radius less than the straight way does
        float leastPenetration = risk.penetration - AVOID_MIN_GAIN;
        WorldPosition clearSpot;
        WorldPosition leastSpot;
        uint8 queries = 0;
        for (DetourSpot const& spot : spots)
        {
            if (queries >= AVOID_MAX_PATHS)
                break;

            if (!Acore::IsValidMapCoord(spot.x, spot.y))
                continue;

            float const z = map->GetHeight(phase, spot.x, spot.y, from.z + 15.0f, true, 40.0f);
            if (z <= INVALID_HEIGHT || map->IsInWater(phase, spot.x, spot.y, z, bot->GetCollisionHeight()))
                continue;

            // The straight line there tells most of it, and costs no path query.
            Movement::PointsArray const line{ from, G3D::Vector3(spot.x, spot.y, z) };
            float const linePenetration = AssessPath(line, FLT_MAX, threats).penetration;
            if (linePenetration > 0.0f && (haveClear || linePenetration >= leastPenetration))
                continue;

            ++queries;
            PathGenerator path(bot);
            CalculateRoute(path, spot.x, spot.y, z);
            if (!PathWalkable(path))
                continue;

            // The navmesh has to lead to the spot itself, and not by some other way round.
            Movement::PointsArray const& points = path.GetPath();
            G3D::Vector3 const& end = path.GetActualEndPosition();
            float const missX = end.x - spot.x;
            float const missY = end.y - spot.y;
            if (std::sqrt(missX * missX + missY * missY) > AVOID_SPOT_MISS ||
                PathLength(points) > 1.5f * (end - from).length() + 10.0f)
                continue;

            WorldPosition const reached(bot->GetMapId(), end.x, end.y, end.z);
            float const penetration = AssessPath(points, FLT_MAX, threats).penetration;
            if (penetration > 0.0f)
            {
                if (!haveClear && penetration < leastPenetration)
                {
                    leastPenetration = penetration;
                    leastSpot = reached;
                    haveLeast = true;
                }
                continue;
            }

            // Clear all the way to the spot. The first such spot from which the way on is clear too is the one:
            // as the crow flies first, then - only when that looks clear - as the navmesh really leads, which is
            // what catches the gate or the bridge behind a hostile that brings the road straight back to it.
            Movement::PointsArray const onward{ end, to };
            if (AssessPath(onward, AVOID_LOOKAHEAD, threats).blocker < 0 && queries < AVOID_MAX_PATHS)
            {
                ++queries;
                PathGenerator next(bot);
                next.SetUseStraightPath(true);
                next.CalculatePath(end.x, end.y, end.z, to.x, to.y, to.z, false);
                if (PathWalkable(next) && AssessPath(next.GetPath(), AVOID_LOOKAHEAD, threats).blocker < 0)
                {
                    out = reached;
                    clear = true;
                    return true;
                }
            }

            if (!haveClear)
            {
                clearSpot = reached;
                haveClear = true;
            }
        }

        if (haveClear)
        {
            out = clearSpot;
            clear = true;
            return true;
        }

        if (haveLeast)
        {
            out = leastSpot;
            return true;
        }

        return false;
    }
}

bool FollowTravelCovers(PlayerbotAI* botAI)
{
    return sPlayerbotAIConfig.groupSmartTravel && botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) &&
           !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT);
}

bool IsSmartTravelling(PlayerbotAI* botAI)
{
    FollowTravelState const& st =
        botAI->GetAiObjectContext()->GetValue<FollowTravelState&>("follow travel state")->Get();
    if (st.phase == FollowTravelPhase::None)
        return false;

    // A walking trip nobody has stepped (no progress) for its whole give-up window is dead, not paused: StepTravel
    // would abandon it on its next tick. Its state can outlive it - a bot that fell out of a trip driven from
    // outside the combat engine keeps the phase it had - and must not keep the bot from grinding or fleeing.
    // Riding a taxi or a ferry has no such clock.
    if (st.phase != FollowTravelPhase::InFlight && st.phase != FollowTravelPhase::ToDock &&
        st.phase != FollowTravelPhase::Aboard && st.giveUpAtMs && getMSTime() > st.giveUpAtMs)
        return false;

    return true;
}

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
    if (!FollowTravelCovers(botAI))
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
    if (ResolveTravelGoal(botAI, bot, goal, dungeonInside, toDungeon) != GoalStatus::Ready)
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

    // Past the follow distance the planner always has a move: fly, follow the portal / taxi / ferry chain to the
    // goal's region, or walk. Handing this to plain follow would beeline straight through terrain and water.
    //
    // Out of combat the walk takes over much sooner when it is to go round hostiles: plain follow runs the shortest
    // navmesh line through whatever stands on it, which is how a bot invited from across a camp arrived with the
    // whole camp behind it. In combat the long distance holds, so a bot is not pulled out of a fight it is in just
    // because its master stepped away.
    float minDist = sPlayerbotAIConfig.groupSmartTravelMinDist;
    if (!inCombat && sPlayerbotAIConfig.smartTravelAvoidEnemies)
        minDist = std::min(minDist, std::max(sPlayerbotAIConfig.smartTravelAvoidMinDist, ARRIVE_DIS + 20.0f));

    return !sameMap || dist2d > minDist;
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
    GoalStatus const status = ResolveTravelGoal(botAI, bot, goal, dungeonInside, toDungeon);
    return status == GoalStatus::Ready || (status == GoalStatus::Pending && st.phase != FollowTravelPhase::None);
}

bool FollowTravelAction::Execute(Event /*event*/)
{
    FollowTravelState& st = AI_VALUE(FollowTravelState&, "follow travel state");
    uint32 const nowMs = getMSTime();

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
    GoalStatus const status = ResolveTravelGoal(botAI, bot, goal, dungeonInside, toDungeon);

    if (status == GoalStatus::Pending)
    {
        // Keep the plan while they load; drop it if they never reappear.
        if (!st.pendingSinceMs)
            st.pendingSinceMs = nowMs;

        if (nowMs - st.pendingSinceMs < PENDING_MAX_MS)
            return true;
    }

    if (status != GoalStatus::Ready)
    {
        st.Clear();
        return false;
    }

    st.pendingSinceMs = 0;

    if (toDungeon && bot->GetMapId() == goal.GetMapId() &&
        bot->GetExactDist2d(goal.GetPositionX(), goal.GetPositionY()) < DUNGEON_STEP_DIS)
    {
        // On the portal - step inside, the same short hop clicking it performs.
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        bot->TeleportTo(dungeonInside.GetMapId(), dungeonInside.GetPositionX(), dungeonInside.GetPositionY(),
                        dungeonInside.GetPositionZ(), dungeonInside.GetOrientation());
        st.Clear();
        return true;
    }

    // The goal went somewhere else (the master teleported, the group was queued meanwhile): the leg in progress
    // leads to the old place, so plan again from here.
    if (st.phase != FollowTravelPhase::None && GoalMoved(bot, st, goal, toDungeon))
    {
        LOG_DEBUG("playerbots", "[FollowTravel] {}: goal moved to map {} ({:.0f},{:.0f}) - re-planning",
                  bot->GetName(), goal.GetMapId(), goal.GetPositionX(), goal.GetPositionY());
        st.phase = FollowTravelPhase::None;
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

bool FollowTravelAction::PlanFlight(FollowTravelState& st, WorldPosition const& target, float directDist)
{
    TravelMgr::FlightPlan plan;
    if (!sTravelMgr.PlanFlightToward(bot, target, FLIGHT_MAX_WALK, plan))
        return false;

    // Only worth it when walking to the flight master and on from the landing beats walking there outright.
    if (plan.walkDist + plan.landDist + FLIGHT_OVERHEAD_DIS >= directDist)
        return false;

    // ...and never a round trip. The only node near a stuck bot is often the one it just flew into (Honor Hold for
    // the Hellfire Citadel doors); flying back to it would loop for ever.
    if (bot->GetExactDist2d(plan.arrival.GetPositionX(), plan.arrival.GetPositionY()) < FLIGHT_ROUND_TRIP_DIS)
    {
        LOG_DEBUG("playerbots", "[FollowTravel] {}: rejecting a taxi that lands where it took off", bot->GetName());
        return false;
    }

    if (!TaxiChainFlyable(plan.nodes))
    {
        LOG_DEBUG("playerbots", "[FollowTravel] {}: rejecting {}-node taxi chain with a DBC gap", bot->GetName(),
                  plan.nodes.size());
        return false;
    }

    st.flightMasterEntry = plan.flightMaster->templateEntry;
    st.flightMasterPos = plan.flightMaster->pos;
    st.taxiNodes = std::move(plan.nodes);
    st.taxiArrival = plan.arrival;
    st.taxiTakeoffAtMs = 0;
    st.flightStallSinceMs = 0;
    st.phase = FollowTravelPhase::ToFlightMaster;
    st.ResetLeg(st.flightMasterPos);
    LOG_DEBUG("playerbots",
              "[FollowTravel] {}: fly ({} hops) toward map {} ({:.0f},{:.0f}) - walk {:.0f}y, land {:.0f}y short",
              bot->GetName(), st.taxiNodes.size() - 1, target.GetMapId(), target.GetPositionX(),
              target.GetPositionY(), plan.walkDist, plan.landDist);
    return true;
}

bool FollowTravelAction::PlanLink(FollowTravelState& st, TravelMgr::TravelEdge const& edge)
{
    float const toStaging = bot->GetExactDist2d(edge.staging.GetPositionX(), edge.staging.GetPositionY());

    switch (edge.kind)
    {
        case TravelMgr::TravelEdge::Kind::Taxi:
        {
            // A flight from any flight master nearby straight across the seam; else walk to the one the link
            // starts at and fly the link itself.
            if (PlanFlight(st, edge.dest, FLT_MAX))
                return true;

            TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetFlightMasterForNode(edge.taxiFrom, bot->GetTeamId());
            std::vector<uint32> nodes = sTravelMgr.GetFactionTaxiRoute(edge.taxiFrom, edge.taxiTo, bot->GetTeamId());
            if (!fm || nodes.size() < 2 || !TaxiChainFlyable(nodes))
                return false;

            st.flightMasterEntry = fm->templateEntry;
            st.flightMasterPos = fm->pos;
            st.taxiNodes = std::move(nodes);
            st.taxiArrival = edge.dest;
            st.taxiTakeoffAtMs = 0;
            st.flightStallSinceMs = 0;
            st.phase = FollowTravelPhase::ToFlightMaster;
            st.ResetLeg(st.flightMasterPos);
            LOG_DEBUG("playerbots", "[FollowTravel] {}: walking to flight master for region link {} -> {}",
                      bot->GetName(), edge.fromRegion, edge.toRegion);
            return true;
        }
        case TravelMgr::TravelEdge::Kind::Portal:
        {
            // A same-region shortcut - a capital's own portal to the foot of the Dark Portal is the standing
            // example - can get the bot to this edge's staging point far faster than the region graph (which
            // only models hops that cross a region boundary) can see. Take it first; landing near `edge.staging`
            // is all that matters, StepTravel re-plans fresh from there and picks up the real edge with a tiny
            // remaining distance.
            TravelMgr::TravelEdge shortcut;
            bool const haveShortcut = sTravelMgr.FindApproachShortcut(bot, edge.staging, shortcut);
            TravelMgr::TravelEdge const& use = haveShortcut ? shortcut : edge;
            float const toUseStaging = haveShortcut
                                           ? bot->GetExactDist2d(use.staging.GetPositionX(), use.staging.GetPositionY())
                                           : toStaging;

            if (toUseStaging > FLY_TO_LINK_DIS && PlanFlight(st, use.staging, toUseStaging))
                return true;

            st.portalStaging = use.staging;
            st.portalDestMap = use.dest.GetMapId();
            st.portalDestPos = use.dest;
            st.phase = FollowTravelPhase::ToPortal;
            st.ResetLeg(st.portalStaging);
            LOG_DEBUG("playerbots", "[FollowTravel] {}: portal link {} -> {} at ({:.0f},{:.0f}), {:.0f}y away{}",
                      bot->GetName(), edge.fromRegion, edge.toRegion, use.staging.GetPositionX(),
                      use.staging.GetPositionY(), toUseStaging, haveShortcut ? " (same-region shortcut)" : "");
            return true;
        }
        case TravelMgr::TravelEdge::Kind::Ferry:
        {
            if (toStaging > FLY_TO_LINK_DIS && PlanFlight(st, edge.staging, toStaging))
                return true;

            st.transportEntry = edge.transportEntry;
            st.waitPos = WorldPosition();
            st.waitPosTried = false;
            st.dockPos = edge.staging;
            st.landPos = edge.dest;
            st.dockWaitSinceMs = 0;
            st.phase = FollowTravelPhase::ToDock;
            st.ResetLeg(st.dockPos);
            LOG_DEBUG("playerbots", "[FollowTravel] {}: ferry {} link {} -> {}, dock {:.0f}y away", bot->GetName(),
                      edge.transportEntry, edge.fromRegion, edge.toRegion, toStaging);
            return true;
        }
    }

    return false;
}

bool FollowTravelAction::HopTo(FollowTravelState& st, WorldPosition const& where, char const* why)
{
    if (!where || !sPlayerbotAIConfig.smartTravelTeleportFallback || bot->IsInCombat())
    {
        LOG_DEBUG("playerbots", "[FollowTravel] {}: {} - no hop allowed, giving up", bot->GetName(), why);
        return false;
    }

    LOG_DEBUG("playerbots", "[FollowTravel] {}: {} - hopping to travel point on map {} ({:.0f},{:.0f},{:.0f})",
              bot->GetName(), why, where.GetMapId(), where.GetPositionX(), where.GetPositionY(),
              where.GetPositionZ());

    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bot->TeleportTo(where.GetMapId(), where.GetPositionX(), where.GetPositionY(), where.GetPositionZ(),
                    bot->GetOrientation());
    st.phase = FollowTravelPhase::None;
    return true;
}

bool FollowTravelAction::LegStalled(FollowTravelState& st, WorldPosition const& travelPoint, char const* what)
{
    if (++st.legFails < MAX_LEG_FAILS)
    {
        LOG_DEBUG("playerbots", "[FollowTravel] {}: stalled on the way to the {} - retrying", bot->GetName(), what);
        st.ResetLeg(st.moveFarPos);
        return true;
    }

    st.legFails = 0;

    // Standing next to it already: hopping would only repeat the leg that just failed.
    if (bot->GetMapId() == travelPoint.GetMapId() &&
        bot->GetExactDist2d(travelPoint.GetPositionX(), travelPoint.GetPositionY()) < HOP_POINTLESS_DIS)
    {
        LOG_DEBUG("playerbots", "[FollowTravel] {}: stalled right at the {} - giving up", bot->GetName(), what);
        return false;
    }

    std::string const why = std::string("cannot walk to the ") + what;
    return HopTo(st, travelPoint, why.c_str());
}

bool FollowTravelAction::StepTravel(FollowTravelState& st, WorldPosition const& goal)
{
    uint32 const now = getMSTime();
    st.goal = goal;

    if (st.phase == FollowTravelPhase::None)
    {
        // Clear() wipes the dungeon flags the caller just set; carry them over.
        bool const goalIsDungeon = st.goalIsDungeon;
        WorldPosition const dungeonInside = st.dungeonInside;
        st.Clear();
        st.goal = goal;
        st.planGoal = goal;
        st.goalIsDungeon = goalIsDungeon;
        st.dungeonInside = dungeonInside;
        st.giveUpAtMs = now + GIVE_UP_MS;

        if (bot->IsInFlight())
        {
            st.phase = FollowTravelPhase::InFlight;
            return true;
        }

        uint32 const botRegion = RegionOf(bot);
        uint32 const goalRegion = TravelMgr::GetTravelRegion(goal);
        bool const sameRegion = bot->GetMapId() == goal.GetMapId() && botRegion == goalRegion;
        float const dist = sameRegion ? bot->GetExactDist2d(goal.GetPositionX(), goal.GetPositionY()) : FLT_MAX;

        if (sameRegion && dist < (st.goalIsDungeon ? DUNGEON_STEP_DIS : ARRIVE_DIS))
            return false;

        if (sameRegion)
        {
            // 1. A long way across the same land: fly when a flight really shortens the trip.
            if (dist > TAXI_WORTH_DIS && PlanFlight(st, goal, dist))
                return true;

            // 2. Walk. No distance cap: a goal the navmesh cannot reach (the Blood Furnace door hangs up the
            //    citadel wall) is walked to the closest reachable point first, and only from there may the rest be
            //    hopped.
            st.phase = FollowTravelPhase::FinalApproach;
            st.ResetLeg(goal);
            LOG_DEBUG("playerbots", "[FollowTravel] {}: walk {:.0f}y toward goal on map {}", bot->GetName(), dist,
                      goal.GetMapId());
            return true;
        }

        // 3. Another continent, or a cut-off piece of this one: the next portal / taxi / ferry on the shortest chain
        //    there. This walks a Darnassus bot to the Rut'theran portal, flies it on to Auberdine and ships it across.
        TravelMgr::TravelEdge edge;
        if (sTravelMgr.NextTravelEdge(bot, goal, edge))
        {
            if (PlanLink(st, edge))
                return true;

            LOG_DEBUG("playerbots", "[FollowTravel] {}: link {} -> {} cannot be used - giving up", bot->GetName(),
                      edge.fromRegion, edge.toRegion);
            return false;
        }

        // 4. No chain for this bot at all (a level-gated region, ferries disabled). Hop to the flight master nearest
        //    the goal - a travel point, not the goal itself - and carry on from there.
        LOG_DEBUG("playerbots", "[FollowTravel] {}: no route from region {} (map {} {:.0f},{:.0f}) to region {}",
                  bot->GetName(), botRegion, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), goalRegion);
        TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetFlightMasterNear(goal, bot->GetTeamId());
        return HopTo(st, fm ? fm->pos : goal, "no route to the goal's region");
    }

    // InFlight and the ferry phases run on their own budgets: a taxi ride or a wait at a dock
    // is legitimately minutes long and must not trip the no-progress timer.
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
                return TravelFarTo(st, st.flightMasterPos) || LegStalled(st, st.flightMasterPos, "flight master");

            Creature* flightMaster = bot->FindNearestCreature(st.flightMasterEntry, INTERACTION_DISTANCE * 3.0f);
            if (!flightMaster || !flightMaster->IsAlive())
                return false;

            if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
                return TravelFarTo(st, WorldPosition(flightMaster)) ||
                       LegStalled(st, WorldPosition(flightMaster), "flight master");

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

            LOG_INFO("playerbots",
                     "[FollowTravel] {}: taxi stalled in flight - force-landing at map {} ({:.0f},{:.0f})",
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
            // Attached already (second half of the boarding hop below, or we were aboard).
            if (bot->GetTransport())
            {
                st.deckPos = WorldPosition();
                st.phase = FollowTravelPhase::Aboard;
                st.dockWaitSinceMs = now;
                return true;
            }

            if (bot->GetMapId() != st.dockPos.GetMapId())
            {
                st.phase = FollowTravelPhase::None;
                return true;
            }

            Transport* transport = FindTransport(bot, st.transportEntry);

            // Second half of boarding: we hopped onto the deck last tick. Only attach once the
            // hop has actually been applied - AddPassenger records the offset from our current
            // position, so attaching early glues the bot to the ship at pier coordinates and it
            // gets dragged through the air.
            if (st.deckPos)
            {
                if (bot->IsBeingTeleported())
                    return true;

                if (transport && TravelMgr::IsTransportParked(transport) &&
                    bot->GetExactDist(st.deckPos.GetPositionX(), st.deckPos.GetPositionY(),
                                      st.deckPos.GetPositionZ()) < 4.0f)
                {
                    transport->AddPassenger(bot, true);
                    bot->StopMovingOnCurrentPos();
                    st.deckPos = WorldPosition();
                    st.waitPos = WorldPosition();
                    st.phase = FollowTravelPhase::Aboard;
                    st.dockWaitSinceMs = now;
                    LOG_DEBUG("playerbots", "[FollowTravel] {}: aboard ferry {}", bot->GetName(), st.transportEntry);
                    return true;
                }

                // The ship left between hop and attach: step back onto our spot on the pier (the dock
                // stop itself is over the water) and wait for the next one.
                st.deckPos = WorldPosition();
                WorldPosition const& back = st.waitPos ? st.waitPos : st.dockPos;
                if (back)
                    bot->NearTeleportTo(back.GetPositionX(), back.GetPositionY(), back.GetPositionZ(),
                                        bot->GetOrientation());
                return true;
            }

            // Once in the harbour, claim a personal spot on the pier so a group of bots waiting for
            // the same ship stands side by side instead of stacking on the dock key frame.
            if (!st.waitPosTried &&
                bot->GetExactDist2d(st.dockPos.GetPositionX(), st.dockPos.GetPositionY()) < DOCK_WAIT_PICK_DIS)
            {
                st.waitPosTried = true;
                sTravelMgr.FindDockWaitSpot(bot, st.dockPos, st.waitPos);
            }

            if (st.waitPos)
            {
                if (bot->GetExactDist2d(st.waitPos.GetPositionX(), st.waitPos.GetPositionY()) > DOCK_WAIT_SPOT_DIS)
                {
                    MoveTo(st.waitPos.GetMapId(), st.waitPos.GetPositionX(), st.waitPos.GetPositionY(),
                           st.waitPos.GetPositionZ(), false, false, false, true);
                    return true;
                }
            }
            else if (bot->GetExactDist2d(st.dockPos.GetPositionX(), st.dockPos.GetPositionY()) > DOCK_ARRIVE_DIS)
            {
                if (TravelFarTo(st, st.dockPos))
                    return true;

                // The dock key frame itself is out over the water - a hop to it would drop the bot into the sea.
                // Only dry ground on the pier beside it will do; without that, give up and try again later.
                WorldPosition pier;
                if (!sTravelMgr.FindDockWaitSpot(bot, st.dockPos, pier))
                {
                    LOG_DEBUG("playerbots", "[FollowTravel] {}: cannot reach ferry dock and found no pier - giving up",
                              bot->GetName());
                    return false;
                }

                return LegStalled(st, pier, "ferry dock");
            }

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

            // Wait - do not walk anywhere - until the ship is parked right at this pier.
            if (!transport || !TravelMgr::IsTransportParked(transport) ||
                bot->GetExactDist2d(transport->GetPositionX(), transport->GetPositionY()) > DOCK_BOARD_DIS)
                return true;

            // Step aboard the way a portal is stepped through; walking onto a hull the navmesh
            // cannot see is what sent bots through the planks and back and forth on the pier.
            WorldPosition deck;
            if (!sTravelMgr.FindDeckSpot(transport, bot, deck))
                return true;

            st.deckPos = deck;
            bot->NearTeleportTo(deck.GetPositionX(), deck.GetPositionY(), deck.GetPositionZ(), bot->GetOrientation());
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
                WorldPosition pier;
                bool const havePier = sTravelMgr.FindPierSpot(transport, bot, st.landPos, pier);
                TravelMgr::DetachFromTransport(bot);
                if (havePier)
                    bot->NearTeleportTo(pier.GetPositionX(), pier.GetPositionY(), pier.GetPositionZ(),
                                        bot->GetOrientation());
                st.dockWaitSinceMs = 0;
                st.phase = FollowTravelPhase::None;
                return true;
            }

            // The core carries passengers across the map boundary. Get off only while the ship is
            // parked at the destination pier - judged by where WE are (on the deck), which is far
            // more reliable than the hull origin versus the dock key frame.
            if (bot->GetMapId() != st.landPos.GetMapId() || !TravelMgr::IsTransportParked(transport) ||
                bot->GetExactDist2d(st.landPos.GetPositionX(), st.landPos.GetPositionY()) > DOCK_LAND_DIS)
                return true;

            WorldPosition pier;
            if (!sTravelMgr.FindPierSpot(transport, bot, st.landPos, pier))
                return true;  // keep riding; try again next tick while it is still parked

            // Detach first - fully, or the bot keeps a dangling link to the ship - then hop ashore. Never walk off.
            TravelMgr::DetachFromTransport(bot);
            bot->NearTeleportTo(pier.GetPositionX(), pier.GetPositionY(), pier.GetPositionZ(), bot->GetOrientation());
            LOG_DEBUG("playerbots", "[FollowTravel] {}: left ferry {} at map {}", bot->GetName(), st.transportEntry,
                      bot->GetMapId());
            st.dockWaitSinceMs = 0;
            st.phase = FollowTravelPhase::None;
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
                return TravelFarTo(st, st.portalStaging) || LegStalled(st, st.portalStaging, "portal");

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

            // Dungeon goals are walked right onto the portal (Execute steps inside); for a master
            // plain follow closes the last few yards.
            if (bot->GetExactDist2d(st.goal.GetPositionX(), st.goal.GetPositionY()) <
                (st.goalIsDungeon ? DUNGEON_STEP_DIS : ARRIVE_DIS))
                return false;

            WorldPosition const target = st.approachPos ? st.approachPos : st.goal;
            bool const onApproach =
                st.approachPos && bot->GetExactDist(st.approachPos.GetPositionX(), st.approachPos.GetPositionY(),
                                                    st.approachPos.GetPositionZ()) <= APPROACH_AT_DIS;

            if (!onApproach && TravelFarTo(st, target))
                return true;

            // Dead end: the leg stopped making progress, or we reached the closest walkable point.
            float const gap =
                bot->GetExactDist(st.goal.GetPositionX(), st.goal.GetPositionY(), st.goal.GetPositionZ());

            // Far from the goal: never hop onto it from here.
            if (gap > APPROACH_SEARCH_DIS)
            {
                if (st.approachTries++ < MAX_APPROACH_TRIES)
                {
                    st.approachPos = WorldPosition();
                    st.ResetLeg(st.goal);
                    return true;
                }

                // A flight may get round what the road cannot.
                if (gap > TAXI_WORTH_DIS && PlanFlight(st, st.goal, gap))
                    return true;

                // Else the flight master nearest the goal, when that is really nearer - a travel point to walk on
                // from, not the goal.
                TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetFlightMasterNear(st.goal, bot->GetTeamId());
                if (fm && st.goal.GetExactDist(fm->pos.GetPositionX(), fm->pos.GetPositionY(),
                                               fm->pos.GetPositionZ()) + HOP_MIN_GAIN < gap)
                    return HopTo(st, fm->pos, "stuck far from the goal");

                LOG_DEBUG("playerbots", "[FollowTravel] {}: stuck {:.0f}y from goal - giving up, no teleport",
                          bot->GetName(), gap);
                st.phase = FollowTravelPhase::None;
                return false;
            }

            // Near the goal: is there a walkable spot closer than where we stand?
            WorldPosition closest;
            bool const found = FindApproachPoint(bot, st.goal, closest);
            float const closestGap =
                found ? st.goal.GetExactDist(closest.GetPositionX(), closest.GetPositionY(), closest.GetPositionZ())
                      : gap;
            float const toClosest =
                found ? bot->GetExactDist(closest.GetPositionX(), closest.GetPositionY(), closest.GetPositionZ())
                      : 0.0f;

            // Standing on it already, or it is no nearer the goal than we are.
            bool const atClosest = !found || toClosest <= APPROACH_AT_DIS || closestGap + APPROACH_AT_DIS >= gap;

            if (!atClosest && st.approachTries++ < MAX_APPROACH_TRIES)
            {
                st.approachPos = closest;
                st.ResetLeg(closest);
                LOG_DEBUG("playerbots", "[FollowTravel] {}: walking to closest reachable point {:.0f}y from goal",
                          bot->GetName(), closestGap);
                return true;
            }

            // As close as the navmesh lets us get. Only now, and only for a short remainder, hop it.
            if (atClosest && gap <= sPlayerbotAIConfig.smartTravelTeleportNearDist)
            {
                if (!HopTo(st, st.goal, "at the closest reachable point to the goal"))
                {
                    st.phase = FollowTravelPhase::None;
                    return false;
                }
                return true;
            }

            LOG_DEBUG("playerbots", "[FollowTravel] {}: cannot get closer than {:.0f}y - giving up, no teleport",
                      bot->GetName(), gap);
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
    {
        st.ResetLeg(dest);
        PreloadRouteGrids(bot, dest);
    }

    uint32 const now = getMSTime();
    LastMovement& lm = AI_VALUE(LastMovement&, "last movement");

    // Detours count against the bot only while they are recent.
    if (st.detours && now > st.detoursUntilMs)
        st.detours = 0;

    // A move is under way: the wait the last MoveTo asked for, or - that wait is capped at a few seconds - a spline
    // still running toward its point on this map. Let it finish before recomputing (anti-oscillation, as in
    // NewRpgBaseAction::MoveFarTo)...
    bool underWay = IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL);
    if (!underWay && bot->isMoving() && lm.lastMoveToMapId == bot->GetMapId())
        underWay = bot->GetExactDist(lm.lastMoveToX, lm.lastMoveToY, lm.lastMoveToZ) > 10.0f;

    // ...unless a hostile stands on what is left of it. A step is 90 yards of road: one that was clear when the bot
    // set out is not checked again for the next ten seconds otherwise, and whatever shows up on it meanwhile - the
    // far half of the step was out of sight, a patrol walked in - is run straight into.
    std::vector<Threat> threats;
    bool rerouting = false;
    if (underWay)
    {
        if (!sPlayerbotAIConfig.smartTravelAvoidEnemies || !bot->isMoving() || st.detours >= AVOID_MAX_DETOURS ||
            lm.priority > MovementPriority::MOVEMENT_NORMAL || now < st.nextAvoidScanMs)
            return true;

        st.nextAvoidScanMs = now + AVOID_SCAN_MS;
        CollectThreats(botAI, bot, dest, threats);

        Movement::PointsArray rest;
        if (threats.empty() || !CommittedPath(bot, rest) || AssessPath(rest, AVOID_LOOKAHEAD, threats).blocker < 0)
            return true;

        rerouting = true;
    }
    else if (st.detours < AVOID_MAX_DETOURS)
        CollectThreats(botAI, bot, dest, threats);

    // Swimming for a long stretch means the leg is crossing open water the navmesh cannot solve - that is how a bot
    // ends up swimming a lake or an ocean under the terrain. Report a stall so the caller can retry or hop.
    if (bot->IsInWater())
    {
        if (st.swimSinceMs == 0)
            st.swimSinceMs = now;
        else if (now - st.swimSinceMs > MAX_SWIM_MS)
        {
            LOG_DEBUG("playerbots", "[FollowTravel] {}: swimming too long toward ({:.0f},{:.0f}) - leg stalled",
                      bot->GetName(), dest.GetPositionX(), dest.GetPositionY());
            return false;
        }
    }
    else
        st.swimSinceMs = 0;

    float const distToDest =
        bot->GetExactDist(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());

    // The last search found nothing walkable: wait a moment before burning another fan of path queries, but keep
    // the leg's own clock running so a bot that never finds a step still gives up.
    if (st.nextStepSearchMs && now < st.nextStepSearchMs)
    {
        if (++st.stuckAttempts >= 5 && st.stuckSinceMs != 0 && GetMSTimeDiffToNow(st.stuckSinceMs) >= LEG_STUCK_MS)
            return false;

        return true;
    }

    // Pick the next step first: whether it follows a complete route decides how progress is measured. On a route
    // that is the route's remaining length - a real way round an obstacle leads away from the target for a while
    // and must not read as "going nowhere" - otherwise the straight-line distance.
    WorldPosition step;
    bool onRoute = false;
    float routeLength = 0.0f;
    Movement::PointsArray leg;
    bool const haveStep =
        distToDest >= PATHFINDER_DIS && NextRouteStep(bot, dest, step, onRoute, routeLength, leg);
    bool const routeMetric = haveStep && onRoute;
    float const progress = routeMetric ? routeLength : distToDest;

    if (routeMetric != st.progressIsRoute)
    {
        // The two metrics are not comparable: start measuring again, without crediting this tick as progress.
        st.progressIsRoute = routeMetric;
        st.nearestMoveFarDis = progress;
    }
    else if (progress + 5.0f < st.nearestMoveFarDis)
    {
        // Nearer than it has been on this leg (the first reading of a leg is not that).
        if (st.nearestMoveFarDis < FLT_MAX)
            st.detours = 0;

        st.nearestMoveFarDis = progress;
        st.stuckSinceMs = now;
        st.stuckAttempts = 0;
        st.giveUpAtMs = now + GIVE_UP_MS;  // real progress keeps the plan alive, however long the road
    }
    else if (++st.stuckAttempts >= 5 && st.stuckSinceMs != 0 && GetMSTimeDiffToNow(st.stuckSinceMs) >= LEG_STUCK_MS)
    {
        return false;  // no meaningful progress - the leg stalled
    }

    // A guess that carries the bot this far off is a dead end; a complete route is trusted wherever it leads.
    if (!routeMetric && !st.progressIsRoute && st.nearestMoveFarDis < FLT_MAX &&
        distToDest > st.nearestMoveFarDis + REGRESS_DIS)
    {
        LOG_DEBUG("playerbots", "[FollowTravel] {}: drifted {:.0f}y away from ({:.0f},{:.0f}) - leg stalled",
                  bot->GetName(), distToDest - st.nearestMoveFarDis, dest.GetPositionX(), dest.GetPositionY());
        return false;
    }

    // Where to walk now: straight to the target when it is near, else the next step of the route.
    WorldPosition aim;
    if (distToDest < PATHFINDER_DIS)
    {
        aim = dest;
        leg.clear();
        if (!threats.empty())
        {
            PathGenerator path(bot);
            CalculateRoute(path, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
            if (PathWalkable(path))
                leg = path.GetPath();
        }

        if (leg.size() < 2)
        {
            leg.clear();
            leg.emplace_back(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            leg.emplace_back(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        }
    }
    else if (!haveStep)
    {
        st.nextStepSearchMs = now + STEP_SEARCH_RETRY_MS;
        return true;  // nothing usable this tick, but not declared stuck yet
    }
    else
    {
        st.nextStepSearchMs = 0;
        aim = step;
    }

    // Hostiles on that stretch: go round them.
    WorldPosition target = aim;
    bool noWayRound = false;
    if (!threats.empty())
    {
        PathRisk const risk = AssessPath(leg, AVOID_LOOKAHEAD, threats);
        if (risk.blocker >= 0)
        {
            WorldPosition detour;
            bool clear = false;
            if (FindDetour(bot, threats, risk, aim, detour, clear))
            {
                target = detour;

                // A detour gains nothing on the target for a while, by design: it must not read as a stalled leg.
                // Only so many in a row, though. A bot that has gone round and round and is no nearer stands before
                // something it cannot round - the only door, a bridge - and from then on it takes the road as it
                // is, until it gets nearer again or a while has passed.
                ++st.detours;
                st.detoursUntilMs = now + AVOID_DETOUR_MEMORY_MS;
                st.stuckSinceMs = now;
                st.stuckAttempts = 0;

                LOG_DEBUG("playerbots",
                          "[FollowTravel] {}: {} hostile(s) near, one {:.0f}y into the way - {} via ({:.0f},{:.0f})",
                          bot->GetName(), threats.size(), risk.penetration, clear ? "going round" : "squeezing past",
                          detour.GetPositionX(), detour.GetPositionY());
            }
            else
            {
                noWayRound = true;
                LOG_DEBUG("playerbots",
                          "[FollowTravel] {}: {} hostile(s) near, one {:.0f}y into the way - no way round",
                          bot->GetName(), threats.size(), risk.penetration);
            }

            // The path taken clips a radius knowingly: looking at it again every scan would only find the same.
            if (!clear)
                st.nextAvoidScanMs = now + AVOID_RETRY_MS;
        }
    }

    if (rerouting)
    {
        // Nothing better than the move already under way.
        if (noWayRound)
            return true;

        lm.lastdelayTime = 0.0f;  // that move is still being waited for, and MoveTo would refuse to replace it
    }

    // A refused move (rooted, mid-cast, the same point twice) is not a stall - the timer above decides that.
    MoveTo(bot->GetMapId(), target.GetPositionX(), target.GetPositionY(), target.GetPositionZ(), false, false, false,
           true);
    return true;
}
