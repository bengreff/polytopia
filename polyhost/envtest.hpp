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
    std::map<std::string, int> missingByClass, extraByClass, notOffered, botOnlyByClass;
};

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

    void apply(env::Match& b, FogStats& st) {
        Game& g = api.g;
        Object* bs = b.state();
        const Method* invisible = methodWith(api.unit, "IsInvisible", 2, "GameState");
        for (Object* t : b.tiles()) {
            Object* u = env::at<Object*>(t, api.tUnit);
            if (!b.exploredBy(t, viewer)) {
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
    std::printf("determinism: %d clones, %d mismatched\n", st.clones, st.cloneMismatch);
    return st;
}

}  // namespace envtest
