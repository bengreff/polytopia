#pragma once
// Sets up and plays matches using the game's own classes: GameStateUtils to create a
// match, ActionManager to apply commands, and the built-in AI to choose bot moves.

#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

#include "il2cpp.hpp"

using namespace il2;

inline const std::string kRoot = "/data/local/tmp/poly";

inline std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) die("cannot read " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline void boot() {
    load(kRoot + "/libil2cpp.so");
    set_data_dir((kRoot + "/Managed").c_str());
    if (!init("polyhost")) die("il2cpp_init failed");
}

// The game reads its rules data through PolytopiaDataManager, which normally asks a
// Unity-side provider for the JSON. Prefill its cache with the shipped JSON instead.
inline void loadGameData() {
    Class* gld = cls("GameLogicAssembly", "Polytopia.Data", "GameLogicData");
    Class* avatar = cls("GameLogicAssembly", "Polytopia.Data", "AvatarData");
    Class* pdm = cls("GameLogicAssembly", "", "PolytopiaDataManager");
    Object* logicCache = nullptr;
    Object* avatarCache = nullptr;
    field_static_get_value(field(pdm, "gameLogicDatas"), &logicCache);
    field_static_get_value(field(pdm, "avatarDatas"), &avatarCache);
    if (!logicCache || !avatarCache) die("data caches are null");
    const Method* setLogic = method(object_get_class(logicCache), "set_Item", 2);
    const Method* setAvatar = method(object_get_class(avatarCache), "set_Item", 2);
    Object* latest = nullptr;
    for (int v = 1; v <= 28; ++v) {
        Object* d = make(gld);
        std::string json = readFile(kRoot + "/gamedata/GameLogicData" + std::to_string(v) + ".txt");
        call(method(gld, "Parse", 1), d, {string_new(json.c_str())});
        call(setLogic, logicCache, {&v, d});
        latest = d;
    }
    field_static_set_value(field(pdm, "currentVersion"), latest);
    Object* a = make(avatar);
    call(method(avatar, "Parse", 1), a, {string_new(readFile(kRoot + "/gamedata/AvatarData.txt").c_str())});
    for (int v = 0; v <= 1; ++v) call(setAvatar, avatarCache, {&v, a});
}

// Handles to the game's own classes and methods used to play a match.
struct Game {
    Class* gameState = cls("GameLogicAssembly", "", "GameState");
    Class* settings = cls("GameLogicAssembly", "", "GameSettings");
    Class* utils = cls("GameLogicAssembly", "", "GameStateUtils");
    Class* actionManager = cls("GameLogicAssembly", "", "ActionManager");
    Class* ai = cls("GameLogicAssembly", "", "AI");
    Class* player = cls("GameLogicAssembly", "", "PlayerState");
    Class* startMatch = cls("GameLogicAssembly", "", "StartMatchCommand");
    const Method* createAI = method(utils, "CreateAIOpponent", 5);
    const Method* createLocal = method(utils, "CreateLocalGame", 4);
    const Method* handicapFor = method(settings, "HandicapFromDifficulty", 1);
    const Method* playerStates = method(gameState, "get_PlayerStates", 0);
    const Method* currentPlayer = method(gameState, "get_CurrentPlayer", 0);
    const Method* currentTurn = method(gameState, "get_CurrentTurn", 0);
    const Method* currentState = method(gameState, "get_CurrentState", 0);
    const Method* playerId = method(player, "get_Id", 0);
    const Method* getMove = method(ai, "GetMove", 3);
    const Method* execute = method(actionManager, "ExecuteCommand", 2);
    const Method* tryTrigger = method(cls("GameLogicAssembly", "", "CommandTriggerUtils"), "TryGetTriggerCommand", 2);
    Class* list = nullptr;
    const Method* listCount = nullptr;
    const Method* listItem = nullptr;
    const Method* listAdd = nullptr;

    Game() {
        Object* probe = make(gameState);
        list = object_get_class(call(playerStates, probe));
        listCount = method(list, "get_Count", 0);
        listItem = method(list, "get_Item", 1);
        listAdd = method(list, "Add", 1);
    }

    template <class T>
    T get(const Method* m, Object* self) { return unbox<T>(call(m, self)); }

    int count(Object* list) { return unbox<int>(call(method(object_get_class(list), "get_Count", 0), list)); }
    Object* item(Object* list, int i) { return call(method(object_get_class(list), "get_Item", 1), list, {&i}); }

    Object* playerById(Object* gs, uint8_t id) {
        Object* ps = call(playerStates, gs);
        int n = get<int>(listCount, ps);
        for (int i = 0; i < n; ++i) {
            Object* p = call(listItem, ps, {&i});
            if (get<uint8_t>(playerId, p) == id) return p;
        }
        die("no player " + std::to_string(id));
    }

    uint8_t state(Object* gs) { return get<uint8_t>(currentState, gs); }  // GameState.State; 4 = Ended

    // The built-in AI's next command for the current player, including resolving a
    // pending choice such as a city level-up reward, exactly as the game does for bots.
    Object* botCommand(Object* gs) {
        Object* cmd = nullptr;
        if (unbox<bool>(call(tryTrigger, nullptr, {gs, &cmd})) && cmd) return cmd;
        int none = 0;
        return call(getMove, nullptr, {gs, playerById(gs, get<uint8_t>(currentPlayer, gs)), &none});
    }

    bool run(Object* am, Object* cmd, std::string* error = nullptr) {
        String* err = nullptr;
        bool ok = unbox<bool>(call(execute, am, {cmd, &err}));
        if (!ok && error) *error = utf8(err);
        return ok;
    }

    Object* start(Object* gs) {
        Object* am = make(actionManager, 1, {gs});
        uint8_t first = get<uint8_t>(currentPlayer, gs);
        std::string err;
        if (!run(am, make(startMatch, 1, {&first}), &err)) die("StartMatch rejected: " + err);
        return am;
    }
};

// Uniform seeds in [0, 2^31 - 1), the range of RandomNumberGenerator.GetInt32(int.MaxValue),
// from a seeded splitmix64 so games are reproducible.
struct SeedSource {
    uint64_t state;
    std::vector<int> replay;  // recorded seeds to hand out first
    size_t next = 0;
    explicit SeedSource(uint64_t s) : state(s) {}
    int32_t operator()() {
        if (next < replay.size()) return replay[next++];
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return int32_t(z % 0x7FFFFFFFull);
    }
};

// MapGenerator.Generate(gameState, settings, onComplete: null, iterations: -1), step for step as
// decompiled from the 2.17.3 build, with attempt seeds from `seeds` instead of the system's
// cryptographic generator.
inline void generateMap(Game& g, Object* gen, Object* gs, Object* mgs, SeedSource& seeds) {
    Class* generator = object_get_class(gen);
    const Method* generateInternal = method(generator, "GenerateInternal", 3);
    const Method* inequality = method(cls("GameLogicAssembly", "", "MapFairnessCalculations"), "GetInequality", 1);
    int iterations = get<int32_t>(mgs, "equalityIterations");
    float limit = get<float>(mgs, "equalityLimit");
    int retries = iterations != 1 ? 15 : 0;
    float best = 3.4028235e38f;
    int bestSeed = -1, lastSeed = -1;
    for (int i = 0; i < iterations; ++i) {
        int seed = seeds();
        lastSeed = seed;
        call(generateInternal, gen, {&seed, gs, mgs});
        float ineq = unbox<float>(call(inequality, nullptr, {gs}));
        if (-999.0f <= ineq) {  // a usable map
            if (ineq < best) { best = ineq; bestSeed = seed; }
            if (ineq < limit) { bestSeed = seed; break; }  // fair enough: keep it
        } else if (--retries > 0) {
            ++iterations;  // unusable: try again, up to 15 extra times
        }
    }
    int finalSeed = iterations != 1 ? bestSeed : lastSeed;
    if (lastSeed != finalSeed) call(generateInternal, gen, {&finalSeed, gs, mgs});
    // Each attempt added score; set every score to the final map's (PlayerExtensions.GetScore).
    const Method* getScore = method(cls("GameLogicAssembly", "", "PlayerExtensions"), "GetScore", 2);
    Object* players = call(g.playerStates, gs);
    for (int i = 0, n = g.count(players); i < n; ++i) {
        Object* p = g.item(players, i);
        uint32_t score = 0;
        if (g.get<uint8_t>(g.playerId, p) != 255) score = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(call(getScore, nullptr, {p, gs})) + 0x50);
        field_set_value(p, field(g.player, "score"), &score);
    }
}

