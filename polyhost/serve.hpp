#pragma once
// A small HTTP server so a person can play a match in the browser, on the bot interface
// (env.hpp): the same game construction, the same legal moves from the game's own generators,
// moves applied by the game's ActionManager, and the game's built-in AI as the opponent.
//
//   GET  /state          map, players and the human's legal moves
//   POST /new?tribe=&opponent=&mode=&difficulty=&size=&map=
//   POST /do?i=<index into legal moves>     then the bot plays until it is your turn
//
//   GET  /games          recorded games (eval_vs_bot.py --save_games) under $POLY_GAMES (default runs/games)
//   POST /watch?path=    replay one: <dir>/<path>.start + .final (the final state's command stack)
//   POST /wstep?to=N | d=±k | turn=±1    move through it; going back replays from the start
//   POST /wview?v=0|1|2  whole map, or one player's fog
//
// difficulty -1 makes the opponent our trained network instead of the game's AI: seat 2 is then an
// outside seat, and each of its decisions is asked of py/agent_server.py on 127.0.0.1:$POLY_AGENT_PORT
// (default 8131), with the same observation and legal list the bot interface gives Python.
//   GET  /gamedata       the game's GameLogicData JSON, for names and stats

#include <arpa/inet.h>
#include <dirent.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <vector>

#include <memory>

#include "env.hpp"

struct Session {
    Game& g;
    env::Api api{g};
    std::unique_ptr<env::Match> match;
    std::vector<std::string> log;  // bot commands since the human's last move
    bool reveal = false;
    std::string lastError;
    uint64_t games = 0;
    bool netOpponent = false;  // seat 2 is played by py/agent_server.py
    int agentFd = -1;

    // Watch mode: a recorded game replayed command by command from its start state and the final
    // state's command stack (env::Match::loadReplay), the whole map visible or one player's view.
    bool watching = false;
    std::string watchPath, watchStart, watchFinal;
    int watchBase = 0, watchAt = 0, watchTotal = 0;  // stack positions: start state's, current, end
    std::vector<int> turnStarts;                     // stack positions where a player's turn begins
    uint8_t viewer = 0;                              // 0 = whole map

    // The person's own games are recorded for watch mode too: <gamesDir>/play_<date>/<time>.start/.final.
    std::string playStart, playName, playOpponent;
    int playTribes[2] = {0, 0};
    bool playRecorded = true;

    static std::string versionPrefix() {
        Class* versions = cls("GameLogicAssembly", "", "VersionManager");
        int32_t v = unbox<int>(call(method(versions, "get_GameVersion", 0), nullptr));
        return std::string(reinterpret_cast<const char*>(&v), 4);
    }

