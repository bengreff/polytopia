#pragma once
// The bot interface. A Match is one game in the championship format, built the way the real
// client builds a pass-and-play game (ClientBase.CreateSession, GameType.PassAndPlay branch).
// The current player's legal commands come from the game's own generators and validators, the
// observation is fog-correct (only what that player's client shows), and save/restore uses the
// game's own serializer. No rule is written here.

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "dump.hpp"

namespace env {

// Managed List<T>: items array at 0x10, count at 0x18. Managed array: data at 0x20.
inline int listCount(Object* l) { return l ? *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(l) + 0x18) : 0; }
template <class T>
inline T listAt(Object* l, int i) {
    char* items = *reinterpret_cast<char**>(reinterpret_cast<char*>(l) + 0x10);
    return reinterpret_cast<T*>(items + 0x20)[i];
}
template <class T>
inline T at(Object* o, size_t off) { return *reinterpret_cast<T*>(reinterpret_cast<char*>(o) + off); }
template <class T>
inline void put(Object* o, size_t off, T v) { *reinterpret_cast<T*>(reinterpret_cast<char*>(o) + off) = v; }
inline size_t off(Class* c, const char* name) { return field_get_offset(field(c, name)); }

struct Coord { int32_t x, y; };

// One seat of a match. bot < 0: the seat's moves come from outside (the agent, or a person).
// bot >= 0: the game's own AI plays it at that BotDifficulty (0 Easy, 1 Normal, 2 Hard, 3 Crazy).
struct Seat {
    int tribe = 7;
    int bot = -1;
};

// The championship format by default: Might, Lakes, Small (14x14).
struct Spec {
    Seat seats[2];
    int mode = 4;       // GameMode.Might
    int mapPreset = 2;  // MapPreset.Lakes
    int mapSize = 14;   // Small
    uint64_t seed = 1;  // all of the game's random draws at creation come from this
};

// A command as the policy sees it: its CommandType, the tile it acts from or on, the tile it
// targets, and one argument (unit, improvement or tech type, city reward, opponent).
struct Cmd {
    int16_t type = 0, a = -1, b = -1, arg = 0;
    bool operator==(const Cmd& o) const { return type == o.type && a == o.a && b == o.b && arg == o.arg; }
};

// Legal-list version, recorded with data. 2: tile menus only on explored tiles, as the client.
constexpr int kLegalVersion = 2;

// Observation layout, versioned so recorded data says which encoder made it. Tile features are
// zero on unexplored tiles; unit features describe the unit the viewer's client shows there (the
// game's TileData.GetUnit, which hides invisible enemy units). Owners are relative (see rel()).
// Unit types are stored +1 so 0 means no unit; tiles are indices y * width + x, -1 for none.
constexpr int kObsVersion = 1;
enum TileFeature {
    T_EXPLORED, T_TERRAIN, T_CLIMATE, T_OWNER, T_CAPITAL_OF, T_RULING_CITY, T_RESOURCE, T_ROAD, T_ROUTE, T_EFFECTS,
    T_IMPROVEMENT, T_IMP_OWNER, T_IMP_LEVEL, T_IMP_POPULATION, T_IMP_PRODUCTION, T_IMP_BORDER, T_IMP_CONNECTED,
    T_IMP_REWARDS, T_IMP_EFFECTS, T_IMP_XP,
    T_UNIT, T_UNIT_OWNER, T_UNIT_HEALTH, T_UNIT_MAX_HEALTH, T_UNIT_PROMOTED, T_UNIT_XP, T_UNIT_MOVED, T_UNIT_ATTACKED,
    T_UNIT_PASSENGER, T_UNIT_EFFECTS, T_UNIT_AGE, T_UNIT_HOME,
    T_COUNT
};
enum GlobalFeature {
    G_TURN, G_SEAT, G_ME, G_TRIBE, G_STARS, G_INCOME, G_SCORE, G_CITIES, G_KILLS, G_CASUALTIES, G_HANDICAP,
    G_OPP_TRIBE, G_OPP_SCORE, G_OPP_KNOWN, G_RELATION, G_MY_EMBASSY, G_THEIR_EMBASSY, G_TRIGGER, G_WIDTH, G_HEIGHT,
    G_COUNT
};
constexpr int kTechSlots = 64;  // TechData.Type values are 0-50 in logic data 28

// Owner as seen by the observer: 0 nobody, 1 the observer, 2 the opponent, 3 anyone else.
inline int16_t rel(uint8_t owner, uint8_t me) { return owner == 0 ? 0 : owner == me ? 1 : owner == 255 ? 3 : 2; }

