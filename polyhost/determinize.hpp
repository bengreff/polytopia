#pragma once
// Fog-safe copies of a match for search (docs/design/architecture.md, Layer 0: the determinized-state
// builder; research log S9). A planner that simulates turns on the true state would see what the
// player can't: tiles it never explored, ruins' rewards, invisible units, the opponent's stars and
// techs. Determinizer::build writes into `dst` one sample of the hidden part, consistent with what
// `viewer`'s client shows, so a search run in `dst` uses only the viewer's information:
//
// - the command history is emptied (and the last command's actions unless they are the viewer's;
//   stripHistory), and the random seed is replaced by the sample's
//   (the game's XXHash is rebuilt from it on load), so ruins, rewards and every later draw differ;
// - enemy units the viewer can't see are removed: invisible ones on explored tiles, and every unit
//   on a tile the viewer hasn't explored (the sample's belief: the fog holds no units);
// - tiles the viewer hasn't explored take their terrain, climate, resource and unowned
//   improvements (villages, ruins, ...) from a map generated with the same settings and tribes and
//   the sample's seed, with roads, routes and effects cleared. Hidden tiles in enemy territory keep
//   their owner (the territory is consistent with the city) but lose their improvements;
// - KNOWN LEAK (v1): enemy cities on unexplored tiles are kept where they are, with their level,
//   since the game needs a living player's cities; their positions are therefore not resampled;
// - the opponent's techs are what the viewer can infer: its tribe's starting techs, every tech that
//   unlocks a unit type of theirs or an improvement they built (ImprovementState.founder) that the
//   viewer sees, and those techs' prerequisites (units they may have without the tech don't count:
//   rewardUnit, the starting unit, and units born in another tribe, i.e. converted);
// - the opponent's banked stars are a prior (kBankedStars, the human mean); its income arrives at
//   the start of its turn as usual;
// - enemy units' xp and creation turn (the client shows neither) are set to 0;
// - all units are renumbered in tile order (the ids would count the units the opponent trained),
//   and the state goes through the game's serializer once, so every cache is rebuilt from it.
//
// Same viewer observation + same sample seed -> byte-identical output, whatever else differs
// (envtest::deterTest checks it by perturbing the hidden part). Not yet sampled (later layers): units
// in the fog, the opponent's banked stars beyond one turn of income, enemy unit home cities (kept).

#include <random>
#include <set>

#include "env.hpp"

namespace env {

struct Determinizer {
    Api& api;
    Game& g;
    Class* techData = cls("GameLogicAssembly", "Polytopia.Data", "TechData");
    Class* unitData = cls("GameLogicAssembly", "Polytopia.Data", "UnitData");
    Class* impData = cls("GameLogicAssembly", "Polytopia.Data", "ImprovementData");
    Class* tribeData = cls("GameLogicAssembly", "Polytopia.Data", "TribeData");
    Class* gld = cls("GameLogicAssembly", "Polytopia.Data", "GameLogicData");
    Field* fImprovement = field(api.tile, "improvement");
    Field* fResource = field(api.tile, "resource");
    Field* fUnit = field(api.tile, "<unit>k__BackingField");
    size_t tHasRoad = off(api.tile, "hasRoad"), tHadRoute = off(api.tile, "hadRoute");
    size_t techIdx = off(techData, "idx"), techChildren = off(techData, "techUnlocks");
    size_t techUnits = off(techData, "unitUnlocks"), techImps = off(techData, "improvementUnlocks");
    size_t unitIdx = off(unitData, "idx"), impIdx = off(impData, "idx"), tribeStartTech = off(tribeData, "startingTech");
    size_t iFounder = off(api.improvement, "founder"), tribeStartUnit = off(tribeData, "startingUnit");
    size_t uBirthClimate = off(api.unit, "birthClimate"), pClimate = off(g.player, "_climate");
    const Method* tryTribe = methodWith(gld, "TryGetData", 2, "TribeData");
    const Method* allTech = methodWith(gld, "GetAllTechForTribe", 1, "TribeData");
    const Method* setCurrency = method(g.player, "set_Currency", 1);
    int idOffset = -1;  // CurrentUnitId minus the highest unit id in a new game (found once)
    static constexpr int kCity = 1;  // ImprovementData.Type.City (villages are cities on unowned tiles)
    // The opponent's banked stars in a sample: the human championship games' mean at End Turn, turns
    // 3-20 (docs/strategy Part 0). Its income still arrives at the start of its turn.
    static constexpr int kBankedStars = 3;

    explicit Determinizer(Api& a) : api(a), g(a.g) {}