    static std::string now(const char* fmt) {
        std::time_t t = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof buf, fmt, std::localtime(&t));
        return buf;
    }

    void recordPlay(const char* result) {
        if (playRecorded || watching || playStart.empty()) return;
        playRecorded = true;
        std::string tag = "play_" + playName.substr(0, 10), name = playName.substr(11), dir = gamesDir() + "/" + tag;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream(dir + "/" + name + ".start", std::ios::binary) << playStart;
        std::ofstream(dir + "/" + name + ".final", std::ios::binary) << versionPrefix() + match->save();
        Json j;
        j.open('{').key("path").str(tag + "/" + name).key("tag").str(tag).key("time").str(playName.substr(0, 10) + " " +
            name.substr(0, 2) + ":" + name.substr(2, 2) + ":" + name.substr(4, 2))
            .key("tribes").open('[').num(playTribes[0]).num(playTribes[1]).close(']')
            .key("players").open('{').key("1").str("you").key("2").str(playOpponent).close('}')
            .key("agent").num(1).key("result").str(result).key("turns").num(match->turn()).close('}');
        std::ofstream(dir + "/games.jsonl", std::ios::app) << j.s << "\n";
    }

    static std::string gamesDir() { const char* d = std::getenv("POLY_GAMES"); return d ? d : "runs/games"; }

    static std::string readOr(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Every games.jsonl line under gamesDir()/*/, and the games sent for watching (sent.jsonl).
    static std::string gamesJson() {
        std::string out = "{\"games\":[", dir = gamesDir();
        bool first = true;
        auto add = [&](const std::string& file) {
            std::stringstream ss(readOr(file));
            for (std::string line; std::getline(ss, line);)
                if (line.size() > 2 && line[0] == '{') { out += first ? "" : ","; out += line; first = false; }
        };
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d))
                if (e->d_name[0] != '.') add(dir + "/" + e->d_name + "/games.jsonl");
            closedir(d);
        }
        out += "],\"sent\":[";
        first = true;
        add(dir + "/sent.jsonl");
        return out + "]}";
    }

    void reload() {
        log.clear();
        match = std::make_unique<env::Match>(api);
        match->loadReplay(env::Spec(), watchStart, watchFinal);
        watchBase = watchAt = match->replayAt;
        watchTotal = match->replayCount;
    }

    bool watch(const std::string& path) {
        if (path.empty() || path.find("..") != std::string::npos || path[0] == '/') { lastError = "bad path"; return false; }
        std::string s = readOr(gamesDir() + "/" + path + ".start"), f = readOr(gamesDir() + "/" + path + ".final");
        if (s.size() < 8 || f.size() < 8) { lastError = "no such game: " + path; return false; }
        watchStart = s, watchFinal = f, watchPath = path;
        watching = true, netOpponent = false, viewer = 0;
        lastError.clear();
        // One pass to find where each player's turn begins (after every EndTurn), then back to the start.
        reload();
        turnStarts = {watchBase};
        while (watchAt < watchTotal) {
            uint8_t p;
            int32_t label;
            env::Cmd c;
            if (!match->replayStep(&p, &label, &c)) break;
            watchAt = match->replayAt;
            uint8_t next = match->currentPlayer();  // after EndTurn (CommandType 15); nature's turns are skipped
            if (c.type == 15 && watchAt < watchTotal && (next == 1 || next == 2)) turnStarts.push_back(watchAt);
        }
        reload();
        return true;
    }

    // Moves to stack position `to`; the commands played on the way are the log (the last 400).
    void seek(int to) {
        to = std::max(watchBase, std::min(to, watchTotal));
        if (to < watchAt) reload();
        log.clear();
        while (watchAt < to) {
            Object* cmd = g.item(match->replayStack.get(), match->replayAt);
            uint8_t p;
            int32_t label;
            env::Cmd c;
            if (!match->replayStep(&p, &label, &c)) { lastError = match->lastError; break; }
            watchAt = match->replayAt;
            if (watchAt > to - 400) {
                Json j;
                j.open('{').key("p").num(p).key("c");
                Reflect::command(j, cmd);
                j.close('}');
                log.push_back(j.s);
            }
        }
    }

    void seekTurn(int dir) {
        if (dir > 0) {
            for (int t : turnStarts) if (t > watchAt) return seek(t);
            return seek(watchTotal);
        }
        int target = watchBase;
        for (int t : turnStarts) if (t < watchAt) target = t;
        seek(target);
    }

    const Method* explored = method(api.tile, "GetExplored", 1);
    const Method* tileUnit = method(api.tile, "get_unit", 0);
    const Method* income = api.income;
    const Method* maxHealth = api.maxHealth;
    const Method* winner = api.winner;
    Class* gld = cls("GameLogicAssembly", "Polytopia.Data", "GameLogicData");
    const Method* gameLogicData = method(g.gameState, "get_GameLogicData", 0);
    const Method* techData = method(gld, "GetTechData", 1);
    const Method* techPrice = method(gld, "GetTechPrice", 3);
    const Method* unitData = method(gld, "GetUnitData", 1);
    const Method* improvementData = method(gld, "GetImprovementData", 1);
    const Method* improvementCost = [] {
        Class* c = cls("GameLogicAssembly", "", "ResourceDataUtils");
        void* iter = nullptr;
        while (const Method* m = class_get_methods(c, &iter))  // the overload taking ImprovementData
            if (std::string(method_get_name(m)) == "GetCurrencyCost" && method_get_param_count(m) == 1) return m;
        die("no GetCurrencyCost(ImprovementData)");
    }();
    const Method* cityWork = method(cls("GameLogicAssembly", "", "TileDataExtensions"), "CalculateWork", 2);

    explicit Session(Game& game) : g(game) {}

    uint8_t human() const { return 1; }
    Object* state() const { return match->state(); }

    // A pass-and-play match as the bot interface builds it, with the person in seat 1 and the
    // game's AI in seat 2.
    void start(int tribe, int opponent, int difficulty, int mode, int mapPreset, int mapSize) {
        if (match && !watching && match->turn() > 0) recordPlay("unfinished");
        watching = false;
        env::Spec spec;
        spec.seats[0] = {tribe, -1};
        netOpponent = difficulty < 0;
        spec.seats[1] = {opponent, netOpponent ? -1 : difficulty};
        spec.mode = mode;
        spec.mapPreset = mapPreset;
        spec.mapSize = mapSize;
        spec.seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) + games++;
        log.clear();
        match = std::make_unique<env::Match>(api);
        match->onBotCommand = [this](Object* cmd) { logCommand(cmd); };
        match->create(spec);
        playStart = versionPrefix() + match->save();
        playName = now("%Y-%m-%d_%H%M%S");
        playTribes[0] = tribe, playTribes[1] = opponent;
        const char* level[] = {"bot, Easy", "bot, Normal", "bot, Hard", "bot, Crazy"};
        playOpponent = netOpponent ? "our network" : difficulty >= 0 && difficulty <= 3 ? level[difficulty] : "bot";
        playRecorded = false;
        agentMoves();
    }

    static bool sendAll(int fd, const void* p, size_t n) {
        for (size_t off = 0; off < n;) {
            ssize_t k = write(fd, static_cast<const char*>(p) + off, n - off);
            if (k <= 0) return false;
            off += size_t(k);
        }
        return true;
    }

    static bool readAll(int fd, void* p, size_t n) {
        for (size_t off = 0; off < n;) {
            ssize_t k = read(fd, static_cast<char*>(p) + off, n - off);
            if (k <= 0) return false;
            off += size_t(k);
        }
        return true;
    }

    // The network's choice for the player to move: u16 width, u16 height, u32 legal rows, then
    // the observation (tiles, globals, techs) and the legal rows, as wire.hpp sends them. Returns
    // -1 if the agent server can't be reached.
    int askAgent() {
        if (agentFd < 0) {
            const char* env = std::getenv("POLY_AGENT_PORT");
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(env ? std::atoi(env) : 8131);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            agentFd = socket(AF_INET, SOCK_STREAM, 0);
            if (connect(agentFd, (sockaddr*)&a, sizeof a)) { close(agentFd); agentFd = -1; return -1; }
        }
        std::vector<int16_t> tiles(size_t(match->width) * match->height * env::T_COUNT);
        int32_t globals[env::G_COUNT];
        uint8_t techs[env::kTechSlots];
        match->observe(match->currentPlayer(), tiles.data(), globals, techs);
        uint16_t wh[2] = {uint16_t(match->width), uint16_t(match->height)};
        uint32_t n = uint32_t(match->legal.size());
        int32_t choice = -1;
        bool ok = sendAll(agentFd, wh, sizeof wh) && sendAll(agentFd, &n, sizeof n) &&
                  sendAll(agentFd, tiles.data(), tiles.size() * sizeof(int16_t)) && sendAll(agentFd, globals, sizeof globals) &&
                  sendAll(agentFd, techs, sizeof techs) && sendAll(agentFd, match->legal.data(), n * sizeof(env::Cmd)) &&
                  readAll(agentFd, &choice, sizeof choice);
        if (!ok) { close(agentFd); agentFd = -1; return -1; }
        return choice;
    }

    // Plays the network's seat until it is the person's turn again. If the agent server is down,
    // the game's AI moves for it, and lastError says so.
    void agentMoves() {
        for (int k = 0; netOpponent && !match->ended() && match->currentPlayer() == 2 && k < 2000; ++k) {
            int i = askAgent();
            if (i < 0 || i >= int(match->legal.size())) {
                lastError = "agent server not reachable on POLY_AGENT_PORT; the game's AI moved instead";
                i = match->botChoice();
                if (i < 0) { match->step(env::Match::kBotOwn); continue; }
            }
            logCommand(match->legalCmds[size_t(i)].get());
            if (!match->step(i)) { lastError = "agent: " + match->lastError; return; }
        }
    }

    void logCommand(Object* cmd) {
        Json j;
        j.open('{').key("p").num(match->currentPlayer()).key("c");
        Reflect::command(j, cmd);
        j.close('}');
        log.push_back(j.s);
    }

    std::vector<Object*> tiles() { return match->tiles(); }

    bool play(size_t i) {
        if (watching) { lastError = "watching a recorded game; start a new game to play"; return false; }
        if (i >= match->legal.size()) { lastError = "not available"; return false; }
        log.clear();
        if (!match->step(int(i))) { lastError = match->lastError; return false; }
        lastError.clear();
        agentMoves();
        if (match->ended()) recordPlay(match->winner() == human() ? "win" : "loss");
        return true;
    }

    // Star price of a research, build or train command, from the game's own pricing.
    int cost(Object* cmd) {
        std::string name = class_get_name(object_get_class(cmd));
        if (name != "ResearchCommand" && name != "BuildCommand" && name != "TrainCommand") return -1;
        int type = get<int32_t>(cmd, "<Type>k__BackingField");
        Object* data = call(gameLogicData, state());
        Object* r = nullptr;
        if (name == "ResearchCommand") {
            Object* tech = call(techData, data, {&type});
            return tryCall(techPrice, data, {tech, g.playerById(state(), human()), state()}, &r) && r ? unbox<int>(r) : -1;
        }
        if (name == "BuildCommand") {
            Object* imp = call(improvementData, data, {&type});
            return tryCall(improvementCost, nullptr, {imp}, &r) && r ? unbox<int>(r) : -1;
        }
        return get<int32_t>(call(unitData, data, {&type}), "cost");
    }

    void unitJson(Json& j, Object* u) {
        Object* r = nullptr;
        int maxHp = tryCall(maxHealth, nullptr, {u, state()}, &r) && r ? unbox<int>(r) : 0;
        j.open('{').key("id").num(get<uint32_t>(u, "id")).key("type").num(get<int32_t>(u, "type"))
            .key("owner").num(get<uint8_t>(u, "owner")).key("hp").num(get<uint16_t>(u, "health")).key("maxHp").num(maxHp)
            .key("moved").boolean(get<bool>(u, "moved")).key("attacked").boolean(get<bool>(u, "attacked"))
            .key("promotion").num(get<uint16_t>(u, "promotionLevel")).key("xp").num(get<uint16_t>(u, "xp"));
        Object* passenger = get<Object*>(u, "passengerUnit");
        j.key("passenger");
        passenger ? unitJson(j, passenger) : (void)j.null();
        j.close('}');
    }

    std::string stateJson() {
        Object* state = this->state();
        Json j;
        j.open('{');
        j.key("turn").num(g.get<uint32_t>(g.currentTurn, state));
        j.key("currentPlayer").num(g.get<uint8_t>(g.currentPlayer, state));
        j.key("state").num(g.state(state));
        j.key("human").num(watching ? (viewer ? viewer : 1) : human());
        if (watching) {
            j.key("watch").open('{').key("path").str(watchPath).key("at").num(watchAt).key("base").num(watchBase)
                .key("total").num(watchTotal).key("viewer").num(viewer).key("turnStarts").open('[');
            for (int t : turnStarts) j.num(t);
            j.close(']').close('}');
        }
        j.key("mode").num(match->spec.mode);
        j.key("reveal").boolean(reveal);
        Object* won = nullptr;
        if (g.state(state) == 4 && unbox<bool>(call(winner, state, {&won})) && won) j.key("winner").num(g.get<uint8_t>(g.playerId, won));
        if (!lastError.empty()) j.key("error").str(lastError);

        j.key("players").open('[');
        Object* ps = call(g.playerStates, state);
        for (int i = 0, n = g.count(ps); i < n; ++i) {
            Object* p = g.item(ps, i);
            uint8_t id = g.get<uint8_t>(g.playerId, p);
            j.open('{').key("id").num(id).key("tribe").num(get<int32_t>(p, "tribe"))
                .key("stars").num(unbox<int>(call(method(g.player, "get_Currency", 0), p)))
                .key("income").num(unbox<int>(call(income, nullptr, {state, &id})))
                .key("score").num(get<uint32_t>(p, "score")).key("cities").num(get<int32_t>(p, "cities"))
                .key("kills").num(get<uint32_t>(p, "kills"));
            j.key("techs").open('[');
            Object* techs = get<Object*>(p, "availableTech");
            for (int k = 0, m = techs ? g.count(techs) : 0; k < m; ++k) j.num(unbox<int32_t>(g.item(techs, k)));
            j.close(']').close('}');
        }
        j.close(']');

        Object* map = call(method(g.gameState, "get_Map", 0), state);
        j.key("width").num(unbox<int>(call(method(object_get_class(map), "get_Width", 0), map)));
        j.key("height").num(unbox<int>(call(method(object_get_class(map), "get_Height", 0), map)));
        j.key("tiles").open('[');
        uint8_t h = watching ? viewer : human();
        for (Object* t : tiles()) {
            const int32_t* c = reinterpret_cast<const int32_t*>(reinterpret_cast<char*>(t) + 0x10);
            j.open('{').key("x").num(c[0]).key("y").num(c[1]);
            bool seen = (reveal && !watching) || !h || unbox<bool>(call(explored, t, {&h}));
            if (!seen) { j.key("fog").boolean(true).close('}'); continue; }
            j.key("terrain").num(get<int32_t>(t, "terrain")).key("climate").num(get<int32_t>(t, "climate"))
                .key("owner").num(get<uint8_t>(t, "owner")).key("capitalOf").num(get<uint8_t>(t, "capitalOf"))
                .key("road").boolean(get<bool>(t, "hasRoad")).key("route").boolean(get<bool>(t, "hasRoute"));
            Object* res = get<Object*>(t, "resource");
            if (res) j.key("resource").num(get<int32_t>(res, "type"));
            Object* imp = get<Object*>(t, "improvement");
            if (imp) {
                String* name = get<String*>(imp, "name");
                j.key("improvement").open('{').key("type").num(get<int32_t>(imp, "type")).key("owner").num(get<uint8_t>(imp, "owner"))
                    .key("level").num(get<uint16_t>(imp, "level")).key("population").num(get<int16_t>(imp, "population"))
                    .key("production").num(get<uint16_t>(imp, "production")).key("xp").num(get<int16_t>(imp, "xp"))
                    .key("border").num(get<uint16_t>(imp, "borderSize")).key("name").str(name ? utf8(name) : "")
                    .key("stars").num(unbox<int>(call(cityWork, nullptr, {t, state}))).close('}');
            }
            Object* u = call(tileUnit, t);
            if (u) { j.key("unit"); unitJson(j, u); }
            j.close('}');
        }
        j.close(']');

        j.key("legal").open('[');
        for (size_t i = 0; i < (watching ? 0 : match->legal.size()); ++i) {
            Object* cmd = match->legalCmds[i].get();
            Reflect::command(j, cmd);
            j.s.pop_back();
            j.first = false;
            int c = cost(cmd);
            if (c >= 0) j.key("cost").num(c);
            j.key("ok").boolean(true).close('}');
        }
        j.close(']');
        j.key("techPrices").open('{');
        {
            Object* data = call(gameLogicData, state);
            Object* me = g.playerById(state, human());
            for (int tech = 0; tech < 80; ++tech) {
                Object* td = nullptr;
                Object* price = nullptr;
                if (!tryCall(techData, data, {&tech}, &td) || !td) continue;
                if (tryCall(techPrice, data, {td, me, state}, &price) && price)
                    j.key(std::to_string(tech).c_str()).num(unbox<int>(price));
            }
        }
        j.close('}');
        j.key("log").open('[');
        for (auto& l : log) j.raw(l);
        j.close(']');
        j.close('}');
        return j.s;
    }
};

inline std::string urlDecode(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size()) { out += char(std::stoi(v.substr(i + 1, 2), nullptr, 16)); i += 2; }
        else out += v[i] == '+' ? ' ' : v[i];
    }
    return out;
}

struct Request {
    std::string method, path;
    std::map<std::string, std::string> query;
};

inline Request parseRequest(const std::string& head) {
    Request r;
    size_t a = head.find(' '), b = head.find(' ', a + 1);
    r.method = head.substr(0, a);
    std::string target = head.substr(a + 1, b - a - 1);
    size_t q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string::npos) {
        std::stringstream ss(target.substr(q + 1));
        std::string kv;
        while (std::getline(ss, kv, '&')) {
            size_t e = kv.find('=');
            if (e != std::string::npos) r.query[kv.substr(0, e)] = kv.substr(e + 1);
        }
    }
    return r;
}

inline void respond(int fd, int code, const char* type, const std::string& body) {
    std::string head = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") +
                       "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    std::string all = head + body;
    for (size_t off = 0; off < all.size();) {
        ssize_t n = write(fd, all.data() + off, all.size() - off);
        if (n <= 0) break;
        off += size_t(n);
    }
}

inline const char* contentType(const std::string& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js")) return "text/javascript; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    return "application/octet-stream";
}

inline void serve(int port) {
    Game g;
    Session session(g);
    session.start(7, 13, 1, 4, 2, 14);  // Imperius vs a Normal Xin-xi bot, Might, Lakes, Small

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) || listen(srv, 16)) die("cannot listen on port " + std::to_string(port));
    std::printf("serving on 127.0.0.1:%d\n", port);
    std::fflush(stdout);

    while (true) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        std::string buf;
        char chunk[4096];
        while (buf.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = read(fd, chunk, sizeof chunk);
            if (n <= 0) break;
            buf.append(chunk, size_t(n));
        }
        Request r = parseRequest(buf.substr(0, buf.find("\r\n")));
        auto arg = [&](const char* k, int def) { return r.query.count(k) ? std::atoi(r.query[k].c_str()) : def; };
        if (r.path == "/state") {
            respond(fd, 200, "application/json", session.stateJson());
        } else if (r.path == "/do" && r.method == "POST") {
            session.play(size_t(arg("i", -1)));
            respond(fd, 200, "application/json", session.stateJson());
        } else if (r.path == "/new" && r.method == "POST") {
            session.reveal = false;
            session.start(arg("tribe", 7), arg("opponent", 13), arg("difficulty", 1), arg("mode", 4), arg("map", 2), arg("size", 14));
            respond(fd, 200, "application/json", session.stateJson());
        } else if (r.path == "/reveal" && r.method == "POST") {
            session.reveal = arg("on", 0);
            respond(fd, 200, "application/json", session.stateJson());
        } else if (r.path == "/games") {
            respond(fd, 200, "application/json", Session::gamesJson());
        } else if (r.path == "/watch" && r.method == "POST") {
            session.watch(urlDecode(r.query["path"]));
            respond(fd, 200, "application/json", session.stateJson());
        } else if (r.path == "/wstep" && r.method == "POST" && session.watching) {
            if (r.query.count("to")) session.seek(arg("to", 0));
            else if (r.query.count("turn")) session.seekTurn(arg("turn", 1));
            else session.seek(session.watchAt + arg("d", 1));
            respond(fd, 200, "application/json", session.stateJson());
        } else if (r.path == "/wview" && r.method == "POST") {
            session.viewer = uint8_t(arg("v", 0));
            respond(fd, 200, "application/json", session.stateJson());
        } else if (r.path == "/gamedata") {
            respond(fd, 200, "application/json", readFile(kRoot + "/gamedata/GameLogicData28.txt"));
        } else {
            std::string file = r.path == "/" ? "/index.html" : r.path;
            if (file.find("..") != std::string::npos) { respond(fd, 404, "text/plain", "not found"); close(fd); continue; }
            std::ifstream in(kRoot + "/web" + file, std::ios::binary);
            if (!in) respond(fd, 404, "text/plain", "not found");
            else { std::stringstream ss; ss << in.rdbuf(); respond(fd, 200, contentType(file), ss.str()); }
        }
        close(fd);
    }
}