// Handles to the game's classes, methods and field offsets that the interface uses.
struct Api {
    Game& g;
    Class* tile = cls("GameLogicAssembly", "", "TileData");
    Class* unit = cls("GameLogicAssembly", "", "UnitState");
    Class* improvement = cls("GameLogicAssembly", "", "ImprovementState");
    Class* map = cls("GameLogicAssembly", "", "MapData");
    Class* playerData = cls("GameLogicAssembly", "", "PlayerData");
    Class* cmdUtils = cls("GameLogicAssembly", "", "CommandUtils");
    Class* unitExt = cls("GameLogicAssembly", "", "UnitDataExtensions");
    Class* commandBase = cls("GameLogicAssembly", "", "CommandBase");
    const Method* unitActions = method(cmdUtils, "GetUnitActions", 4);
    const Method* trainable = method(cmdUtils, "GetTrainableUnits", 4);
    const Method* buildable = method(cmdUtils, "GetBuildableImprovements", 4);
    const Method* abilities = method(cmdUtils, "GetImprovementAbilities", 4);
    const Method* diplomacy = method(cmdUtils, "GetDiplomacyCommandsForOpponent", 4);
    const Method* movement = methodWith(unitExt, "GetMovement", 2, "UnitState");
    const Method* moveOptions = method(unitExt, "GetMovementOptions", 3);
    const Method* range = methodWith(unitExt, "GetRange", 2, "UnitState");
    const Method* attackOptions = method(unitExt, "GetAttackOptions", 4);
    const Method* maxHealth = method(unitExt, "GetMaxHealth", 2);
    const Method* isValid = method(commandBase, "IsValid", 1);
    const Method* pendingTrigger = method(g.gameState, "TryGetPendingCommandTrigger", 2);
    const Method* explored = method(tile, "GetExplored", 1);
    const Method* visibleUnit = method(tile, "GetUnit", 3);
    const Method* income = method(cls("GameLogicAssembly", "", "ResourceDataUtils"), "CalculateIncomeFor", 2);
    const Method* winner = method(g.gameState, "TryGetWinner", 1);
    const Method* getCurrency = method(g.player, "get_Currency", 0);
    Class* cMove = cls("GameLogicAssembly", "", "MoveCommand");
    Class* cAttack = cls("GameLogicAssembly", "", "AttackCommand");
    Class* cResearch = cls("GameLogicAssembly", "", "ResearchCommand");
    Class* cEndTurn = cls("GameLogicAssembly", "", "EndTurnCommand");
    Class* cCityReward = cls("GameLogicAssembly", "", "CityRewardCommand");
    Class* cPeaceResponse = cls("GameLogicAssembly", "", "PeaceRequestResponseCommand");
    Class* cEndMatch = cls("GameLogicAssembly", "", "EndMatchCommand");
    const Method* newMove = method(cMove, ".ctor", 3);
    const Method* newAttack = method(cAttack, ".ctor", 3);
    const Method* newResearch = method(cResearch, ".ctor", 2);
    const Method* newEndTurn = method(cEndTurn, ".ctor", 1);
    const Method* newCityReward = method(cCityReward, ".ctor", 3);
    const Method* newPeaceResponse = method(cPeaceResponse, ".ctor", 3);
    const Method* newEndMatch = method(cEndMatch, ".ctor", 1);

    // Field offsets.
    size_t gsMap = off(g.gameState, "<Map>k__BackingField");
    size_t gsPlayers = off(g.gameState, "<PlayerStates>k__BackingField");
    size_t gsTurn = off(g.gameState, "<CurrentTurn>k__BackingField");
    size_t gsState = off(g.gameState, "<CurrentState>k__BackingField");
    size_t mapWidth = off(map, "width"), mapHeight = off(map, "height"), mapTiles = off(map, "tiles");
    size_t tCoords = off(tile, "coordinates"), tTerrain = off(tile, "terrain"), tClimate = off(tile, "climate");
    size_t tEffects = off(tile, "effects"), tOwner = off(tile, "owner"), tCapitalOf = off(tile, "capitalOf");
    size_t tExplorers = off(tile, "explorers"), tRuling = off(tile, "rulingCityCoordinates");
    size_t tImprovement = off(tile, "improvement"), tResource = off(tile, "resource"), tUnit = off(tile, "<unit>k__BackingField");
    size_t tRoad = off(tile, "hasRoad"), tRoute = off(tile, "hasRoute");
    size_t uId = off(unit, "id"), uOwner = off(unit, "owner"), uType = off(unit, "type"), uCoords = off(unit, "coordinates");
    size_t uHome = off(unit, "home"), uPassenger = off(unit, "passengerUnit"), uHealth = off(unit, "health");
    size_t uPromotion = off(unit, "promotionLevel"), uXp = off(unit, "xp"), uMoved = off(unit, "moved");
    size_t uAttacked = off(unit, "attacked"), uCreated = off(unit, "createdTurn"), uEffects = off(unit, "effects");
    size_t iType = off(improvement, "type"), iOwner = off(improvement, "owner"), iLevel = off(improvement, "level");
    size_t iXp = off(improvement, "xp"), iPopulation = off(improvement, "population"), iProduction = off(improvement, "production");
    size_t iBorder = off(improvement, "borderSize"), iConnected = off(improvement, "connectedToCapitalOfPlayer");
    size_t iRewards = off(improvement, "rewards"), iEffects = off(improvement, "effects");
    size_t pId = off(g.player, "<Id>k__BackingField"), pTribe = off(g.player, "tribe"), pHandicap = off(g.player, "handicap");
    size_t pTech = off(g.player, "availableTech"), pScore = off(g.player, "score"), pCities = off(g.player, "cities");
    size_t pKills = off(g.player, "kills"), pCasualties = off(g.player, "casualities"), pKnown = off(g.player, "knownPlayers");
    size_t pRelations = off(g.player, "relations"), pAutoPlay = off(g.player, "<AutoPlay>k__BackingField");
    size_t resType = off(cls("GameLogicAssembly", "", "ResourceState"), "type");

    // How each command class maps onto a Cmd, found from its field names.
    struct Layout {
        int16_t type = 0;
        int a = -1, b = -1, arg = -1, argSize = 4, opponent = -1, accepted = -1;
    };
    std::unordered_map<Class*, Layout> layouts;
    std::map<uint8_t, std::vector<Pin>> researchCache;  // see Match::researchCommand
    std::map<uint8_t, Pin> endTurnCache;

    explicit Api(Game& game) : g(game) {}

