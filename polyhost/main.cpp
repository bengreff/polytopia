// polyhost: runs The Battle of Polytopia's own rules code headlessly.
//
//   polyhost symbols <out.tsv>   write every method's offset and full name, for Ghidra
//   polyhost bench <games>       play built-in bot vs bot championship games and report speed
//   polyhost serve [port]        play against the built-in bot in the browser
//   polyhost state               print the opening state of a match as JSON
//   polyhost env [port]          serve the bot interface (py/polyenv.py)
//   polyhost envtest <games> [seed] [every] [turn cap]   check the bot interface against the game
//   polyhost envbench <games>    time the bot interface's parts on championship games
//   polyhost fogtest <games> [seed] [every]   the fog-leak test: hidden changes must not reach the observation

#include "envtest.hpp"
#include "serve.hpp"
#include "wire.hpp"

using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static void dumpSymbols(const char* outPath) {
    FILE* out = std::fopen(outPath, "w");
    if (!out) die(std::string("cannot write ") + outPath);
    size_t nAsm = 0;
    const Assembly** as = domain_get_assemblies(domain_get(), &nAsm);
    size_t count = 0;
    for (size_t a = 0; a < nAsm; ++a) {
        const Image* img = assembly_get_image(as[a]);
        size_t nCls = image_get_class_count(img);
        for (size_t c = 0; c < nCls; ++c) {
            Class* k = const_cast<Class*>(image_get_class(img, c));
            std::string name = class_get_name(k);
            for (Class* d = class_get_declaring_type(k); d; d = class_get_declaring_type(d))
                name = std::string(class_get_name(d)) + "." + name;
            std::string ns = class_get_namespace(k);
            if (!ns.empty()) name = ns + "." + name;
            void* iter = nullptr;
            while (const Method* m = class_get_methods(k, &iter)) {
                uintptr_t off = rva(m);
                if (!off) continue;
                std::fprintf(out, "%lx\t%s\t%s$$%s/%u\n", (unsigned long)off, image_get_name(img), name.c_str(),
                             method_get_name(m), method_get_param_count(m));
                ++count;
            }
        }
    }
    std::fclose(out);
    std::printf("wrote %zu methods to %s\n", count, outPath);
}

// Built-in AI vs built-in AI on the bot interface's championship games (Crazy vs Crazy).
static void bench(int games) {
    Game g;
    env::Api api(g);
    double totalMs = 0;
    long totalCommands = 0, totalTurns = 0;
    for (int k = 0; k < games; ++k) {
        auto t0 = Clock::now();
        env::Spec spec;
        spec.seed = 5000 + k;
        spec.seats[0] = {k % 2 ? 13 : 7, 3};
        spec.seats[1] = {k % 2 ? 4 : 10, 3};
        env::Match m(api);
        long commands = 0;
        m.onBotCommand = [&](Object*) { ++commands; };
        m.create(spec);
        double ms = since(t0);
        totalMs += ms;
        totalCommands += commands;
        totalTurns += m.turn();
        std::printf("game %d: %s after %u turns, winner %d, %ld commands, %.0f ms\n", k, m.ended() ? "ended" : "unfinished",
                    m.turn(), m.winner(), commands, ms);
    }
    std::printf("%d games: %.1f ms/game, %.0f commands/s, %.1f turns/game (1 thread)\n", games, totalMs / games,
                totalCommands / (totalMs / 1000), double(totalTurns) / games);
}