    struct Report {
        int hiddenUnits = 0;  // removed: invisible enemy units on explored tiles
        int fogUnits = 0;     // removed: units on unexplored tiles
        int fogTiles = 0;     // unexplored tiles resampled
        int keptCities = 0;   // enemy cities on unexplored tiles (the known leak)
        int techs = 0, trueTechs = 0, wrongTechs = 0, stars = 0, trueStars = 0;  // wrongTechs: inferred but not held
    };

    static void clearList(Object* l) {
        if (l) call(method(object_get_class(l), "Clear", 0), l);
    }

    // Empties the command history. The action stack isn't a history: it holds the actions of the last
    // command only (EndCommandAction, RunDeferredAction, score changes...), and a pending city
    // level-up reward reads it for its score (envtest::checkHistory: with it cleared the score
    // differed by ~40 in 1% of states). It is kept when all its actions are `viewer`'s own, and
    // cleared otherwise (the opponent's last command's effects could show what the viewer can't see).
    static void stripHistory(Game& g, Object* gs, uint8_t viewer) {
        clearList(get<Object*>(gs, "<CommandStack>k__BackingField"));
        uint16_t zero = 0;
        field_set_value(gs, field(g.gameState, "<CurrentCommand>k__BackingField"), &zero);
        Object* actions = get<Object*>(gs, "<ActionStack>k__BackingField");
        static size_t actionPlayer = off(cls("GameLogicAssembly", "", "ActionBase"), "<PlayerId>k__BackingField");
        for (int i = 0, n = listCount(actions); i < n; ++i)
            if (at<uint8_t>(listAt<Object*>(actions, i), actionPlayer) != viewer) {
                clearList(actions);
                return;
            }
    }

    Object* gameData(Object* gs) { return call(method(g.gameState, "get_GameLogicData", 0), gs); }

    // The techs `viewer` can infer `opp` has, as sorted TechData.Type values.
    std::map<int, std::string> lastCause;  // tech -> the evidence that implied it (for the tests)

    // Units a player can field without the tech that unlocks them: ruins give swordsmen (and ships on
    // water), city level-ups give giants; a tribe starts with its starting unit; a mind bender converts
    // the other side's units (then only the mind bender itself counts).
    static constexpr int kSwordsman = 11, kGiant = 12, kMindBender = 10;
    static bool rewardUnit(int u) { return u == kSwordsman || u == kGiant || u == 6 || u == 7 || u == 14 || (u >= 41 && u <= 44); }

    std::vector<int> inferTechs(Match& m, uint8_t opp, uint8_t viewer, const std::set<int>& seenUnits, const std::set<int>& imps) {
        lastCause.clear();
        Object* data = gameData(m.state());
        int tribe = at<int32_t>(m.player(opp), api.pTribe);
        Object* td = nullptr;
        if (!unbox<bool>(call(tryTribe, data, {&tribe, &td})) || !td) die("no TribeData for tribe " + std::to_string(tribe));
        Object* su = at<Object*>(td, tribeStartUnit);
        int startUnit = su ? at<int32_t>(su, unitIdx) : -1;
        std::set<int> units;
        for (int u : seenUnits)
            if (!rewardUnit(u) && u != startUnit && (!seenUnits.count(kMindBender) || u == kMindBender)) units.insert(u);
        std::set<int> known, starting;
        Object* start = at<Object*>(td, tribeStartTech);
        for (int i = 0, n = listCount(start); i < n; ++i) starting.insert(at<int32_t>(listAt<Object*>(start, i), techIdx));
        known = starting;
        Object* all = call(allTech, data, {td});
        std::map<int, int> parent;
        for (int i = 0, n = listCount(all); i < n; ++i) {
            Object* t = listAt<Object*>(all, i);
            int idx = at<int32_t>(t, techIdx);
            Object* kids = at<Object*>(t, techChildren);
            for (int k = 0, nk = listCount(kids); k < nk; ++k) parent[at<int32_t>(listAt<Object*>(kids, k), techIdx)] = idx;
            Object* us = at<Object*>(t, techUnits);
            for (int k = 0, nk = listCount(us); k < nk; ++k) {
                int u = at<int32_t>(listAt<Object*>(us, k), unitIdx);
                if (units.count(u) && known.insert(idx).second) lastCause[idx] = "unit " + std::to_string(u);
            }
            Object* is = at<Object*>(t, techImps);
            for (int k = 0, nk = listCount(is); k < nk; ++k) {
                int im = at<int32_t>(listAt<Object*>(is, k), impIdx);
                if (imps.count(im) && known.insert(idx).second) lastCause[idx] = "improvement " + std::to_string(im);
            }
        }
        std::vector<int> todo;
        for (int t : known)
            if (!starting.count(t)) todo.push_back(t);
        while (!todo.empty()) {  // a researched tech's prerequisites (a starting tech comes without them)
            int t = todo.back();
            todo.pop_back();
            auto it = parent.find(t);
            if (it != parent.end() && known.insert(it->second).second) {
                if (!starting.count(it->second)) todo.push_back(it->second);
                lastCause[it->second] = "prerequisite of " + std::to_string(t) + " (" + lastCause[t] + ")";
            }
        }
        return std::vector<int>(known.begin(), known.end());
    }