    const Layout& layout(Object* cmd) {
        Class* c = object_get_class(cmd);
        auto it = layouts.find(c);
        if (it != layouts.end()) return it->second;
        Layout l;
        l.type = int16_t(unbox<uint16_t>(call(method(c, "GetCommandType", 0), cmd)));
        for (Class* k = c; k && std::string(class_get_name(k)) != "Object"; k = class_get_parent(k)) {
            void* iter = nullptr;
            while (Field* f = class_get_fields(k, &iter)) {
                if (field_get_flags(f) & 0x50) continue;
                std::string n = Reflect::cleanName(field_get_name(f));
                int o = int(field_get_offset(f));
                if (n == "Coordinates" || n == "From" || n == "Origin") l.a = o;
                else if (n == "To" || n == "Target") l.b = o;
                else if (n == "Type" || n == "Reward") l.arg = o;
                else if (n == "OpponentId") l.opponent = o;
                else if (n == "Accepted") l.accepted = o;
            }
        }
        return layouts[c] = l;
    }
};

class Match {
public:
    Api& api;
    Game& g;
    Spec spec;
    Pin gs, am;
    std::vector<Pin> legalCmds;
    std::vector<Cmd> legal;
    int width = 0, height = 0;
    std::string lastError;
    std::function<void(Object*)> onBotCommand;  // sees each command a bot seat plays, before it runs

    explicit Match(Api& a) : api(a), g(a.g) {}

    Object* state() const { return gs.get(); }
    uint8_t currentPlayer() { return g.get<uint8_t>(g.currentPlayer, state()); }
    uint32_t turn() { return at<uint32_t>(state(), api.gsTurn); }
    bool ended() { return at<uint8_t>(state(), api.gsState) == 4; }  // GameState.State.Ended
    Object* players() { return at<Object*>(state(), api.gsPlayers); }
    Object* player(uint8_t id) { return g.playerById(state(), id); }
    int seatOf(uint8_t id) { return id >= 1 && id <= 2 ? id - 1 : -1; }
    // Whether the game's AI moves now: a bot seat, or a player outside the two seats (the nature
    // player, which also takes turns and which the client's AutoPlay drives).
    bool isBotTurn() { int s = seatOf(currentPlayer()); return s < 0 || spec.seats[s].bot >= 0; }

    // The winner's player id, or 0 while the game goes on.
    uint8_t winner() {
        if (!ended()) return 0;
        Object* w = nullptr;
        return unbox<bool>(call(api.winner, state(), {&w})) && w ? at<uint8_t>(w, api.pId) : 0;
    }

    // GameSettings as the pass-and-play menus leave them (recorded from the real app, game
    // traces/hotseat1): GameType.PassAndPlay, the chosen mode as base and rules mode with that
    // mode's GameRules preset, map type and size, one PlayerData per seat, OpponentCount and
    // Difficulty untouched (0). unlockedTribes is the menus' list of owned tribes: the free ones,
    // plus any paid tribe a seat uses.
    Object* settings() {
        Object* s = make(g.settings);
        int type = 2;  // GameType.PassAndPlay
        call(method(g.settings, "set_GameType", 1), s, {&type});
        call(method(g.settings, "set_BaseGameMode", 1), s, {&spec.mode});
        call(method(g.settings, "set_RulesGameMode", 1), s, {&spec.mode});
        Object* rules = get<Object*>(s, "rules");
        call(method(object_get_class(rules), "LoadPreset", 1), rules, {&spec.mode});
        call(method(g.settings, "set_MapSize", 1), s, {&spec.mapSize});
        field_set_value(s, field(g.settings, "mapPreset"), &spec.mapPreset);
        Object* unlocked = get<Object*>(s, "unlockedTribes");
        const Method* addTribe = method(object_get_class(unlocked), "Add", 1);
        std::vector<int> owned = {0, 1, 4, 7, 10, 13};
        for (const Seat& seat : spec.seats)
            if (std::find(owned.begin(), owned.end(), seat.tribe) == owned.end()) owned.push_back(seat.tribe);
        for (int t : owned) call(addTribe, unlocked, {&t});
        const Method* addPlayer = method(g.settings, "AddPlayer", 1);
        Class* profileClass = cls("GameLogicAssembly", "", "PlayerProfileState");
        for (int i = 0; i < 2; ++i) {
            const Seat& seat = spec.seats[i];
            Object* pd = make(api.playerData);
            put<int32_t>(pd, off(api.playerData, "type"), seat.bot >= 0 ? 1 : 2);  // PlayerDataType.Bot, LocalUser
            put<int32_t>(pd, off(api.playerData, "state"), 2);
            put<bool>(pd, off(api.playerData, "knownTribe"), true);
            put<int32_t>(pd, off(api.playerData, "tribe"), seat.tribe);
            put<int32_t>(pd, off(api.playerData, "botDifficulty"), std::max(seat.bot, 0));
            std::string name = "Player " + std::to_string(i + 1);
            field_set_value(pd, field(api.playerData, "defaultName"), string_new(name.c_str()));
            Object* profile = make(profileClass);
            unsigned char guid[16] = {uint8_t(i + 1)};  // a local profile's id
            std::memcpy(reinterpret_cast<char*>(profile) + off(profileClass, "id"), guid, 16);
            field_set_value(profile, field(profileClass, "name"), string_new(name.c_str()));
            field_set_value(pd, field(api.playerData, "profile"), profile);
            call(addPlayer, s, {pd});
        }
        return s;
    }