// Times each part of the bot interface over championship games. Both seats are driven from
// outside, choosing what the built-in AI would (Normal, since a person's seat has handicap 1).
static void envBench(int games) {
    Game g;
    env::Api api(g);
    double legalMs = 0, obsMs = 0, botMs = 0, stepMs = 0, saveMs = 0, loadMs = 0, createMs = 0, totalMs = 0;
    long decisions = 0, legalCount = 0, turns = 0;
    std::vector<int16_t> tiles;
    int32_t globals[env::G_COUNT];
    uint8_t techs[env::kTechSlots];
    for (int k = 0; k < games; ++k) {
        auto t0 = Clock::now();
        env::Spec spec;
        spec.seed = 1000 + k;
        spec.seats[0].tribe = k % 2 ? 13 : 7;
        spec.seats[1].tribe = k % 2 ? 4 : 10;
        env::Match m(api);
        auto t = Clock::now();
        m.create(spec);
        createMs += since(t);
        tiles.resize(size_t(m.width) * m.height * env::T_COUNT);
        while (!m.ended() && m.turn() < 100) {
            t = Clock::now();
            m.observe(m.currentPlayer(), tiles.data(), globals, techs);
            obsMs += since(t);
            t = Clock::now();
            int i = m.botChoice();
            botMs += since(t);
            if (decisions % 50 == 0) {
                t = Clock::now();
                std::string bytes = m.save();
                saveMs += since(t);
                t = Clock::now();
                env::Match copy(api);
                copy.load(spec, bytes);
                loadMs += since(t);
            }
            legalCount += long(m.legal.size());
            t = Clock::now();
            if (!m.run(i >= 0 ? m.legalCmds[i].get() : m.botCommand())) die("rejected: " + m.lastError);  // -1: the AI's StayCommand
            if (m.finalTurn() && !m.choicePending()) m.endMatch();
            stepMs += since(t);
            t = Clock::now();
            m.computeLegal();
            legalMs += since(t);
            ++decisions;
        }
        turns += m.turn();
        totalMs += since(t0);
        std::printf("game %d: %s at turn %u, winner %d\n", k, m.ended() ? "ended" : "turn cap", m.turn(), m.winner());
    }
    long saves = (decisions + 49) / 50;
    std::printf("%d games, %ld decisions, %.1f turns/game, %.1f legal commands/decision\n", games, decisions, double(turns) / games,
                double(legalCount) / decisions);
    std::printf("per decision: legal list %.3f ms, observation %.3f ms, built-in AI %.3f ms, execute %.3f ms\n",
                legalMs / decisions, obsMs / decisions, botMs / decisions, stepMs / decisions);
    std::printf("save %.3f ms, load (incl. legal list) %.3f ms, create %.1f ms/game\n", saveMs / saves, loadMs / saves, createMs / games);
    std::printf("total %.1f s, %.0f decisions/s on one thread\n", totalMs / 1000, decisions / (totalMs / 1000));
}