    uint32_t maxUnitId(Match& m) {
        uint32_t top = 0;
        for (Object* t : m.tiles())
            for (Object* u = at<Object*>(t, api.tUnit); u; u = at<Object*>(u, api.uPassenger)) top = std::max(top, at<uint32_t>(u, api.uId));
        return top;
    }

    // dst becomes the determinized copy of src for `viewer` (a player id), with the hidden part drawn
    // from `seed`.
    Report build(Match& src, Match& dst, uint8_t viewer, uint64_t seed) {
        Report rep;
        dst.load(src.spec, src.save());
        Object* gs = dst.state();

        // The sample's map: the same settings and tribes, the sample's seed, no bot seats.
        Match fresh(api);
        Spec fs = src.spec;
        fs.seats[0].bot = fs.seats[1].bot = -1;
        fs.seed = seed;
        fresh.create(fs);
        Field* fUnitId = field(g.gameState, "<CurrentUnitId>k__BackingField");
        if (idOffset < 0) idOffset = int(at<uint32_t>(fresh.state(), field_get_offset(fUnitId))) - int(maxUnitId(fresh));
        std::vector<Object*> ftiles = fresh.tiles();
        if (ftiles.size() != size_t(dst.width) * dst.height) die("determinize: the sample map has another size");

        // Tiles: what the viewer sees stays; the rest comes from the sample's map.
        bool no = false;
        std::map<uint8_t, std::set<int>> oppUnits, oppImps;
        for (Object* t : dst.tiles()) {
            Coord c = at<Coord>(t, api.tCoords);
            Object* ft = ftiles[dst.index(c)];
            Object* u = at<Object*>(t, api.tUnit);
            Object* imp = at<Object*>(t, api.tImprovement);
            if (dst.exploredBy(t, viewer)) {
                if (u && at<uint8_t>(u, api.uOwner) != viewer) {
                    Object* seen = call(api.visibleUnit, t, {gs, &viewer, &no});
                    if (seen != u) {
                        field_set_value(t, fUnit, nullptr);
                        ++rep.hiddenUnits;
                    } else {  // evidence: units it trained itself (a converted unit keeps its birth tribe's look)
                        uint8_t o = at<uint8_t>(u, api.uOwner);
                        int climate = at<int32_t>(dst.player(o), pClimate);
                        for (Object* x = u; x; x = at<Object*>(x, api.uPassenger))
                            if (at<int32_t>(x, uBirthClimate) == climate) oppUnits[o].insert(at<int32_t>(x, api.uType));
                    }
                }
                uint8_t founder = imp ? at<uint8_t>(imp, iFounder) : 0;  // who built it (0: a city, or nature's)
                if (founder != viewer && founder != 0 && founder != 255) oppImps[founder].insert(at<int32_t>(imp, api.iType));
                continue;
            }
            if (u) {
                field_set_value(t, fUnit, nullptr);
                ++rep.fogUnits;
            }
            uint8_t owner = at<uint8_t>(t, api.tOwner);
            bool enemyCity = imp && at<int32_t>(imp, api.iType) == kCity && owner != 0 && owner != viewer;
            if (enemyCity) {  // the known leak: kept as it is
                ++rep.keptCities;
                continue;
            }
            ++rep.fogTiles;
            put<int32_t>(t, api.tTerrain, at<int32_t>(ft, api.tTerrain));
            put<int32_t>(t, api.tClimate, at<int32_t>(ft, api.tClimate));
            field_set_value(t, fResource, at<Object*>(ft, api.tResource));
            Object* fimp = at<Object*>(ft, api.tImprovement);
            bool unowned = fimp && at<uint8_t>(ft, api.tOwner) == 0;  // villages, ruins, ...; not the sample's capitals
            field_set_value(t, fImprovement, owner == 0 && unowned ? fimp : nullptr);
            clearList(at<Object*>(t, api.tEffects));
            put<bool>(t, tHasRoad, false);
            put<bool>(t, api.tRoute, false);
            put<bool>(t, tHadRoute, false);
            if (owner == viewer || owner == 0) {  // (the viewer's own territory is always explored)
                put<uint8_t>(t, api.tOwner, 0);
                put<uint8_t>(t, api.tCapitalOf, 0);
                put<Coord>(t, api.tRuling, at<Coord>(ft, api.tRuling));
            }
        }

        // Enemy units: what the client doesn't show about them.
        for (Object* t : dst.tiles())
            for (Object* u = at<Object*>(t, api.tUnit); u; u = at<Object*>(u, api.uPassenger))
                if (at<uint8_t>(u, api.uOwner) != viewer) {
                    put<uint16_t>(u, api.uXp, 0);
                    put<uint16_t>(u, api.uCreated, 0);
                }

        // The opponents' techs and stars.
        Object* ps = dst.players();
        for (int i = 0, n = listCount(ps); i < n; ++i) {
            Object* p = listAt<Object*>(ps, i);
            uint8_t id = at<uint8_t>(p, api.pId);
            if (id == viewer || id == 255) continue;
            Object* techs = at<Object*>(p, api.pTech);
            rep.trueTechs += listCount(techs);
            std::set<int> held;
            for (int k = 0, nk = listCount(techs); k < nk; ++k) held.insert(listAt<int32_t>(techs, k));
            std::vector<int> inferred = inferTechs(dst, id, viewer, oppUnits[id], oppImps[id]);
            for (int t : inferred)
                if (!held.count(t)) {
                    ++rep.wrongTechs;
                    if (std::getenv("DETER_DEBUG")) std::printf("    inferred tech %d not held (tribe %d): %s\n", t,
                                                                at<int32_t>(p, api.pTribe), lastCause[t].c_str());
                }
            clearList(techs);
            const Method* add = method(object_get_class(techs), "Add", 1);
            for (int t : inferred) call(add, techs, {&t});
            rep.techs += int(inferred.size());
        }
        for (int i = 0, n = listCount(ps); i < n; ++i) {
            Object* p = listAt<Object*>(ps, i);
            uint8_t id = at<uint8_t>(p, api.pId);
            if (id == viewer || id == 255) continue;
            rep.trueStars += unbox<int>(call(api.getCurrency, p));
            int stars = kBankedStars;
            call(setCurrency, p, {&stars});
            rep.stars += stars;
        }

        // The built-in AI's memory (missions, targets: unit references, hidden plans): replanned from
        // the sample the next time the AI moves a seat.
        size_t ai = off(g.player, "aiState");
        Class* aiCls = cls("GameLogicAssembly", "", "AIState");
        size_t aiTurn = off(aiCls, "missionsForTurn"), aiMissions = off(aiCls, "missions"), aiHits = off(aiCls, "hitList");
        size_t aiBlocked = off(aiCls, "blockedTiles"), aiUpgrade = off(aiCls, "upgradeableCities");
        for (int i = 0, n = listCount(ps); i < n; ++i) {  // (AIState is a struct inside PlayerState: offsets from its start)
            char* st = reinterpret_cast<char*>(listAt<Object*>(ps, i)) + ai - 0x10;  // value-type field offsets count the object header
            *reinterpret_cast<uint32_t*>(st + aiTurn) = 0xffffffffu;
            for (size_t o : {aiMissions, aiHits, aiBlocked, aiUpgrade}) clearList(*reinterpret_cast<Object**>(st + o));
        }

        // Unit ids, history, randomness.
        uint32_t k = 0;
        for (Object* t : dst.tiles())
            for (Object* u = at<Object*>(t, api.tUnit); u; u = at<Object*>(u, api.uPassenger)) put<uint32_t>(u, api.uId, ++k);
        uint32_t next = uint32_t(int(k) + idOffset);
        field_set_value(gs, fUnitId, &next);
        stripHistory(g, gs, viewer);
        std::seed_seq sq{uint32_t(seed), uint32_t(seed >> 32), 0x5eedu};
        uint32_t s32;
        sq.generate(&s32, &s32 + 1);
        int32_t newSeed = int32_t(s32 & 0x7fffffff);
        field_set_value(gs, field(g.gameState, "<Seed>k__BackingField"), &newSeed);

        // Through the game's serializer: fresh objects, caches rebuilt, the random hash from the new seed.
        std::string bytes = dst.save();
        dst.load(src.spec, bytes);
        return rep;
    }
};

}  // namespace env