    // The state ClientBase.CreateSession's pass-and-play branch builds, step for step, before the
    // match starts. Every random draw comes from `seeds`.
    Object* build(SeedSource& seeds) {
        Object* settingsObj = settings();
        Object* game = make(g.gameState);
        int version = 123;
        int32_t seed = seeds(), villageSeed = seeds();
        field_set_value(game, field(g.gameState, "<Version>k__BackingField"), &version);
        field_set_value(game, field(g.gameState, "<Seed>k__BackingField"), &seed);
        field_set_value(game, field(g.gameState, "<VillageNameSeed>k__BackingField"), &villageSeed);
        field_set_value(game, field(g.gameState, "<Settings>k__BackingField"), settingsObj);
        Object* list = make(g.list);
        field_set_value(game, field(g.gameState, "<PlayerStates>k__BackingField"), list);
        gs = Pin(game);
        const Method* addAI = method(g.utils, "AddAIOpponent", 4);
        const Method* getPlayer = method(g.settings, "GetPlayer", 1);
        const Method* nameOf = method(api.playerData, "GetNameInternal", 0);
        const Method* settingsPlayerCount = method(g.settings, "GetPlayerCount", 0);
        for (int i = 0; i < 2; ++i) {
            const Seat& seat = spec.seats[i];
            Object* pd = call(getPlayer, settingsObj, {&i});
            if (seat.bot < 0 && (spec.seats[0].bot >= 0 || spec.seats[1].bot >= 0)) {
                // With a bot seated, GetPlayer(i) isn't seat i: the settings hold the person's
                // PlayerData first, so a person in seat 2 read the bot's (its tribe too: every match
                // with a bot in seat 1 was a mirror until 2026-10-22). Take the person's own entry,
                // the one LocalUser.
                int n = unbox<int>(call(settingsPlayerCount, settingsObj));
                for (int k = 0; k < n; ++k) {
                    Object* c = call(getPlayer, settingsObj, {&k});
                    if (get<int32_t>(c, "type") == 2) pd = c;  // PlayerDataType.LocalUser
                }
            }
            int skin = 0;
            if (seat.bot >= 0) {
                int handicap = unbox<int>(call(g.handicapFor, nullptr, {(void*)&seat.bot}));
                if (i > 0) {
                    // The client draws the tribe from GetRandomPickableTribe; the spec fixes it.
                    call(addAI, nullptr, {game, (void*)&seat.tribe, &handicap, &skin});
                } else {
                    // The client always seats a person first, and AddAIOpponent needs a previous
                    // player for its id. A bot in seat 0 is CreateAIOpponent(1, ...), what
                    // AddAIOpponent would build for id 1.
                    int id = 1;
                    call(g.listAdd, list, {call(g.createAI, nullptr, {&id, (void*)&seat.tribe, &handicap, &skin, &skin})});
                }
            } else {
                // As CreateSession builds a local person from their PlayerData.
                Object* p = make(g.player);
                put<uint8_t>(p, api.pId, uint8_t(i + 1));
                Object* profile = get<Object*>(pd, "profile");
                alignas(8) unsigned char account[20] = {1};  // Nullable<Guid>: hasValue, then the profile id
                std::memcpy(account + 4, reinterpret_cast<char*>(profile) + 0x10, 16);
                field_set_value(p, field(g.player, "<AccountId>k__BackingField"), account);
                put<bool>(p, api.pAutoPlay, false);
                field_set_value(p, field(g.player, "<UserName>k__BackingField"), call(nameOf, pd));
                put<int32_t>(p, off(g.player, "_climate"), get<int32_t>(pd, "climate"));
                put<bool>(p, off(g.player, "hasChosenTribe"), true);
                put<int32_t>(p, api.pTribe, get<int32_t>(pd, "tribe"));
                put<int32_t>(p, off(g.player, "tribeMix"), get<int32_t>(pd, "tribeMix"));
                put<int32_t>(p, off(g.player, "skinType"), get<int32_t>(pd, "skinType"));
                call(g.listAdd, list, {p});
            }
        }
        // With no person seated the game would end at once (it ends when no AutoPlay=false player
        // is alive), so bot-only games are played to a result with AutoPlay off for everyone.
        if (spec.seats[0].bot >= 0 && spec.seats[1].bot >= 0)
            for (int i = 0; i < 2; ++i) put<bool>(listAt<Object*>(list, i), api.pAutoPlay, false);
        call(method(g.utils, "SetPlayerColors", 1), nullptr, {game});
        call(method(g.utils, "AddNaturePlayer", 1), nullptr, {game});

        int playerCount = unbox<int>(call(method(g.gameState, "get_PlayerCount", 0), game));
        int minSize = unbox<uint16_t>(call(method(cls("GameLogicAssembly", "", "MapDataExtensions"), "GetMinimumMapSize", 1), nullptr, {&playerCount}));
        uint16_t size = uint16_t(std::max(spec.mapSize, minSize));
        Object* mapObj = object_new(api.map);
        call(methodWith(api.map, ".ctor", 2, "UInt16"), mapObj, {&size, &size});
        field_set_value(game, field(g.gameState, "<Map>k__BackingField"), mapObj);
        Object* mgs = call(method(g.settings, "GetMapGeneratorSettings", 0), settingsObj);
        Object* gen = make(cls("GameLogicAssembly", "", "MapGenerator"));
        generateMap(g, gen, game, mgs, seeds);

        // Starting stars and starting unit for every real player.
        Object* data = call(method(g.gameState, "get_GameLogicData", 0), game);
        Class* gld = cls("GameLogicAssembly", "Polytopia.Data", "GameLogicData");
        const Method* tribeData = methodWith(gld, "TryGetData", 2, "TribeData");
        const Method* unitData = methodWith(gld, "TryGetData", 2, "UnitData");
        const Method* getTile = method(cls("GameLogicAssembly", "", "MapDataExtensions"), "GetTile", 2);
        const Method* train = method(cls("GameLogicAssembly", "", "ActionUtils"), "TrainUnitScored", 4);
        for (int i = 0, n = listCount(list); i < n; ++i) {
            Object* p = listAt<Object*>(list, i);
            if (at<uint8_t>(p, api.pId) == 255) continue;
            int tribe = at<int32_t>(p, api.pTribe);
            Object* td = nullptr;
            call(tribeData, data, {&tribe, &td});
            int stars = get<int32_t>(td, "startingStars");
            call(method(g.player, "set_Currency", 1), p, {&stars});
            Object* startUnit = get<Object*>(td, "startingUnit");
            int unitType = unbox<int32_t>(call(method(object_get_class(startUnit), "get_type", 0), startUnit));
            Object* ud = nullptr;
            if (!unbox<bool>(call(unitData, data, {&unitType, &ud}))) continue;
            alignas(8) unsigned char start[8];
            std::memcpy(start, reinterpret_cast<char*>(p) + off(g.player, "startTile"), 8);
            Object* u = call(train, nullptr, {game, p, call(getTile, nullptr, {mapObj, start}), ud});
            if (u) { put<bool>(u, api.uMoved, false); put<bool>(u, api.uAttacked, false); }
        }
        width = height = size;
        return game;
    }