int main(int argc, char** argv) {
    if (argc < 2) die("usage: polyhost symbols <out> | bench <games> | serve [port]");
    std::string cmd = argv[1];
    auto t0 = Clock::now();
    boot();
    if (const char* wait = std::getenv("POLY_WAIT")) {  // seconds to wait after boot, so Frida can attach
        std::fprintf(stderr, "pid %d waiting %s s\n", getpid(), wait);
        sleep(std::atoi(wait));
    }
    std::printf("il2cpp up in %.0f ms\n", since(t0));
    Class* versions = cls("GameLogicAssembly", "", "VersionManager");
    std::printf("game version %d, logic data version %d\n",
                unbox<int>(call(method(versions, "get_GameVersion", 0), nullptr)),
                unbox<int>(call(method(versions, "get_GameLogicDataVersion", 0), nullptr)));

    if (cmd == "handicaps") {  // the game's BotDifficulty -> handicap table
        const Method* h = method(cls("GameLogicAssembly", "", "GameSettings"), "HandicapFromDifficulty", 1);
        for (int d = 0; d < 5; ++d) std::printf("difficulty %d -> handicap %d\n", d, unbox<int>(call(h, nullptr, {&d})));
        return 0;
    }
    if (cmd == "symbols") {
        dumpSymbols(argc > 2 ? argv[2] : (kRoot + "/symbols.tsv").c_str());
        return 0;
    }
    loadGameData();
    if (cmd == "bench") bench(argc > 2 ? std::atoi(argv[2]) : 1);
    else if (cmd == "state") {  // print the opening state of a human-vs-bot match, for debugging
        Game g;
        Session session(g);
        session.start(7, 13, 1, 4, 2, 14);
        std::printf("%s\n", session.stateJson().c_str());
    } else if (cmd == "dump") {  // dump <state.bin> [version]: a recorded GameState as JSON
        StateIO io;
        std::fprintf(stderr, "loading\n");
        Object* gs = io.load(readFile(argv[2]), argc > 3 ? std::atoi(argv[3]) : 123);
        std::fprintf(stderr, "loaded, dumping\n");
        Dumper d;
        d.object(gs, 0);
        std::printf("%s\n", d.j.s.c_str());
    } else if (cmd == "replay") {
        // replay <start.bin> <final.bin> <snapshots.txt> [json_dir]: execute the final state's
        // command stack from the start state with ActionManager, and after each command compare our
        // state byte for byte with the real snapshot taken at the same command index.
        StateIO io;
        Game g;
        Object* ours = io.load(readFile(argv[2]), 123);
        Object* finalState = io.load(readFile(argv[3]), 123);
        Object* stack = get<Object*>(finalState, "<CommandStack>k__BackingField");
        std::map<int, std::string> snaps;  // CurrentCommand -> serialized real state
        std::ifstream list(argv[4]);
        for (std::string path; std::getline(list, path);) {
            if (path.empty()) continue;
            std::string bytes = readFile(path);
            Object* s = io.load(bytes, 123);
            snaps[unbox<uint16_t>(call(method(g.gameState, "get_CurrentCommand", 0), s))] = bytes;
        }
        Object* am = make(g.actionManager, 1, {ours});
        int n = g.count(stack), compared = 0, matched = 0;
        // With POLY_CHECK_AI set, before each bot command ask the built-in AI, driven the way our
        // engine drives bots, for its choice and compare it with what the real bot played.
        bool checkAI = std::getenv("POLY_CHECK_AI");
        int aiChecked = 0, aiMatched = 0;
        auto commandJson = [](Object* c) { Json j; Reflect::command(j, c); return j.s; };
        std::printf("%d commands, %zu snapshots\n", n, snaps.size());
        for (int i = 0; i < n; ++i) {
            Object* cmd = g.item(stack, i);
            if (checkAI && i > 0) {
                uint8_t pid = get<uint8_t>(cmd, "<PlayerId>k__BackingField");
                Object* player = g.playerById(ours, pid);
                if (unbox<bool>(call(method(g.player, "get_AutoPlay", 0), player)) && pid != 255) {
                    std::string want = commandJson(cmd), got = commandJson(g.botCommand(ours));
                    ++aiChecked;
                    if (want == got) ++aiMatched;
                    else if (aiChecked - aiMatched <= 5) std::printf("AI differs at command %d:\n  real %s\n  ours %s\n", i, want.c_str(), got.c_str());
                }
            }
            std::string err;
            bool ok = g.run(am, cmd, &err);
            int at = unbox<uint16_t>(call(method(g.gameState, "get_CurrentCommand", 0), ours));
            auto it = snaps.find(at);
            if (!ok) std::printf("command %d (%s) rejected: %s\n", i, class_get_name(object_get_class(cmd)), err.c_str());
            if (it == snaps.end()) continue;
            ++compared;
            std::string mine = io.save(ours);
            if (mine == it->second) { ++matched; continue; }
            // Byte differences include UI side effects (lazily created lookups, advisor AI state),
            // so write both states as JSON for a content comparison (tools/statecompare.py).
            if (argc > 5) {
                std::string dir = argv[5];
                Dumper a, b;
                a.object(io.load(mine, 123), 0);  // through save/load like the real snapshot, so unsaved fields match
                b.object(io.load(it->second, 123), 0);
                std::ofstream(dir + "/ours_" + std::to_string(at) + ".json") << a.j.s;
                std::ofstream(dir + "/real_" + std::to_string(at) + ".json") << b.j.s;
            }
        }
        std::printf("compared %d states, %d byte-identical\n", compared, matched);
        if (checkAI) std::printf("bot commands: %d checked, %d chosen identically by our engine's bot\n", aiChecked, aiMatched);
    } else if (cmd == "fresh") {
        // fresh <real_start.bin> <out.bin> <map_seeds_file> <mode> <opponents> <difficulty> <map>:
        // build a game from menu choices with localPlayer/menuSettings (not copied from the
        // recording), using the recording's seeds and bot tribes, and compare.
        StateIO io;
        Game g;
        std::string realBytes = readFile(argv[2]);
        Object* real = io.load(realBytes, 123);
        MenuChoice c;
        c.mode = std::atoi(argv[5]); c.opponents = std::atoi(argv[6]); c.difficulty = std::atoi(argv[7]); c.mapPreset = std::atoi(argv[8]);
        Object* settings = menuSettings(g, c);
        // The game's name is random flavour text from the menu; take the recording's.
        Field* nameField = field(g.settings, "<GameName>k__BackingField");
        Object* name = nullptr;
        field_get_value(get<Object*>(real, "<Settings>k__BackingField"), nameField, &name);
        field_set_value(settings, nameField, name);
        Object* realPlayers = call(g.playerStates, real);
        Object* own = localPlayer(g, get<int32_t>(g.item(realPlayers, 0), "tribe"));
        SeedSource seeds(0);
        seeds.replay = {get<int32_t>(real, "<Seed>k__BackingField"), get<int32_t>(real, "<VillageNameSeed>k__BackingField")};
        std::ifstream in(argv[4]);
        for (int x; in >> x;) seeds.replay.push_back(x);
        std::vector<int> tribes;
        for (int i = 1, n = g.count(realPlayers); i < n; ++i)
            if (g.get<uint8_t>(g.playerId, g.item(realPlayers, i)) != 255) tribes.push_back(get<int32_t>(g.item(realPlayers, i), "tribe"));
        Object* ours = createLocalGame(g, 123, settings, own, seeds, tribes);
        String* alias = string_new("Anonymous");  // AccountManager.AliasInternal offline
        field_set_value(g.item(call(g.playerStates, ours), 0), field(g.player, "<UserName>k__BackingField"), alias);
        std::string ourBytes = io.save(ours);
        std::ofstream(argv[3], std::ios::binary) << ourBytes;
        std::printf(ourBytes == realBytes ? "IDENTICAL: %zu bytes\n" : "DIFFERENT (%zu bytes)\n", ourBytes.size());
    } else if (cmd == "newplayer") {  // the defaults of a freshly constructed PlayerState
        Game g;
        Dumper d;
        d.object(make(g.player), 0);
        std::printf("%s\n", d.j.s.c_str());
    } else if (cmd == "scores") {  // scores <state.bin>...: each player's score, per recorded state
        StateIO io;
        Game g;
        for (int a = 2; a < argc; ++a) {
            Object* gs = io.load(readFile(argv[a]), 123);
            Object* ps = call(g.playerStates, gs);
            std::printf("%s:", argv[a]);
            for (int i = 0, n = g.count(ps); i < n; ++i) std::printf(" %u", get<uint32_t>(g.item(ps, i), "score"));
            std::printf("\n");
        }
    } else if (cmd == "regen") {
        // regen <real_start.bin> <our_start.bin> <map_seeds_file>: rebuild a game the real app created, from its own
        // settings, human player identity and seeds, with createLocalGame; save it and say whether
        // it matches byte for byte.
        StateIO io;
        Game g;
        std::string realBytes = readFile(argv[2]);
        Object* real = io.load(realBytes, 123);
        Object* settings = get<Object*>(real, "<Settings>k__BackingField");
        Object* realOwn = g.item(call(g.playerStates, real), 0);
        Object* own = make(g.player);
        for (const char* f : {"<Id>k__BackingField", "<UserName>k__BackingField", "<AccountId>k__BackingField", "<AutoPlay>k__BackingField",
                              "tribe", "tribeMix", "_climate", "hasChosenTribe", "handicap", "_skin", "skinType"}) {
            Field* fd = class_get_field_from_name(g.player, f);
            if (!fd) continue;
            alignas(8) unsigned char buf[32] = {};
            field_get_value(realOwn, fd, buf);
            field_set_value(own, fd, buf);
        }
        int version = get<int32_t>(real, "<Version>k__BackingField");
        // The recorded seeds, in the order the game drew them: Seed, VillageNameSeed, then the
        // map attempts (argv[4], a file; its final, repeated entry is the regeneration).
        SeedSource seeds(0);
        seeds.replay = {get<int32_t>(real, "<Seed>k__BackingField"), get<int32_t>(real, "<VillageNameSeed>k__BackingField")};
        if (argc > 4) {
            std::ifstream in(argv[4]);
            for (int s; in >> s;) seeds.replay.push_back(s);
        }
        std::vector<int> tribes;
        Object* realPlayers = call(g.playerStates, real);
        for (int i = 1, n = g.count(realPlayers); i < n; ++i) {
            Object* p = g.item(realPlayers, i);
            if (g.get<uint8_t>(g.playerId, p) != 255) tribes.push_back(get<int32_t>(p, "tribe"));
        }
        Object* ours = createLocalGame(g, version, settings, own, seeds, tribes);
        // ClientBase.CreateSession names the local player after CreateLocalGame returns.
        Field* nameField = field(g.player, "<UserName>k__BackingField");
        Object* name = nullptr;
        field_get_value(realOwn, nameField, &name);
        field_set_value(g.item(call(g.playerStates, ours), 0), nameField, name);
        std::string ourBytes = io.save(ours);
        std::ofstream(argv[3], std::ios::binary) << ourBytes;
        size_t firstDiff = 0;
        while (firstDiff < std::min(ourBytes.size(), realBytes.size()) && ourBytes[firstDiff] == realBytes[firstDiff]) ++firstDiff;
        if (ourBytes == realBytes) std::printf("IDENTICAL: %zu bytes\n", ourBytes.size());
        else std::printf("DIFFERENT: ours %zu bytes, real %zu bytes, first difference at byte %zu\n", ourBytes.size(), realBytes.size(), firstDiff);
    } else if (cmd == "serve") serve(argc > 2 ? std::atoi(argv[2]) : 8123);
    else if (cmd == "hotseatcheck") {
        // hotseatcheck <real.bin> <map_seeds_file>: rebuild a pass-and-play game the real app created
        // with the bot interface's own construction (env::Match::build), from the recording's seeds
        // and tribes, and compare it with the recording byte for byte.
        StateIO io;
        Game g;
        env::Api api(g);
        Object* real = io.load(readFile(argv[2]), 123);
        Object* realSettings = get<Object*>(real, "<Settings>k__BackingField");
        Object* realPlayers = call(g.playerStates, real);
        env::Match m(api);
        for (int i = 0; i < 2; ++i) m.spec.seats[i].tribe = get<int32_t>(g.item(realPlayers, i), "tribe");
        m.spec.mode = get<int32_t>(realSettings, "baseGameMode");
        m.spec.mapPreset = get<int32_t>(realSettings, "mapPreset");
        m.spec.mapSize = get<int32_t>(realSettings, "<MapSize>k__BackingField");
        SeedSource seeds(0);
        seeds.replay = {get<int32_t>(real, "<Seed>k__BackingField"), get<int32_t>(real, "<VillageNameSeed>k__BackingField")};
        std::ifstream in(argv[3]);
        for (int x; in >> x;) seeds.replay.push_back(x);
        Object* ours = m.build(seeds);
        // Identity the menus and local profiles supply, with no part in the rules: the game's
        // random flavour name and each person's profile id (names are the profiles' defaults).
        Field* gameName = field(g.settings, "<GameName>k__BackingField");
        Object* name = nullptr;
        field_get_value(realSettings, gameName, &name);
        field_set_value(get<Object*>(ours, "<Settings>k__BackingField"), gameName, name);
        Object* ourPlayers = call(g.playerStates, ours);
        for (int i = 0; i < 2; ++i)
        {
            alignas(8) unsigned char buf[32] = {};
            Field* f = field(g.player, "<AccountId>k__BackingField");
            field_get_value(g.item(realPlayers, i), f, buf);
            field_set_value(g.item(ourPlayers, i), f, buf);
        }
        // The recording was taken as the second starting unit was trained, before the client
        // clears that unit's moved and attacked flags.
        Object* map = get<Object*>(real, "<Map>k__BackingField");
        Object* tiles = get<Object*>(map, "tiles");
        for (uint32_t t = 0; t < array_length(tiles); ++t)
            if (Object* u = get<Object*>(arrayData(tiles)[t], "<unit>k__BackingField")) {
                bool no = false;
                field_set_value(u, field(api.unit, "moved"), &no);
                field_set_value(u, field(api.unit, "attacked"), &no);
            }
        std::string a = io.save(io.load(io.save(ours), 123)), b = io.save(real);
        size_t d = 0;
        while (d < std::min(a.size(), b.size()) && a[d] == b[d]) ++d;
        if (a == b) std::printf("IDENTICAL: %zu bytes\n", a.size());
        else {
            std::printf("DIFFERENT: ours %zu bytes, real %zu bytes, first difference at byte %zu\n", a.size(), b.size(), d);
            Dumper da, db;
            da.object(io.load(a, 123), 0);
            db.object(io.load(b, 123), 0);
            std::ofstream(kRoot + "/hs_ours.json") << da.j.s;
            std::ofstream(kRoot + "/hs_real.json") << db.j.s;
        }
    } else if (cmd == "envdebug") {  // what the unit generators return for each own unit, a few turns in
        Game g;
        env::Api api(g);
        env::Spec spec;
        spec.seed = 1;
        env::Match m(api);
        m.create(spec);
        for (int k = 0; k < (argc > 2 ? std::atoi(argv[2]) : 0); ++k) m.step(m.botChoice());
        uint8_t p = m.currentPlayer();
        Object* me = m.player(p);
        bool no = false, yes = true;
        for (Object* t : m.tiles()) {
            Object* u = env::at<Object*>(t, api.tUnit);
            if (!u || env::at<uint8_t>(u, api.uOwner) != p) continue;
            env::Coord c = env::at<env::Coord>(t, api.tCoords);
            std::printf("unit at %d,%d moved %d attacked %d\n", c.x, c.y, env::at<bool>(u, api.uMoved), env::at<bool>(u, api.uAttacked));
            for (bool inc : {no, yes}) {
                Object* l = call(api.unitActions, nullptr, {m.state(), me, t, &inc});
                for (int i = 0, n = env::listCount(l); i < n; ++i) {
                    Object* cmd = env::listAt<Object*>(l, i);
                    Json j;
                    Reflect::command(j, cmd);
                    std::printf("  includeUnavailable=%d valid=%d %s\n", inc, m.valid(cmd), j.s.c_str());
                }
            }
        }
        std::printf("legal:\n");
        for (size_t i = 0; i < m.legal.size(); ++i) { Json j; Reflect::command(j, m.legalCmds[i].get()); std::printf("  %s\n", j.s.c_str()); }
        std::string a = m.save();
        env::Match copy(api);
        copy.load(spec, a);
        std::string b = copy.save();
        size_t d = 0;
        while (d < std::min(a.size(), b.size()) && a[d] == b[d]) ++d;
        std::printf("save/load/save: %zu vs %zu bytes, first difference at %zu\n", a.size(), b.size(), d);
        for (size_t i = d > 16 ? d - 16 : 0; i < std::min(a.size(), d + 48); ++i) std::printf("%02x%s", (unsigned char)a[i], i + 1 == d ? "|" : " ");
        std::printf("\n");
        for (size_t i = d > 16 ? d - 16 : 0; i < std::min(b.size(), d + 48); ++i) std::printf("%02x%s", (unsigned char)b[i], i + 1 == d ? "|" : " ");
        std::printf("\n");
        Dumper da, db;
        da.object(m.state(), 0);
        db.object(copy.state(), 0);
        std::ofstream("/data/local/tmp/poly/clone_a.json") << da.j.s;
        std::ofstream("/data/local/tmp/poly/clone_b.json") << db.j.s;
        std::printf("legal %zu vs %zu\n", m.legal.size(), copy.legal.size());
    } else if (cmd == "env") {
        wire::Server server;
        server.run(argc > 2 ? std::atoi(argv[2]) : 8200);
    } else if (cmd == "envtest") {
        envtest::Stats st = envtest::run(argc > 2 ? std::atoi(argv[2]) : 2, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1,
                                         argc > 4 ? std::atoi(argv[4]) : 25, argc > 5 ? std::atoi(argv[5]) : 60);
        return st.sweepMismatch || st.botMissing || st.duplicates || st.cloneMismatch || st.strictMismatch ? 1 : 0;
    } else if (cmd == "envbench") envBench(argc > 2 ? std::atoi(argv[2]) : 4);
    else if (cmd == "fogtest")
        return envtest::fogTest(argc > 2 ? std::atoi(argv[2]) : 4, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1,
                                argc > 4 ? std::atoi(argv[4]) : 20);
    else die("unknown command " + cmd);
    return 0;
}
