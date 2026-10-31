#pragma once
// Checks that the bot interface is complete and deterministic, by playing championship games and
// testing sampled states against the game itself:
//
// - legality: the legal list equals a brute-force sweep of every command the rules could accept
//   (every command class x every tile x every argument, each checked with the game's IsValid);
// - bot coverage: every command the built-in AI picks is in the legal list;
// - encoding: no two legal commands encode the same;
// - determinism: save/load round-trips byte for byte, a restored copy has the same legal list and
//   observation, and stepping the copy gives the same state as stepping the original.

#include <map>
#include <random>
#include <set>

#include "determinize.hpp"
#include "env.hpp"

namespace envtest {

using env::Cmd;

struct Brute {
    env::Match& m;
    std::vector<std::pair<Cmd, std::string>> found;

    void tryCmd(Class* c, int argc, std::initializer_list<void*> args) {
        Object* cmd = object_new(c);
        Object* r = nullptr;
        if (!tryCall(method(c, ".ctor", argc), cmd, args, &r)) return;
        if (!m.valid(cmd)) return;
        found.push_back({m.encode(cmd), class_get_name(c)});
    }

    // Every command of every class the rules could accept from the current player.
    void sweep() {
        auto C = [](const char* n) { return cls("GameLogicAssembly", "", n); };
        uint8_t p = m.currentPlayer();
        std::vector<uint8_t> opponents;
        Object* ps = m.players();
        for (int i = 0, n = env::listCount(ps); i < n; ++i) {
            uint8_t id = env::at<uint8_t>(env::listAt<Object*>(ps, i), m.api.pId);
            if (id != p) opponents.push_back(id);
        }
        const char* coordOnly[] = {"RecoverCommand", "HealOthersCommand", "PromoteCommand", "DisbandCommand", "StayCommand",
                                   "DisembarkCommand", "DestroyCommand", "ExamineRuinsCommand", "HideCommand", "BoostCommand",
                                   "BreakIceCommand", "DecomposeCommand", "ExplodeCommand", "FloodCommand", "FreezeAreaCommand",
                                   "SwarmCommand"};
        const char* typed[] = {"BuildCommand", "TrainCommand", "UpgradeCommand"};
        const char* withOpponent[] = {"PeaceTreatyCommand", "BreakPeaceCommand", "EstablishEmbassyCommand", "DestroyEmbassyCommand"};
        for (Object* t : m.tiles()) {
            env::Coord c = env::at<env::Coord>(t, m.api.tCoords);
            for (const char* n : coordOnly) tryCmd(C(n), 2, {&p, &c});
            for (const char* n : typed)
                for (int type = 0; type < 64; ++type) tryCmd(C(n), 3, {&p, &type, &c});
            for (const char* n : withOpponent)
                for (uint8_t o : opponents) tryCmd(C(n), 3, {&p, &o, &c});
            for (int reward = 0; reward <= 13; ++reward) tryCmd(C("CityRewardCommand"), 3, {&p, &reward, &c});
            for (int effect = 0; effect < 16; ++effect) tryCmd(C("ClearTileEffectCommand"), 3, {&p, &c, &effect});
            Object* here = env::at<Object*>(t, m.api.tUnit);
            if (here) {
                uint32_t id = env::at<uint32_t>(here, m.api.uId);
                tryCmd(C("CaptureCommand"), 3, {&p, &id, &c});
            }
            Object* u = env::at<Object*>(t, m.api.tUnit);
            if (u && env::at<uint8_t>(u, m.api.uOwner) == p) {
                for (Object* to : m.tiles()) {
                    env::Coord d = env::at<env::Coord>(to, m.api.tCoords);
                    tryCmd(C("MoveCommand"), 3, {&p, u, &d});
                    tryCmd(C("AttackCommand"), 3, {&p, u, &d});
                }
            }
        }
        for (int tech = 0; tech < 64; ++tech) tryCmd(C("ResearchCommand"), 2, {&p, &tech});
        for (uint8_t o : opponents)
            for (bool a : {true, false}) tryCmd(C("PeaceRequestResponseCommand"), 3, {&p, &o, &a});
        tryCmd(C("EndTurnCommand"), 1, {&p});
    }
};

inline std::string show(const Cmd& c) {
    char b[64];
    std::snprintf(b, sizeof b, "{type %d, a %d, b %d, arg %d}", c.type, c.a, c.b, c.arg);
    return b;
}

struct Stats {
    int states = 0, swept = 0, sweepMismatch = 0, botChecked = 0, botMissing = 0, duplicates = 0;
    int clones = 0, cloneMismatch = 0, games = 0, ended = 0, commands = 0;
    int botOnly = 0;  // AI commands the client never offers a person (StayCommand)
    int strictMismatch = 0;
    std::map<std::string, int> missingByClass, extraByClass, notOffered, botOnlyByClass;
};

// The fast legal list (generator outputs taken as validated) must equal the list with every
// command re-checked by IsValid.
inline void checkStrict(env::Match& m, Stats& st) {
    std::vector<Cmd> fast = m.legal;
    m.strictValidation = true;
    m.computeLegal();
    m.strictValidation = false;
    bool same = fast.size() == m.legal.size();
    for (size_t i = 0; same && i < fast.size(); ++i) same = fast[i] == m.legal[i];
    if (!same && st.strictMismatch++ < 3)
        std::printf("  fast legal list differs from the fully validated one (%zu vs %zu rows)\n", fast.size(), m.legal.size());
    m.computeLegal();
}

// Compares the legal list with the brute-force sweep at the current state. IsValid alone is laxer
// than the client: the game's generators add the checks the UI makes (UnitDataExtensions.CanCapture,
// CanRecover, no training on an occupied city, abilities the unit has, ...), so a command the sweep
// finds that no generator offers is reported, not failed. Move, Attack, Research and EndTurn have
// no such extra checks, so for those the two sets must be equal.
inline void checkLegal(env::Match& m, Stats& st) {
    Brute b{m};
    b.sweep();
    ++st.swept;
    std::set<std::tuple<int, int, int, int>> mine, all;
    for (const Cmd& c : m.legal) mine.insert({c.type, c.a, c.b, c.arg});
    bool trigger = false;
    for (const Cmd& c : m.legal) trigger |= c.type == 11 || c.type == 28;  // CityReward, PeaceRequestResponse
    bool bad = false;
    for (auto& [c, name] : b.found) {
        all.insert({c.type, c.a, c.b, c.arg});
        if (mine.count({c.type, c.a, c.b, c.arg})) continue;
        bool strict = !trigger && (c.type == 6 || c.type == 2 || c.type == 8 || c.type == 15);
        if (strict) {
            if (st.missingByClass[name]++ < 3) std::printf("  missing from legal list: %s %s\n", name.c_str(), show(c).c_str());
            bad = true;
        } else {
            int n = st.notOffered[name]++;
            static const char* watch[] = {"BuildCommand", "UpgradeCommand", "DisembarkCommand", "PromoteCommand", "EstablishEmbassyCommand"};
            if (std::getenv("POLY_SHOW_UNOFFERED") && n < 2)
                for (const char* w : watch)
                    if (name == w) {
                        Object* t = m.tiles()[c.a];
                        Object* u = env::at<Object*>(t, m.api.tUnit);
                        Object* imp = env::at<Object*>(t, m.api.tImprovement);
                        std::printf("  unoffered %s %s: tile owner %d, improvement %d, unit %d owner %d moved %d hp %d\n", name.c_str(),
                                    show(c).c_str(), env::at<uint8_t>(t, m.api.tOwner), imp ? env::at<int32_t>(imp, m.api.iType) : -1,
                                    u ? env::at<int32_t>(u, m.api.uType) : -1, u ? env::at<uint8_t>(u, m.api.uOwner) : -1,
                                    u ? env::at<bool>(u, m.api.uMoved) : -1, u ? env::at<uint16_t>(u, m.api.uHealth) : -1);
                    }
        }
    }
    for (const Cmd& c : m.legal)
        if (!all.count({c.type, c.a, c.b, c.arg})) {
            if (st.extraByClass[std::to_string(c.type)]++ < 3) std::printf("  not accepted by IsValid sweep: %s\n", show(c).c_str());
            bad = true;
        }
    st.sweepMismatch += bad;
}

// Saves, restores into a second match, and compares everything the interface exposes; then
// steps both with the same command and compares the resulting states byte for byte.
inline void checkClone(env::Api& api, env::Match& m, int action, Stats& st) {
    ++st.clones;
    std::string bytes = m.save();
    env::Match copy(api);
    copy.load(m.spec, bytes);
    // A first round trip normalizes what the serializer doesn't keep (map continents, the lobby
    // player list, the nature player's colour); after that it must be exact.
    std::string once = copy.save();
    env::Match twice(api);
    twice.load(m.spec, once);
    std::string why;
    if (twice.save() != once) why += " round-trip";
    bool sameLegal = copy.legal.size() == m.legal.size();
    for (size_t i = 0; sameLegal && i < m.legal.size(); ++i) sameLegal = copy.legal[i] == m.legal[i];
    if (!sameLegal) why += " legal-list";
    std::vector<int16_t> t1(size_t(m.width) * m.height * env::T_COUNT), t2(t1.size());
    int32_t g1[env::G_COUNT], g2[env::G_COUNT];
    uint8_t k1[env::kTechSlots], k2[env::kTechSlots];
    uint8_t viewer = m.currentPlayer();
    m.observe(viewer, t1.data(), g1, k1);
    copy.observe(viewer, t2.data(), g2, k2);
    if (t1 != t2 || std::memcmp(g1, g2, sizeof g1) || std::memcmp(k1, k2, sizeof k1)) why += " observation";
    if (why.empty() && action >= 0) {
        env::Match orig(api);  // step a second restored copy too, so the original game is untouched
        orig.load(m.spec, bytes);
        // The game's own lookups add default diplomacy relations the first time they're asked (no
        // change in content), so give both copies the same reads before comparing bytes.
        orig.observe(viewer, t1.data(), g1, k1);
        orig.run(orig.legalCmds[action].get());
        copy.run(copy.legalCmds[action].get());
        std::string a = orig.save(), b = copy.save();
        if (a != b) {
            why += " step";
            Json j;
            Reflect::command(j, orig.legalCmds[action].get());
            why += " " + j.s;
            Dumper da, db;
            da.object(orig.state(), 0);
            db.object(copy.state(), 0);
            std::ofstream(kRoot + "/step_a.json") << da.j.s;
            std::ofstream(kRoot + "/step_b.json") << db.j.s;
        }
    }
    if (!why.empty()) {
        ++st.cloneMismatch;
        std::printf("  clone mismatch at turn %u:%s\n", m.turn(), why.c_str());
    }
}

// The fog-leak test (M0). On a copy of the state, changes only what `viewer` may not know, and
// checks that the observation is byte-identical: everything on unexplored tiles (terrain,
// climate, resource, improvement, unit, owner), invisible enemy units, the opponent's stars and
// techs, the random seed, the next unit id, and the enemy-unit facts the client doesn't show (XP,
// age, home city). A positive control then changes a fact the viewer does see (own stars) and
// requires the observation to change.
//
// The legal list is the client's own list, computed from the true state, so it may depend on hidden
// facts exactly as the client's menus do (a bridge offered on a visible tile depends on the fogged
// tile across the water). On a second copy, changed only in ways that keep the state consistent
// (no owner or home-city changes), the test fails if any changed row acts on an unexplored tile,
// which no client menu can do; the other changes are counted and reported.
struct FogStats {
    int checks = 0, leaks = 0, controlFailures = 0, legalChanged = 0, fogRows = 0, hiddenTiles = 0, invisibleUnits = 0;
    std::map<std::string, int> changedRows;
};

struct Perturb {
    env::Api& api;
    uint8_t viewer;
    std::mt19937_64& rng;
    bool consistentOnly;
    bool keepCities = false;  // leave owned cities on unexplored tiles alone (env::Determinizer keeps them)