    // A new match: the pass-and-play build with the spec's seed, then StartMatch as the client
    // runs it, then bot seats move until an outside seat must decide.
    void create(const Spec& s) {
        spec = s;
        SeedSource seeds(s.seed);
        am = Pin(g.start(build(seeds)));
        playBots();
    }

    std::string save() { StateIO io; return io.save(state()); }

    void load(const Spec& s, const std::string& bytes, int version = 123) {
        spec = s;
        StateIO io;
        gs = Pin(io.load(bytes, version));
        am = Pin(make(g.actionManager, 1, {state()}));
        Object* m = at<Object*>(state(), api.gsMap);
        width = at<uint16_t>(m, api.mapWidth);
        height = at<uint16_t>(m, api.mapHeight);
        computeLegal();
    }

    bool exploredBy(Object* tile, uint8_t player) {
        Object* explorers = at<Object*>(tile, api.tExplorers);
        for (int i = 0, n = listCount(explorers); i < n; ++i)
            if (listAt<uint8_t>(explorers, i) == player) return true;
        return false;
    }

    int16_t index(Coord c) const { return c.x < 0 || c.y < 0 || c.x >= width || c.y >= height ? -1 : int16_t(c.y * width + c.x); }

    Cmd encode(Object* cmd) {
        const Api::Layout& l = api.layout(cmd);
        Cmd c;
        c.type = l.type;
        if (l.a >= 0) c.a = index(at<Coord>(cmd, l.a));
        if (l.b >= 0) c.b = index(at<Coord>(cmd, l.b));
        if (l.arg >= 0) c.arg = int16_t(at<int32_t>(cmd, l.arg));
        if (l.opponent >= 0) c.arg = rel(at<uint8_t>(cmd, l.opponent), currentPlayer());
        if (l.accepted >= 0) c.arg = int16_t(c.arg * 2 + at<bool>(cmd, l.accepted));
        return c;
    }

    bool valid(Object* cmd) {
        Object* r = nullptr;
        return cmd && tryCall(api.isValid, cmd, {state()}, &r) && r && unbox<bool>(r);
    }

    // The game's command generators already run each command's IsValid before returning it
    // (CommandUtils.AddCommand, GetTrainableUnits), so their lists are taken as they are. Everything
    // we construct ourselves is checked: research, end turn, trigger answers, and moves and attacks,
    // since GetMovementOptions/GetAttackOptions also list targets for a unit that can no longer act.
    // strictValidation re-checks everything; envtest requires the two lists to be equal.
    //
    // Moves are checked once per unit: MoveCommand.IsValid is target-independent checks (the turn,
    // the unit on the From tile, UnitState.CanMove, its unit data) plus PathFinder.GetPath(From, To)
    // being non-empty. The targets come from the same unit's GetMovementOptions, and over ~120k
    // unit move lists (bot games and policy self-play) a unit's targets were always all valid or all
    // invalid. So the first target's full IsValid stands for all of them: late in a game this was
    // 75-80% of the legal list's cost (one path search per target). envtest's strict comparison
    // checks it, and a rejected step relists strictly (step()).
    bool strictValidation = false;
    std::unordered_set<uint64_t> legalKeys;

    static uint64_t key(const Cmd& c) {
        return uint64_t(uint16_t(c.type)) << 48 | uint64_t(uint16_t(c.a)) << 32 | uint64_t(uint16_t(c.b)) << 16 | uint16_t(c.arg);
    }

    void add(Object* cmd, bool check = true) {
        if (!cmd || ((check || strictValidation) && !valid(cmd))) return;
        Cmd c = encode(cmd);
        if (!legalKeys.insert(key(c)).second) return;  // the same command from two generators
        legal.push_back(c);
        legalCmds.emplace_back(cmd);
    }

    void addAll(Object* list) {
        for (int i = 0, n = listCount(list); i < n; ++i) add(listAt<Object*>(list, i), false);
    }

    // Research and end-turn commands hold only a player id and a tech type and are never changed
    // by execution, so one object per (player, tech) serves every match in the process.
    Object* researchCommand(uint8_t p, int tech) {
        auto& v = api.researchCache[p];
        if (v.empty()) v.resize(kTechSlots + 1);
        if (!v[tech].get()) {
            Object* r = object_new(api.cResearch);
            call(api.newResearch, r, {&p, &tech});
            v[tech] = Pin(r);
        }
        return v[tech].get();
    }

    Object* endTurnCommand(uint8_t p) {
        Pin& e = api.endTurnCache[p];
        if (!e.get()) {
            Object* c = object_new(api.cEndTurn);
            call(api.newEndTurn, c, {&p});
            e = Pin(c);
        }
        return e.get();
    }