// GameStateUtils.CreateLocalGame, step for step as decompiled from the 2.17.3 build. The one
// difference: the GameState constructor draws Seed and VillageNameSeed from the system's
// cryptographic generator, and so does MapGenerator.Generate for its attempt seeds; here they
// all come from `seeds`, so a game is reproducible from (settings, own player, seeds).
//
// Bot tribes come from GetRandomPickableTribe, which draws from a process-wide System.Random,
// not from the game's seed. Pass opponentTribes to fix them (for reproducing a recorded game).
inline Object* createLocalGame(Game& g, int version, Object* settings, Object* own, SeedSource& seeds,
                               const std::vector<int>& opponentTribes = {}) {
    Object* gs = make(g.gameState);
    int32_t seed = seeds(), villageSeed = seeds();
    field_set_value(gs, field(g.gameState, "<Version>k__BackingField"), &version);
    field_set_value(gs, field(g.gameState, "<Seed>k__BackingField"), &seed);
    field_set_value(gs, field(g.gameState, "<VillageNameSeed>k__BackingField"), &villageSeed);
    field_set_value(gs, field(g.gameState, "<Settings>k__BackingField"), settings);
    Object* players = make(g.list);
    field_set_value(gs, field(g.gameState, "<PlayerStates>k__BackingField"), players);
    bool tutorial = unbox<bool>(call(method(g.settings, "IsTutorial", 0), settings));
    if (tutorial) { int zero = 0; field_set_value(gs, field(g.gameState, "<Seed>k__BackingField"), &zero); }
    call(g.listAdd, players, {own});

    int difficulty = get<int32_t>(settings, "<Difficulty>k__BackingField");
    int handicap = unbox<int>(call(g.handicapFor, nullptr, {&difficulty}));
    const Method* addAI = method(g.utils, "AddAIOpponent", 4);
    int skin = 0;
    if (!tutorial) {
        const Method* pickTribe = methodWith(g.utils, "GetRandomPickableTribe", 1, "GameState");
        int opponents = get<int32_t>(settings, "<OpponentCount>k__BackingField");
        for (int i = 0; i < opponents; ++i) {
            int tribe = i < int(opponentTribes.size()) ? opponentTribes[i] : unbox<int>(call(pickTribe, nullptr, {gs}));
            call(addAI, nullptr, {gs, &tribe, &handicap, &skin});
        }
    } else {
        int xinxi = 13;
        call(addAI, nullptr, {gs, &xinxi, &handicap, &skin});
    }
    call(method(g.utils, "SetPlayerColors", 1), nullptr, {gs});
    call(method(g.utils, "AddNaturePlayer", 1), nullptr, {gs});

    int playerCount = unbox<int>(call(method(g.gameState, "get_PlayerCount", 0), gs));
    int minSize = unbox<uint16_t>(call(method(cls("GameLogicAssembly", "", "MapDataExtensions"), "GetMinimumMapSize", 1), nullptr, {&playerCount}));
    uint16_t size = uint16_t(std::max(get<int32_t>(settings, "<MapSize>k__BackingField"), minSize));
    Class* mapData = cls("GameLogicAssembly", "", "MapData");
    Object* map = object_new(mapData);
    call(methodWith(mapData, ".ctor", 2, "UInt16"), map, {&size, &size});
    field_set_value(gs, field(g.gameState, "<Map>k__BackingField"), map);
    Object* mgs = call(method(g.settings, "GetMapGeneratorSettings", 0), settings);
    Class* generator = cls("GameLogicAssembly", "", "MapGenerator");
    Object* gen = make(generator);
    if (!tutorial) {
        generateMap(g, gen, gs, mgs, seeds);
    } else {
        int tutorialSeed = 0x849ea;
        call(method(generator, "GenerateInternal", 3), gen, {&tutorialSeed, gs, mgs});
    }

    // Starting stars and starting unit for every real player.
    Object* data = call(method(g.gameState, "get_GameLogicData", 0), gs);
    Class* gld = cls("GameLogicAssembly", "Polytopia.Data", "GameLogicData");
    const Method* tribeData = methodWith(gld, "TryGetData", 2, "TribeData");
    const Method* unitData = methodWith(gld, "TryGetData", 2, "UnitData");
    const Method* setCurrency = method(g.player, "set_Currency", 1);
    const Method* getTile = method(cls("GameLogicAssembly", "", "MapDataExtensions"), "GetTile", 2);
    const Method* train = method(cls("GameLogicAssembly", "", "ActionUtils"), "TrainUnitScored", 4);
    int mode = get<int32_t>(settings, "baseGameMode");
    for (int i = 0, n = g.count(players); i < n; ++i) {
        Object* p = g.item(players, i);
        if (g.get<uint8_t>(g.playerId, p) == 255) continue;
        int tribe = get<int32_t>(p, "tribe");
        Object* td = nullptr;
        call(tribeData, data, {&tribe, &td});
        int stars = get<int32_t>(td, "startingStars");
        call(setCurrency, p, {&stars});
        if (mode == 7) { int zero = 0; call(setCurrency, p, {&zero}); continue; }  // GameMode.Tutorial
        Object* startUnit = get<Object*>(td, "startingUnit");
        int unitType = unbox<int32_t>(call(method(object_get_class(startUnit), "get_type", 0), startUnit));
        Object* ud = nullptr;
        if (!unbox<bool>(call(unitData, data, {&unitType, &ud}))) continue;
        alignas(8) unsigned char start[8];
        std::memcpy(start, reinterpret_cast<char*>(p) + 0x38, 8);  // PlayerState.startTile
        Object* tile = call(getTile, nullptr, {map, start});
        Object* unit = call(train, nullptr, {gs, p, tile, ud});
        if (unit) { *(reinterpret_cast<char*>(unit) + 0x56) = 0; *(reinterpret_cast<char*>(unit) + 0x57) = 0; }  // moved, attacked
    }
    return gs;
}