    void apply(env::Match& b, FogStats& st) {
        Game& g = api.g;
        Object* bs = b.state();
        const Method* invisible = methodWith(api.unit, "IsInvisible", 2, "GameState");
        for (Object* t : b.tiles()) {
            Object* u = env::at<Object*>(t, api.tUnit);
            if (!b.exploredBy(t, viewer)) {
                Object* city = env::at<Object*>(t, api.tImprovement);
                if (keepCities && city && env::at<int32_t>(city, api.iType) == env::Determinizer::kCity &&
                    env::at<uint8_t>(t, api.tOwner) != 0)
                    continue;
                if (!consistentOnly) ++st.hiddenTiles;
                env::put<int32_t>(t, api.tTerrain, int32_t(1 + rng() % 5));
                env::put<int32_t>(t, api.tClimate, int32_t(2 + rng() % 14));
                if (!consistentOnly) env::put<uint8_t>(t, api.tOwner, uint8_t(rng() % 2 ? 0 : 3 - viewer));
                if (Object* res = env::at<Object*>(t, api.tResource)) env::put<int32_t>(res, api.resType, int32_t(1 + rng() % 8));
                if (Object* imp = env::at<Object*>(t, api.tImprovement)) {
                    env::put<uint16_t>(imp, api.iLevel, uint16_t(1 + rng() % 5));
                    env::put<int16_t>(imp, api.iPopulation, int16_t(rng() % 4));
                }
                if (u) {
                    env::put<uint16_t>(u, api.uHealth, uint16_t(10 + rng() % 90));
                    env::put<int32_t>(u, api.uType, int32_t(1 + rng() % 10));
                }
            } else if (u && env::at<uint8_t>(u, api.uOwner) != viewer) {
                Object* r = nullptr;
                if (tryCall(invisible, u, {bs, &viewer}, &r) && r && unbox<bool>(r)) {
                    if (!consistentOnly) ++st.invisibleUnits;
                    env::put<uint16_t>(u, api.uHealth, uint16_t(10 + rng() % 90));
                }
                env::put<uint16_t>(u, api.uXp, uint16_t(rng() % 20));
                env::put<uint16_t>(u, api.uCreated, uint16_t(rng() % 10));
                if (!consistentOnly) env::put<env::Coord>(u, api.uHome, env::Coord{int32_t(rng() % 14), int32_t(rng() % 14)});
            }
        }
        Object* ps = b.players();
        for (int i = 0, n = env::listCount(ps); i < n; ++i) {
            Object* p = env::listAt<Object*>(ps, i);
            if (env::at<uint8_t>(p, api.pId) == viewer) continue;
            if (keepCities && env::at<uint8_t>(p, api.pId) == 255) continue;  // the nature player's state is public and constant
            int stars = int(rng() % 200);
            call(method(g.player, "set_Currency", 1), p, {&stars});
            if (Object* techs = env::at<Object*>(p, api.pTech)) {
                int extra = int(1 + rng() % 50);
                call(method(object_get_class(techs), "Add", 1), techs, {&extra});
            }
        }
        int32_t seed = int32_t(rng() % 1000000);
        field_set_value(bs, field(g.gameState, "<Seed>k__BackingField"), &seed);
        uint32_t unitId = uint32_t(1000 + rng() % 1000);
        field_set_value(bs, field(g.gameState, "<CurrentUnitId>k__BackingField"), &unitId);
    }
};

inline void checkFog(env::Api& api, env::Match& m, FogStats& st, std::mt19937_64& rng) {
    Game& g = api.g;
    uint8_t viewer = m.currentPlayer();
    std::string bytes = m.save();
    env::Match a(api), b(api), c(api);
    a.load(m.spec, bytes);
    b.load(m.spec, bytes);
    c.load(m.spec, bytes);
    std::vector<int16_t> ta(size_t(a.width) * a.height * env::T_COUNT), tb(ta.size());
    int32_t ga[env::G_COUNT], gb[env::G_COUNT];
    uint8_t ka[env::kTechSlots], kb[env::kTechSlots];
    a.observe(viewer, ta.data(), ga, ka);

    Perturb{api, viewer, rng, false}.apply(b, st);
    b.observe(viewer, tb.data(), gb, kb);
    ++st.checks;
    bool leak = ta != tb || std::memcmp(ga, gb, sizeof ga) || std::memcmp(ka, kb, sizeof ka);
    if (leak && st.leaks++ < 5) {
        for (size_t i = 0; i < ta.size(); ++i)
            if (ta[i] != tb[i]) { std::printf("  LEAK: tile %zu feature %zu: %d -> %d\n", i / env::T_COUNT, i % env::T_COUNT, ta[i], tb[i]); break; }
        for (int i = 0; i < env::G_COUNT; ++i)
            if (ga[i] != gb[i]) std::printf("  LEAK: global %d: %d -> %d\n", i, ga[i], gb[i]);
    }

    // Positive control: the viewer's own stars are visible, so the observation must change.
    Object* me = b.player(viewer);
    int mine = unbox<int>(call(api.getCurrency, me)) + 7;
    call(method(g.player, "set_Currency", 1), me, {&mine});
    b.observe(viewer, tb.data(), gb, kb);
    if (!std::memcmp(ga, gb, sizeof ga)) ++st.controlFailures;

    Perturb{api, viewer, rng, true}.apply(c, st);
    c.computeLegal();
    std::set<std::tuple<int, int, int, int>> la, lc;
    for (const Cmd& x : a.legal) la.insert({x.type, x.a, x.b, x.arg});
    for (const Cmd& x : c.legal) lc.insert({x.type, x.a, x.b, x.arg});
    st.legalChanged += la != lc;
    std::vector<Object*> tiles = a.tiles();
    for (auto* pair : {&la, &lc})
        for (auto& x : *pair)
            if (!(pair == &la ? lc : la).count(x)) {
                int type = std::get<0>(x), at = std::get<1>(x);
                bool diplomacy = type == 27 || type == 29 || type == 30 || type == 32;  // the diplomacy screen
                bool fog = !diplomacy && at >= 0 && !a.exploredBy(tiles[at], viewer);
                st.fogRows += fog;
                ++st.changedRows[std::to_string(type) + (pair == &la ? " lost" : " gained") + (fog ? ", acting on an unexplored tile" : "")];
            }
}

inline int fogTest(int games, uint64_t seed, int every) {
    Game g;
    env::Api api(g);
    FogStats st;
    std::mt19937_64 rng(seed);
    for (int k = 0; k < games; ++k) {
        env::Spec spec;
        const int tribes[] = {2, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        spec.seats[0].tribe = tribes[rng() % 12];
        spec.seats[1].tribe = tribes[rng() % 12];
        spec.seed = rng();
        env::Match m(api);
        m.create(spec);
        int decisions = 0;
        while (!m.ended() && m.turn() < 100) {
            if (++decisions % every == 0) checkFog(api, m, st, rng);
            int bot = m.botChoice();
            if (!m.step(bot >= 0 ? bot : env::Match::kBotOwn)) die("step failed: " + m.lastError);
        }
        std::printf("game %d: %d checks so far, %d leaks, %d control failures, %d fog rows\n", k, st.checks, st.leaks,
                    st.controlFailures, st.fogRows);
        std::fflush(stdout);
    }
    std::printf("\nfog leak: %d states checked (%d hidden tiles and %d invisible enemy units perturbed), %d leaks, "
                "%d positive-control failures\n", st.checks, st.hiddenTiles, st.invisibleUnits, st.leaks, st.controlFailures);
    std::printf("legal list: changed in %d of %d states under consistent hidden changes; %d changed rows act on an "
                "unexplored tile (must be 0). Changed rows by CommandType:\n", st.legalChanged, st.checks, st.fogRows);
    for (auto& [k, n] : st.changedRows) std::printf("  type %-45s %d\n", k.c_str(), n);
    return st.leaks || st.fogRows ? 1 : st.controlFailures ? 2 : 0;
}

struct DeterStats {
    int checks = 0, obsMismatch = 0, notIdempotent = 0, perturbed = 0, leaks = 0, withHiddenCities = 0, cityLeaks = 0;
    int playouts = 0, playoutFailures = 0, historyChecks = 0, historyMismatch = 0;
    long hiddenUnits = 0, fogUnits = 0, fogTiles = 0, keptCities = 0, techs = 0, trueTechs = 0, wrongTechs = 0, stars = 0, trueStars = 0;
};

inline bool sameObs(env::Match& a, env::Match& b, uint8_t viewer, std::string* where = nullptr) {
    std::vector<int16_t> ta(size_t(a.width) * a.height * env::T_COUNT), tb(ta.size());
    int32_t ga[env::G_COUNT], gb[env::G_COUNT];
    uint8_t ka[env::kTechSlots], kb[env::kTechSlots];
    a.observe(viewer, ta.data(), ga, ka);
    b.observe(viewer, tb.data(), gb, kb);
    for (size_t i = 0; i < ta.size(); ++i)
        if (ta[i] != tb[i]) {
            if (where) *where = "tile " + std::to_string(i / env::T_COUNT) + " feature " + std::to_string(i % env::T_COUNT) + ": " +
                                std::to_string(ta[i]) + " -> " + std::to_string(tb[i]);
            return false;
        }
    for (int i = 0; i < env::G_COUNT; ++i)
        if (ga[i] != gb[i]) {
            if (where) *where = "global " + std::to_string(i) + ": " + std::to_string(ga[i]) + " -> " + std::to_string(gb[i]);
            return false;
        }
    if (std::memcmp(ka, kb, sizeof ka)) {
        if (where) *where = "techs";
        return false;
    }
    return true;
}

// Where two states' JSON dumps first differ, with some context.
inline std::string firstDiff(Object* a, Object* b) {
    Dumper da, db;
    da.object(a, 0);
    db.object(b, 0);
    const std::string &x = da.j.s, &y = db.j.s;
    size_t i = 0;
    while (i < x.size() && i < y.size() && x[i] == y[i]) ++i;
    size_t from = i > 160 ? i - 160 : 0;
    return "..." + x.substr(from, 240) + "\n   vs ..." + y.substr(from, 240);
}

// The determinized-state builder (determinize.hpp) on one state, for the current player.
inline void checkDeter(env::Api& api, env::Determinizer& det, env::Match& m, DeterStats& st, std::mt19937_64& rng) {
    uint8_t viewer = m.currentPlayer();
    if (std::getenv("DETER_DEBUG")) {
        int owned = 0, hidden = 0;
        for (Object* t : m.tiles())
            if (Object* imp = env::at<Object*>(t, api.tImprovement))
                if (env::at<int32_t>(imp, api.iType) == env::Determinizer::kCity && env::at<uint8_t>(t, api.tOwner) != 0 &&
                    env::at<uint8_t>(t, api.tOwner) != viewer) {
                    ++owned;
                    hidden += !m.exploredBy(t, viewer);
                }
        std::printf("  debug turn %u viewer %d: %d enemy cities, %d unexplored\n", m.turn(), viewer, owned, hidden);
        std::map<std::pair<int, int>, int> kinds;
        for (Object* t : m.tiles())
            if (Object* imp = env::at<Object*>(t, api.tImprovement))
                ++kinds[{env::at<int32_t>(imp, api.iType), int(env::at<uint8_t>(imp, env::off(api.improvement, "founder"))) * 1000 + env::at<uint8_t>(t, api.tOwner)}];
        for (auto& [k, n] : kinds) std::printf("    improvement type %d, founder %d, tile owner %d: %d\n", k.first, k.second / 1000, k.second % 1000, n);
    }
    env::Match d1(api);
    env::Determinizer::Report rep = det.build(m, d1, viewer, 7);
    ++st.checks;
    st.hiddenUnits += rep.hiddenUnits;
    st.fogUnits += rep.fogUnits;
    st.fogTiles += rep.fogTiles;
    st.keptCities += rep.keptCities;
    st.techs += rep.techs;
    st.trueTechs += rep.trueTechs;
    st.wrongTechs += rep.wrongTechs;
    st.stars += rep.stars;
    st.trueStars += rep.trueStars;

    // 1. The viewer sees exactly what it saw.
    std::string where;
    if (!sameObs(m, d1, viewer, &where) && st.obsMismatch++ < 5) std::printf("  OBSERVATION CHANGED (turn %u): %s\n", m.turn(), where.c_str());
    // 2. The output is a fixed point of the game's serializer.
    std::string s1 = d1.save();
    env::Match d1b(api);
    d1b.load(m.spec, s1);
    if (d1b.save() != s1) ++st.notIdempotent;
    // 3. Hidden changes (all but owned cities in the fog) don't reach the output.
    FogStats fs;
    env::Match b(api);
    b.load(m.spec, m.save());
    Perturb{api, viewer, rng, true, true}.apply(b, fs);
    if (sameObs(m, b, viewer)) {
        ++st.perturbed;
        env::Match d2(api);
        det.build(b, d2, viewer, 7);
        if (d2.save() != s1 && st.leaks++ < 5) std::printf("  LEAK (turn %u):\n   %s\n", m.turn(), firstDiff(d1.state(), d2.state()).c_str());
    }
    // 4. The known leak: owned cities in the fog are kept, so changing them shows.
    if (rep.keptCities) {
        ++st.withHiddenCities;
        env::Match c(api), d3(api);
        c.load(m.spec, m.save());
        Perturb{api, viewer, rng, true, false}.apply(c, fs);
        det.build(c, d3, viewer, 7);
        st.cityLeaks += d3.save() != s1;
    }
    // 5. The sample is a playable game: the built-in AI plays both seats for two turns.
    ++st.playouts;
    uint32_t until = d1.turn() + 2;
    for (int n = 0; !d1.ended() && d1.turn() < until && n < 4000; ++n) {
        Object* cmd = nullptr;  // the AI's move, as Game::botCommand but reporting an exception
        int none = 0;
        uint8_t cur = d1.currentPlayer();
        Object* trig = nullptr;
        std::string err;
        if (!(unbox<bool>(call(api.g.tryTrigger, nullptr, {d1.state(), &trig})) && trig) &&
            !tryCall(api.g.getMove, nullptr, {d1.state(), d1.player(cur), &none}, &cmd, &err)) {
            if (st.playoutFailures++ < 5) std::printf("  AI FAILED ON THE SAMPLE (turn %u, player %d): %.900s\n", d1.turn(), cur, err.c_str());
            break;
        }
        int bot = d1.botChoice();
        if (!d1.step(bot >= 0 ? bot : env::Match::kBotOwn)) {
            if (st.playoutFailures++ < 5) std::printf("  PLAYOUT FAILED (turn %u): %s\n", d1.turn(), d1.lastError.c_str());
            break;
        }
    }
}

// The rules never read the command history: the same commands from a state and from its copy with
// the history emptied give the same legal lists, and the same state apart from the history.
inline void checkHistory(env::Api& api, env::Match& m, DeterStats& st, std::mt19937_64& rng) {
    env::Match a(api), h(api);
    a.load(m.spec, m.save());
    h.load(m.spec, m.save());
    env::Determinizer::stripHistory(api.g, h.state(), m.currentPlayer());
    h.computeLegal();
    ++st.historyChecks;
    std::mt19937_64 r(rng());
    auto stripped = [&](env::Match& x) {  // the state without its history
        env::Match y(api);
        y.load(x.spec, x.save());
        env::Determinizer::stripHistory(api.g, y.state(), m.currentPlayer());
        return y.save();
    };
    bool diverged = false;
    for (int i = 0; i < 80 && !a.ended(); ++i) {
        if (!(a.legal == h.legal)) { ++st.historyMismatch; return; }
        int k = r() % 8 == 0 ? int(a.legal.size()) - 1 : int(r() % a.legal.size());  // the last row is usually End Turn
        env::Cmd c = a.legal[k];
        if (a.step(k) != h.step(k)) { ++st.historyMismatch; return; }
        if (!diverged && stripped(a) != stripped(h)) {
            diverged = true;
            Object* acts = get<Object*>(m.state(), "<ActionStack>k__BackingField");
            size_t pid = env::off(cls("GameLogicAssembly", "", "ActionBase"), "<PlayerId>k__BackingField");
            int n = env::listCount(acts);
            std::printf("  action stack tail (%d actions, viewer %d):", n, m.currentPlayer());
            for (int j = std::max(0, n - 14); j < n; ++j) {
                Object* x = env::listAt<Object*>(acts, j);
                std::printf(" %s/%d", class_get_name(object_get_class(x)), env::at<uint8_t>(x, pid));
            }
            std::printf("\n");
            std::printf("  history: states diverge after command type %d (%d -> %d, arg %d), turn %u, step %d\n", c.type, c.a, c.b,
                        c.arg, a.turn(), i);
        }
    }
    env::Determinizer::stripHistory(api.g, a.state(), m.currentPlayer());
    env::Determinizer::stripHistory(api.g, h.state(), m.currentPlayer());
    if (a.save() != h.save()) {
        if (st.historyMismatch++ < 3) std::printf("  HISTORY READ:\n   %s\n", firstDiff(a.state(), h.state()).c_str());
    }
}

inline int deterTest(int games, uint64_t seed, int every) {
    Game g;
    env::Api api(g);
    env::Determinizer det(api);
    DeterStats st;
    std::mt19937_64 rng(seed);
    for (int k = 0; k < games; ++k) {
        env::Spec spec;
        const int tribes[] = {2, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        spec.seats[0].tribe = tribes[rng() % 12];
        spec.seats[1].tribe = tribes[rng() % 12];
        spec.seed = rng();
        env::Match m(api);
        m.create(spec);
        int decisions = 0;
        while (!m.ended() && m.turn() < 60) {
            if (++decisions % every == 0) {
                checkDeter(api, det, m, st, rng);
                if (decisions % (every * 4) == 0) checkHistory(api, m, st, rng);
            }
            int bot = m.botChoice();
            if (!m.step(bot >= 0 ? bot : env::Match::kBotOwn)) die("step failed: " + m.lastError);
        }
        std::printf("game %d (turn %u): %d checks, %d observation changes, %d leaks, %d city leaks of %d with hidden cities, "
                    "%d playout failures, %d history mismatches of %d\n", k, m.turn(), st.checks, st.obsMismatch, st.leaks,
                    st.cityLeaks, st.withHiddenCities, st.playoutFailures, st.historyMismatch, st.historyChecks);
        std::fflush(stdout);
    }
    double n = std::max(st.checks, 1);
    std::printf("\ndeterminize: %d states; per state %.1f invisible units, %.1f units in the fog removed, %.1f fog tiles "
                "resampled, %.2f enemy cities in the fog kept\n", st.checks, st.hiddenUnits / n, st.fogUnits / n, st.fogTiles / n,
                st.keptCities / n);
    std::printf("opponent techs: %.1f inferred vs %.1f true, %ld inferred but not held (must be 0); stars: %.1f set vs %.1f true\n",
                st.techs / n, st.trueTechs / n, st.wrongTechs, st.stars / n, st.trueStars / n);
    std::printf("checks: %d observation changes (must be 0), %d not idempotent (must be 0), %d leaks of %d perturbed (must be 0), "
                "%d playout failures of %d (must be 0), history read in %d of %d (must be 0); known city leak in %d of %d\n",
                st.obsMismatch, st.notIdempotent, st.leaks, st.perturbed, st.playoutFailures, st.playouts, st.historyMismatch,
                st.historyChecks, st.cityLeaks, st.withHiddenCities);
    return st.obsMismatch || st.notIdempotent || st.leaks || st.playoutFailures || st.historyMismatch || st.wrongTechs ? 1 : 0;
}

// Plays `games` games with both seats driven from outside: mostly the built-in AI's choice, with
// some random legal commands for variety. Checks every decision for bot coverage and encoding, and
// every `every`-th decision for legality and clone determinism.
inline Stats run(int games, uint64_t seed, int every, int turnCap) {
    Game g;
    env::Api api(g);
    Stats st;
    std::mt19937_64 rng(seed);
    const int tribes[] = {13, 7, 4, 10};  // the free tribes the real app has: Xin-xi, Imperius, Bardur, Oumaji
    for (int k = 0; k < games; ++k) {
        env::Spec spec;
        spec.seats[0].tribe = tribes[rng() % 4];
        spec.seats[1].tribe = tribes[rng() % 4];
        spec.seed = rng();
        env::Match m(api);
        m.create(spec);
        ++st.games;
        while (!m.ended() && m.turn() < uint32_t(turnCap)) {
            ++st.states;
            std::set<std::tuple<int, int, int, int>> seen;
            for (const Cmd& c : m.legal)
                if (!seen.insert({c.type, c.a, c.b, c.arg}).second) ++st.duplicates;
            int bot = m.botChoice();
            ++st.botChecked;
            if (bot < 0) {
                Object* cmd = m.botCommand();
                std::string name = class_get_name(object_get_class(cmd));
                // Outside the agent's list by design: the AI's StayCommand, and commands on tiles the
                // player hasn't explored (the AI sees the whole map; Ben: fine for the built-in AI).
                Cmd enc = m.encode(cmd);
                bool fog = enc.a >= 0 && !m.exploredBy(m.tiles()[enc.a], m.currentPlayer()) &&
                           !(enc.type == 27 || enc.type == 29 || enc.type == 30 || enc.type == 32);
                if (name == "StayCommand" || fog) {
                    ++st.botOnly;
                    ++st.botOnlyByClass[name + (fog ? " on an unexplored tile" : "")];
                } else if (st.botMissing++ < 5) {
                    Json j;
                    Reflect::command(j, cmd);
                    std::printf("  bot command not in legal list (turn %u): %s\n", m.turn(), j.s.c_str());
                }
            }
            int action = rng() % 10 ? (bot >= 0 ? bot : env::Match::kBotOwn) : int(rng() % m.legal.size());
            checkStrict(m, st);
            if (st.states % every == 0) {
                checkLegal(m, st);
                checkClone(api, m, action >= 0 ? action : -1, st);
            }
            if (!m.step(action)) die("legal command rejected: " + m.lastError);
            ++st.commands;
        }
        st.ended += m.ended();
        std::printf("game %d: %s at turn %u, winner %d, %d decisions so far\n", k, m.ended() ? "ended" : "turn cap",
                    m.turn(), m.winner(), st.states);
        std::fflush(stdout);
    }
    std::printf("\n%d games (%d ended), %d decisions, %d commands\n", st.games, st.ended, st.states, st.commands);
    std::printf("legality: %d states swept, %d mismatched\n", st.swept, st.sweepMismatch);
    std::printf("bot coverage: %d checked, %d not in the legal list, %d outside it by design:\n", st.botChecked, st.botMissing, st.botOnly);
    for (auto& [name, n] : st.botOnlyByClass) std::printf("  %-40s %d\n", name.c_str(), n);
    std::printf("accepted by IsValid but not offered by the game's generators (expected, see checkLegal):\n");
    for (auto& [name, n] : st.notOffered) std::printf("  %-28s %d\n", name.c_str(), n);
    std::printf("encoding: %d duplicate encodings\n", st.duplicates);
    std::printf("fast vs fully validated legal list: %d of %d states differ\n", st.strictMismatch, st.states);
    std::printf("determinism: %d clones, %d mismatched\n", st.clones, st.cloneMismatch);
    return st;
}

}  // namespace envtest