    std::vector<Object*> tiles() {
        Object* arr = at<Object*>(at<Object*>(state(), api.gsMap), api.mapTiles);
        return std::vector<Object*>(arrayData(arr), arrayData(arr) + array_length(arr));
    }

    // Every command the current player may play now, from the game's own generators, each
    // checked with the command's own IsValid. A pending choice (city level-up reward, peace
    // request) must be answered first, as the client's popup requires.
    void computeLegal() {
        legal.clear();
        legalCmds.clear();
        legalKeys.clear();
        if (ended()) return;
        Object* gsObj = state();
        uint8_t p = currentPlayer();
        Object* me = player(p);

        alignas(8) unsigned char trigger[16] = {};
        if (unbox<bool>(call(api.pendingTrigger, gsObj, {&p, trigger}))) {
            int type = *reinterpret_cast<int32_t*>(trigger + 4);
            if (type == 1) {  // CommandTriggerType.CityLevelUp
                for (int reward = 1; reward <= 13; ++reward) {
                    Object* c = object_new(api.cCityReward);
                    call(api.newCityReward, c, {&p, &reward, trigger + 8});
                    add(c);
                }
            } else if (type == 2) {  // CommandTriggerType.PeaceRequest
                for (bool accept : {true, false}) {
                    Object* c = object_new(api.cPeaceResponse);
                    call(api.newPeaceResponse, c, {&p, trigger + 1, &accept});
                    add(c);
                }
            }
            if (!legal.empty()) return;
        }

        bool no = false;
        for (Object* t : tiles()) {
            Object* u = at<Object*>(t, api.tUnit);
            if (u && at<uint8_t>(u, api.uOwner) == p) {
                int moves = unbox<int>(call(api.movement, nullptr, {u, gsObj}));
                Object* to = call(api.moveOptions, nullptr, {u, gsObj, &moves});
                bool unitMoves = false;
                for (int i = 0, n = listCount(to); i < n; ++i) {
                    Coord c = listAt<Coord>(to, i);
                    Object* m = object_new(api.cMove);
                    call(api.newMove, m, {&p, u, &c});
                    if (strictValidation) add(m);
                    else {
                        if (i == 0) unitMoves = valid(m);
                        if (unitMoves) add(m, false);
                    }
                }
                int range = unbox<int>(call(api.range, nullptr, {u, gsObj}));
                Object* targets = call(api.attackOptions, nullptr, {u, gsObj, &range, &no});
                for (int i = 0, n = listCount(targets); i < n; ++i) {
                    Coord c = listAt<Coord>(targets, i);
                    Object* a = object_new(api.cAttack);
                    call(api.newAttack, a, {&p, u, &c});
                    add(a);
                }
                addAll(call(api.unitActions, nullptr, {gsObj, me, t, &no}));
            }
            // The client opens a tile's menus (train, build, abilities) only for tiles the player has
            // explored: tapping a hidden tile clears the selection (ClientInteraction.SelectTile,
            // Tile.get_IsHidden). The generators alone would offer roads and bridges into the fog.
            if (!exploredBy(t, p)) continue;
            addAll(call(api.trainable, nullptr, {gsObj, me, t, &no}));
            addAll(call(api.buildable, nullptr, {gsObj, me, t, &no}));
            addAll(call(api.abilities, nullptr, {gsObj, me, t, &no}));
        }
        for (int tech = 1; tech < kTechSlots; ++tech) add(researchCommand(p, tech));
        Object* ps = players();
        for (int i = 0, n = listCount(ps); i < n; ++i) {
            uint8_t opp = at<uint8_t>(listAt<Object*>(ps, i), api.pId);
            if (opp != p && opp != 255) addAll(call(api.diplomacy, nullptr, {gsObj, me, &opp, &no}));
        }
        add(endTurnCommand(p));
    }

    // The built-in AI's command for the current player, as the game drives bots.
    Object* botCommand() { return g.botCommand(state()); }

    // Where the built-in AI's choice for the current player sits in the legal list, or -1.
    int botChoice() {
        if (ended()) return -1;
        Object* cmd = botCommand();
        if (!cmd) return -1;
        Cmd c = encode(cmd);
        for (size_t i = 0; i < legal.size(); ++i)
            if (legal[i] == c) return int(i);
        return -1;
    }

    bool run(Object* cmd) {
        lastError.clear();
        return g.run(am.get(), cmd, &lastError);
    }

    // Once the win condition is met the game is in State.FinalTurn and waits for an EndMatchCommand,
    // which is valid only then and only with no choice pending. The built-in AI answers any pending
    // choice and then sends it; outside seats answer the choice themselves, then it is sent for them.
    bool finalTurn() { return at<uint8_t>(state(), api.gsState) == 3; }

    bool choicePending() {
        uint8_t p = currentPlayer();
        alignas(8) unsigned char trigger[16] = {};
        return unbox<bool>(call(api.pendingTrigger, state(), {&p, trigger}));
    }

    void endMatch() {
        uint8_t p = currentPlayer();
        Object* c = object_new(api.cEndMatch);
        call(api.newEndMatch, c, {&p});
        if (!run(c)) die("EndMatch rejected: " + lastError);
    }

    // Lets the built-in AI play every bot seat until an outside seat must decide or the game ends.
    int playBots(int limit = 100000) {
        int n = 0;
        while (!ended() && n < limit) {
            if (isBotTurn()) {
                Object* cmd = botCommand();
                if (onBotCommand) onBotCommand(cmd);
                if (!run(cmd)) die("bot command rejected: " + lastError);
            } else if (finalTurn() && !choicePending()) {
                endMatch();
            } else {
                break;
            }
            ++n;
        }
        computeLegal();
        return n;
    }

