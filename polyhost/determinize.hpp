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
// - enemy cities the viewer can't locate (the city and all its territory unexplored) move to a city
//   site of the sample's map (relocateHiddenCities; until 2026-11-24 they were kept in place, a leak).
//   Their number and levels still come from the truth;
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
    size_t pStartTile = off(g.player, "startTile");
    const Method* tryTribe = methodWith(gld, "TryGetData", 2, "TribeData");
    const Method* allTech = methodWith(gld, "GetAllTechForTribe", 1, "TribeData");
    const Method* setCurrency = method(g.player, "set_Currency", 1);
    int idOffset = -1;  // CurrentUnitId minus the highest unit id in a new game (found once)
    static constexpr int kCity = 1;  // ImprovementData.Type.City (villages are cities on unowned tiles)
    static constexpr int kField = 3, kForest = 5;  // TerrainData.Type
    // The opponent's banked stars in a sample: the human championship games' mean at End Turn, turns
    // 3-20 (docs/strategy Part 0). Its income still arrives at the start of its turn.
    static constexpr int kBankedStars = 3;

    explicit Determinizer(Api& a) : api(a), g(a.g) {}

    struct Report {
        int hiddenUnits = 0;  // removed: invisible enemy units on explored tiles
        int fogUnits = 0;     // removed: units on unexplored tiles
        int fogTiles = 0;     // unexplored tiles resampled
        int keptCities = 0;   // hidden enemy cities left in place: no free site in the sample (the leak, now rare)
        int movedCities = 0;  // hidden enemy cities moved to a site of the sample's map (relocateHiddenCities)
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

    // Hidden enemy cities (planner v3, 2026-11-24; v1 kept them in place: the known leak). An enemy city
    // whose tile and every tile of its territory `viewer` has never explored moves to a city site of the
    // sample's map: its owner's capital site there for a capital, else a village site, else any field or
    // forest tile, on land that nobody owns once the hidden cities are lifted, with no city within 1 tile. Its territory
    // there is every unexplored, unowned tile within its border size. Visible units whose home it was
    // follow it. A city with an explored territory tile stays: the viewer sees that tile's ruling-city
    // coordinates, so it knows where the city is. Still taken from the truth: how many hidden cities
    // there are, and their levels. A hidden city with no free site stays (rep.keptCities).
    void relocateHiddenCities(Match& dst, const std::vector<Object*>& ftiles, uint8_t viewer, uint64_t seed, Report& rep) {
        const int W = dst.width, H = dst.height, N = W * H;
        std::vector<Object*> ts(N, nullptr);
        for (Object* t : dst.tiles()) {
            int i = dst.index(at<Coord>(t, api.tCoords));
            if (i >= 0) ts[i] = t;
        }
        struct City { int i; uint8_t owner; bool capital; int border; std::vector<int> terr; bool known; Object* imp; };
        std::vector<City> cs;
        std::map<int, int> where;  // city tile index -> position in cs
        for (int i = 0; i < N; ++i) {
            Object* t = ts[i];
            Object* imp = t ? at<Object*>(t, api.tImprovement) : nullptr;
            uint8_t o = t ? at<uint8_t>(t, api.tOwner) : 0;
            if (!imp || at<int32_t>(imp, api.iType) != kCity || o == 0 || o == viewer) continue;
            where[i] = int(cs.size());
            cs.push_back({i, o, at<uint8_t>(t, api.tCapitalOf) == o, std::max(1, int(at<uint16_t>(imp, api.iBorder))), {}, dst.exploredBy(t, viewer), imp});
        }
        for (int i = 0; i < N; ++i) {
            if (!ts[i]) continue;
            int r = dst.index(at<Coord>(ts[i], api.tRuling));
            auto it = r >= 0 ? where.find(r) : where.end();
            if (it == where.end()) continue;
            City& c = cs[it->second];
            if (at<uint8_t>(ts[i], api.tOwner) != c.owner) continue;
            if (i != c.i) c.terr.push_back(i);
            if (dst.exploredBy(ts[i], viewer)) c.known = true;
        }
        std::vector<int> hidden;
        for (int k = 0; k < int(cs.size()); ++k)
            if (!cs[k].known) hidden.push_back(k);
        if (hidden.empty()) return;
        struct Orig { int32_t terrain, climate; Object* resource; };
        std::map<int, Orig> orig;  // the city tile as it was, for a city that stays
        for (int k : hidden) {
            Object* t = ts[cs[k].i];
            orig[cs[k].i] = {at<int32_t>(t, api.tTerrain), at<int32_t>(t, api.tClimate), at<Object*>(t, api.tResource)};
        }
        const bool keepAll = std::getenv("DETER_KEEP_ALL") != nullptr;  // (debugging: lift every hidden city and put it back)
        if (std::getenv("DETER_V1")) {  // (debugging: v1, every hidden city left untouched; counted as kept)
            rep.keptCities += int(hidden.size());
            return;
        }
        // Placement order from what isn't position: capitals first, then by level, high first.
        std::stable_sort(hidden.begin(), hidden.end(), [&](int a, int b) {
            if (cs[a].capital != cs[b].capital) return cs[a].capital;
            return at<uint16_t>(cs[a].imp, api.iLevel) > at<uint16_t>(cs[b].imp, api.iLevel);
        });
        // Lift them: their tiles become unowned sample land (unexplored tiles already have the sample's terrain).
        std::vector<char> taken(N, 0);  // owned by someone after the lift, or claimed by a placed city
        std::vector<char> cityAt(N, 0);
        for (int i = 0; i < N; ++i)
            if (ts[i]) {
                taken[i] = at<uint8_t>(ts[i], api.tOwner) != 0;
                Object* imp = at<Object*>(ts[i], api.tImprovement);
                cityAt[i] = imp && at<int32_t>(imp, api.iType) == kCity;
            }
        for (int k : hidden) {
            const City& c = cs[k];
            for (int i : c.terr) {
                put<uint8_t>(ts[i], api.tOwner, 0);
                put<uint8_t>(ts[i], api.tCapitalOf, 0);
                put<Coord>(ts[i], api.tRuling, at<Coord>(ftiles[i], api.tRuling));
                taken[i] = 0;
            }
            Object* t = ts[c.i];
            Object* ft = ftiles[c.i];
            Object* fimp = at<Object*>(ft, api.tImprovement);
            bool unowned = fimp && at<uint8_t>(ft, api.tOwner) == 0;
            field_set_value(t, fImprovement, unowned ? fimp : nullptr);
            put<int32_t>(t, api.tTerrain, at<int32_t>(ft, api.tTerrain));
            put<int32_t>(t, api.tClimate, at<int32_t>(ft, api.tClimate));
            field_set_value(t, fResource, at<Object*>(ft, api.tResource));
            clearList(at<Object*>(t, api.tEffects));
            put<bool>(t, tHasRoad, false);
            put<bool>(t, api.tRoute, false);
            put<bool>(t, tHadRoute, false);
            put<uint8_t>(t, api.tOwner, 0);
            put<uint8_t>(t, api.tCapitalOf, 0);
            put<Coord>(t, api.tRuling, at<Coord>(ftiles[c.i], api.tRuling));
            taken[c.i] = 0;
            cityAt[c.i] = 0;
        }
        auto cheb = [&](int a, int b) { return std::max(std::abs(a % W - b % W), std::abs(a / W - b / W)); };
        // A site is free if nobody owns it, no city is within 1 tile, and every tile within the city's
        // border is unexplored (a tile the viewer saw unowned can't be next to a city).
        auto freeSite = [&](int i, int border) {
            if (!ts[i] || taken[i]) return false;
            for (int j = 0; j < N; ++j) {
                int d = cheb(i, j);
                if (cityAt[j] && d <= 1 && j != i) return false;  // (the sample's own village on the site is replaced)
                if (d <= border && ts[j] && dst.exploredBy(ts[j], viewer)) return false;
            }
            return true;
        };
        // The sample map's sites: [capital of player p] and village sites (cities on unowned tiles).
        std::map<uint8_t, int> capSite;
        std::vector<int> villages;
        for (int i = 0; i < N; ++i) {
            Object* fimp = ftiles[i] ? at<Object*>(ftiles[i], api.tImprovement) : nullptr;
            if (!fimp || at<int32_t>(fimp, api.iType) != kCity) continue;
            uint8_t cap = at<uint8_t>(ftiles[i], api.tCapitalOf);
            if (cap) capSite[cap] = i;
            else villages.push_back(i);
        }
        std::mt19937_64 rng(seed ^ 0xc17e5u);
        std::shuffle(villages.begin(), villages.end(), rng);
        // Then any field or forest tile without an improvement (cities stand on those), in random order.
        std::vector<int> land;
        for (int i = 0; i < N; ++i)
            if (ts[i] && !at<Object*>(ts[i], api.tImprovement)) {
                int tr = at<int32_t>(ts[i], api.tTerrain);
                if (tr == kField || tr == kForest) land.push_back(i);
            }
        std::shuffle(land.begin(), land.end(), rng);
        std::vector<Coord> fromC, toC;
        for (int k : hidden) {
            const City& c = cs[k];
            int site = -1;
            auto cap = capSite.find(c.owner);
            if (!keepAll && c.capital && cap != capSite.end() && freeSite(cap->second, c.border)) site = cap->second;
            for (size_t v = 0; !keepAll && site < 0 && v < villages.size(); ++v)
                if (freeSite(villages[v], c.border)) site = villages[v];
            for (size_t v = 0; !keepAll && site < 0 && v < land.size(); ++v)
                if (freeSite(land[v], c.border)) site = land[v];
            if (site < 0) {  // no free site: put it back where it was, as it was
                site = c.i;
                ++rep.keptCities;
                const Orig& og = orig[c.i];
                put<int32_t>(ts[site], api.tTerrain, og.terrain);
                put<int32_t>(ts[site], api.tClimate, og.climate);
            } else {
                ++rep.movedCities;
            }
            Object* t = ts[site];
            field_set_value(t, fImprovement, c.imp);
            field_set_value(t, fResource, nullptr);
            Coord sc = at<Coord>(t, api.tCoords);
            put<uint8_t>(t, api.tOwner, c.owner);
            put<uint8_t>(t, api.tCapitalOf, c.capital ? c.owner : 0);
            put<Coord>(t, api.tRuling, sc);
            if (c.capital) put<Coord>(dst.player(c.owner), pStartTile, sc);  // the game finds a capital from it
            taken[site] = 1;
            cityAt[site] = 1;
            for (int i = 0; i < N; ++i)
                if (i != site && cheb(i, site) <= c.border && ts[i] && !taken[i] && !cityAt[i] && !dst.exploredBy(ts[i], viewer)) {
                    put<uint8_t>(ts[i], api.tOwner, c.owner);
                    put<Coord>(ts[i], api.tRuling, sc);
                    taken[i] = 1;
                }
            if (std::getenv("DETER_CITY_DEBUG"))
                std::printf("    hidden city of %d%s, level %d, border %d, territory %zu: (%d,%d) -> (%d,%d)%s\n", c.owner,
                            c.capital ? " (capital)" : "", int(at<uint16_t>(c.imp, api.iLevel)), c.border, c.terr.size(), c.i % W, c.i / W,
                            site % W, site / W, site == c.i ? " kept" : "");
            fromC.push_back(at<Coord>(ts[c.i], api.tCoords));
            toC.push_back(sc);
        }
        // Visible units whose home was a moved city.
        for (Object* t : ts)
            for (Object* u = t ? at<Object*>(t, api.tUnit) : nullptr; u; u = at<Object*>(u, api.uPassenger)) {
                if (at<uint8_t>(u, api.uOwner) == viewer) continue;
                Coord h = at<Coord>(u, api.uHome);
                for (size_t j = 0; j < fromC.size(); ++j)
                    if (h.x == fromC[j].x && h.y == fromC[j].y) {
                        put<Coord>(u, api.uHome, toC[j]);
                        break;
                    }
            }
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
            if (enemyCity) continue;  // relocateHiddenCities, below, moves it if its whole territory is unexplored
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

        relocateHiddenCities(dst, ftiles, viewer, seed, rep);

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