// The local player as ClientBase.CreateSession builds it: a new PlayerState (whose handicap
// defaults to 1, the +1 capital star) with Id 1, an empty account id, AutoPlay off and the
// tribe, skin and climate picked in the menu. Its name is set after CreateLocalGame returns.
inline Object* localPlayer(Game& g, int tribe, int skin = 0) {
    Object* p = make(g.player);
    uint8_t id = 1;
    field_set_value(p, field(g.player, "<Id>k__BackingField"), &id);
    alignas(8) unsigned char account[24] = {1};  // Nullable<Guid>: hasValue, then Guid.Empty
    field_set_value(p, field(g.player, "<AccountId>k__BackingField"), account);
    bool off = false, on = true;
    field_set_value(p, field(g.player, "<AutoPlay>k__BackingField"), &off);
    field_set_value(p, field(g.player, "tribe"), &tribe);
    int mix = 0;
    field_set_value(p, field(g.player, "tribeMix"), &mix);
    field_set_value(p, field(g.player, "skinType"), &skin);
    field_set_value(p, field(g.player, "hasChosenTribe"), &on);
    field_set_value(p, field(g.player, "_climate"), &tribe);
    return p;
}

// GameSettings as the client's menus leave them for a game mode chosen on the Game Mode screen
// (GameModeScreen_UI2.SelectGameMode: set_BaseGameMode, set_RulesGameMode, GameRules.LoadPreset)
// and the Game Setup screen (opponents, difficulty; map size follows the opponent count).
struct MenuChoice {
    int mode = 2;          // GameMode: 1 Perfection, 2 Domination
    int opponents = 1;
    int difficulty = 1;    // BotDifficulty
    int mapPreset = 3;     // MapPreset; the single-player default is Continents
    int mapSize = 0;       // 0: the size the setup screen derives from the opponent count
    std::vector<int> unlockedTribes = {0, 1, 4, 7, 10, 13};  // the free tribes; purchases add more
};

inline Object* menuSettings(Game& g, const MenuChoice& c) {
    Object* s = make(g.settings);
    call(method(g.settings, "set_BaseGameMode", 1), s, {(void*)&c.mode});
    call(method(g.settings, "set_RulesGameMode", 1), s, {(void*)&c.mode});
    call(method(g.settings, "set_OpponentCount", 1), s, {(void*)&c.opponents});
    call(method(g.settings, "set_Difficulty", 1), s, {(void*)&c.difficulty});
    int size = c.mapSize ? c.mapSize
                         : unbox<int>(call(method(g.settings, "GetPreferredMapSizeFromOpponentCount", 1), nullptr, {(void*)&c.opponents}));
    call(method(g.settings, "set_MapSize", 1), s, {&size});
    field_set_value(s, field(g.settings, "mapPreset"), (void*)&c.mapPreset);
    Object* unlocked = get<Object*>(s, "unlockedTribes");
    const Method* add = method(object_get_class(unlocked), "Add", 1);
    for (int t : c.unlockedTribes) call(add, unlocked, {&t});
    return s;
}