    // step(kBotOwn) plays the built-in AI's own command for the current player, even one the
    // client never offers a person (the AI's StayCommand), for generating data from bot games.
    static constexpr int kBotOwn = -2;

    // Plays legal command i for the current player, then lets bot seats move.
    bool step(int i) {
        if (i == kBotOwn) {
            if (ended()) { lastError = "game over"; return false; }
            if (!run(botCommand())) return false;
            playBots();
            return true;
        }
        if (i < 0 || i >= int(legal.size())) { lastError = "no such legal command"; return false; }
        if (!run(legalCmds[i].get())) {
            // The game rejected a listed command (a move whose own path check disagreed with its
            // unit's: never seen). The state is unchanged; list it again with every command checked.
            std::string err = lastError;
            strictValidation = true;
            computeLegal();
            strictValidation = false;
            lastError = err;
            return false;
        }
        playBots();
        return true;
    }

    // A recorded game, e.g. a replay downloaded from the game's server. Both states are blobs as
    // the server sends them and SerializationHelpers.FromByteArray reads them: an int32 version,
    // then GameState.Deserialize at that version. The start state is loaded, then the final
    // state's command stack is played one command at a time through ActionManager.
    Pin replayStack;
    int replayAt = 0, replayCount = 0;

    static int blobVersion(const std::string& blob) {
        if (blob.size() < 4) die("replay blob too short");
        int32_t v;
        std::memcpy(&v, blob.data(), 4);
        return v;
    }

    void loadReplay(const Spec& s, const std::string& start, const std::string& final) {
        load(s, start.substr(4), blobVersion(start));
        StateIO io;
        Object* last = io.load(final.substr(4), blobVersion(final));
        replayStack = Pin(get<Object*>(last, "<CommandStack>k__BackingField"));
        replayCount = g.count(replayStack.get());
        replayAt = g.count(get<Object*>(state(), "<CommandStack>k__BackingField"));
    }

    static constexpr int kNotInLegal = -1;   // the player's command is not in our legal list
    static constexpr int kNoDecision = -2;   // played by someone outside the seats, or EndMatch
    static constexpr int kOutOfTurn = -3;    // played by a seat that is not the current player

    // Plays the next recorded command. Returns its player id, and in `label` where it sits in the
    // current player's legal list (computed before it runs) or one of the codes above.
    bool replayStep(uint8_t* player, int32_t* label, Cmd* recorded) {
        if (replayAt >= replayCount) { lastError = "replay finished"; return false; }
        Object* cmd = g.item(replayStack.get(), replayAt++);
        *player = get<uint8_t>(cmd, "<PlayerId>k__BackingField");
        *recorded = encode(cmd);
        bool endMatch = object_get_class(cmd) == api.cEndMatch;
        if (seatOf(*player) < 0 || endMatch) *label = kNoDecision;
        else if (*player != currentPlayer()) *label = kOutOfTurn;
        else {
            *label = kNotInLegal;
            for (size_t i = 0; i < legal.size(); ++i)
                if (legal[i] == *recorded) { *label = int32_t(i); break; }
        }
        bool ok = run(cmd);
        computeLegal();
        return ok;
    }

    // What `viewer`'s client shows: explored tiles with their live contents, minus invisible
    // enemy units; the viewer's own economy and techs; the opponent's public facts.
    void observe(uint8_t viewer, int16_t* tilesOut, int32_t* globalOut, uint8_t* techOut) {
        Object* gsObj = state();
        std::vector<Object*> ts = tiles();
        std::fill(tilesOut, tilesOut + ts.size() * T_COUNT, int16_t(0));
        bool no = false;
        for (Object* t : ts) {
            Coord c = at<Coord>(t, api.tCoords);
            int16_t* f = tilesOut + index(c) * T_COUNT;
            Object* explorers = at<Object*>(t, api.tExplorers);
            bool seen = false;
            for (int i = 0, n = listCount(explorers); i < n && !seen; ++i) seen = listAt<uint8_t>(explorers, i) == viewer;
            if (!seen) continue;
            f[T_EXPLORED] = 1;
            f[T_TERRAIN] = int16_t(at<int32_t>(t, api.tTerrain));
            f[T_CLIMATE] = int16_t(at<int32_t>(t, api.tClimate));
            f[T_OWNER] = rel(at<uint8_t>(t, api.tOwner), viewer);
            f[T_CAPITAL_OF] = rel(at<uint8_t>(t, api.tCapitalOf), viewer);
            f[T_RULING_CITY] = index(at<Coord>(t, api.tRuling));
            Object* res = at<Object*>(t, api.tResource);
            f[T_RESOURCE] = res ? int16_t(at<int32_t>(res, api.resType)) : 0;
            f[T_ROAD] = at<bool>(t, api.tRoad);
            f[T_ROUTE] = at<bool>(t, api.tRoute);
            f[T_EFFECTS] = bits(at<Object*>(t, api.tEffects), 4);
            if (Object* imp = at<Object*>(t, api.tImprovement)) {
                f[T_IMPROVEMENT] = int16_t(at<int32_t>(imp, api.iType));
                f[T_IMP_OWNER] = rel(at<uint8_t>(imp, api.iOwner), viewer);
                f[T_IMP_LEVEL] = int16_t(at<uint16_t>(imp, api.iLevel));
                f[T_IMP_POPULATION] = at<int16_t>(imp, api.iPopulation);
                f[T_IMP_PRODUCTION] = int16_t(at<uint16_t>(imp, api.iProduction));
                f[T_IMP_BORDER] = int16_t(at<uint16_t>(imp, api.iBorder));
                f[T_IMP_CONNECTED] = rel(at<uint8_t>(imp, api.iConnected), viewer);
                f[T_IMP_REWARDS] = bits(at<Object*>(imp, api.iRewards), 4);
                f[T_IMP_EFFECTS] = bits(at<Object*>(imp, api.iEffects), 4);
                f[T_IMP_XP] = at<int16_t>(imp, api.iXp);
            }
            Object* u = call(api.visibleUnit, t, {gsObj, &viewer, &no});  // hides invisible enemy units
            if (u) {
                f[T_UNIT] = int16_t(at<int32_t>(u, api.uType) + 1);
                f[T_UNIT_OWNER] = rel(at<uint8_t>(u, api.uOwner), viewer);
                f[T_UNIT_HEALTH] = int16_t(at<uint16_t>(u, api.uHealth));
                f[T_UNIT_MAX_HEALTH] = int16_t(unbox<int>(call(api.maxHealth, nullptr, {u, gsObj})));
                f[T_UNIT_PROMOTED] = int16_t(at<uint16_t>(u, api.uPromotion));
                f[T_UNIT_MOVED] = at<bool>(u, api.uMoved);
                f[T_UNIT_ATTACKED] = at<bool>(u, api.uAttacked);
                Object* passenger = at<Object*>(u, api.uPassenger);
                f[T_UNIT_PASSENGER] = passenger ? int16_t(at<int32_t>(passenger, api.uType) + 1) : 0;
                f[T_UNIT_EFFECTS] = bits(at<Object*>(u, api.uEffects), 4);
                f[T_UNIT_HOME] = -1;
                if (f[T_UNIT_OWNER] == 1) {  // the client shows these for your own units only
                    f[T_UNIT_XP] = int16_t(at<uint16_t>(u, api.uXp));
                    f[T_UNIT_AGE] = int16_t(turn() - at<uint16_t>(u, api.uCreated));
                    f[T_UNIT_HOME] = index(at<Coord>(u, api.uHome));
                }
            }
        }

        std::fill(globalOut, globalOut + G_COUNT, 0);
        std::fill(techOut, techOut + kTechSlots, uint8_t(0));
        Object* me = player(viewer);
        globalOut[G_TURN] = int32_t(turn());
        globalOut[G_SEAT] = seatOf(viewer);
        globalOut[G_ME] = viewer;
        globalOut[G_TRIBE] = at<int32_t>(me, api.pTribe);
        globalOut[G_STARS] = unbox<int>(call(api.getCurrency, me));
        globalOut[G_INCOME] = unbox<int>(call(api.income, nullptr, {gsObj, &viewer}));
        globalOut[G_SCORE] = int32_t(at<uint32_t>(me, api.pScore));
        globalOut[G_CITIES] = at<int32_t>(me, api.pCities);
        globalOut[G_KILLS] = int32_t(at<uint32_t>(me, api.pKills));
        globalOut[G_CASUALTIES] = int32_t(at<uint32_t>(me, api.pCasualties));
        globalOut[G_HANDICAP] = at<int32_t>(me, api.pHandicap);
        Object* techs = at<Object*>(me, api.pTech);
        for (int i = 0, n = listCount(techs); i < n; ++i) {
            int t = listAt<int32_t>(techs, i);
            if (t >= 0 && t < kTechSlots) techOut[t] = 1;
        }
        Object* ps = players();
        for (int i = 0, n = listCount(ps); i < n; ++i) {
            Object* o = listAt<Object*>(ps, i);
            uint8_t id = at<uint8_t>(o, api.pId);
            if (id == viewer || id == 255) continue;
            globalOut[G_OPP_TRIBE] = at<int32_t>(o, api.pTribe);
            globalOut[G_OPP_SCORE] = int32_t(at<uint32_t>(o, api.pScore));
            Object* known = at<Object*>(me, api.pKnown);
            for (int k = 0, m = listCount(known); k < m; ++k)
                if (listAt<uint8_t>(known, k) == id) globalOut[G_OPP_KNOWN] = 1;
            relation(me, id, &globalOut[G_RELATION], &globalOut[G_MY_EMBASSY]);
            int32_t unused;
            relation(o, viewer, &unused, &globalOut[G_THEIR_EMBASSY]);
        }
        alignas(8) unsigned char trigger[16] = {};
        if (unbox<bool>(call(api.pendingTrigger, gsObj, {&viewer, trigger}))) globalOut[G_TRIGGER] = *reinterpret_cast<int32_t*>(trigger + 4);
        globalOut[G_WIDTH] = width;
        globalOut[G_HEIGHT] = height;
    }

private:
    // A small List<enum> as a bit set, for effect and reward lists.
    static int16_t bits(Object* list, int size) {
        int16_t b = 0;
        for (int i = 0, n = listCount(list); i < n; ++i) {
            int v = size == 4 ? listAt<int32_t>(list, i) : listAt<uint8_t>(list, i);
            if (v >= 0 && v < 15) b |= int16_t(1 << v);
        }
        return b;
    }

    // DiplomacyRelation of `owner` toward player `id`: its State and EmbassyLevel.
    void relation(Object* owner, uint8_t id, int32_t* state, int32_t* embassy) {
        Object* dict = at<Object*>(owner, api.pRelations);
        if (!dict) return;
        Object* r = nullptr;
        static const Method* tryGet = nullptr;
        if (!tryGet) tryGet = method(object_get_class(dict), "TryGetValue", 2);
        if (unbox<bool>(call(tryGet, dict, {&id, &r})) && r) {
            *state = get<int32_t>(r, "State");
            *embassy = get<int32_t>(r, "EmbassyLevel");
        }
    }
};

}  // namespace env
